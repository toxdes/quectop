/* Measure the device layout, allocate one arena, then populate it. */
static u8 *arena_base;
static unsigned long arena_used;
static void *reserve(unsigned long bytes) {
  arena_used = (arena_used + 7) & ~7UL;
  unsigned long at = arena_used;
  arena_used += bytes;
  return arena_base ? arena_base + at : 0;
}
static void memory_layout(void) {
  cpu_ids = reserve((unsigned long)core_count * sizeof(*cpu_ids));
  previous_core =
      reserve((unsigned long)core_count * sizeof(*previous_core));
  cpu_state = reserve(core_count);
  networks = reserve((unsigned long)interface_count * sizeof(*networks));
  frequencies =
      reserve((unsigned long)frequency_count * sizeof(*frequencies));
  latest.value.small = reserve((unsigned long)small_count * sizeof(u16));
  latest.value.net =
      reserve((unsigned long)interface_count * 2 * sizeof(u64));
  latest.valid = reserve((metrics + 7) / 8);
  id_wire = reserve(metadata_bytes);
  wire = reserve(max_record);
  live = reserve(live_capacity);
  pending =
      reserve((unsigned long)pending_capacity * tiers[0].record_bytes);
  for (u32 i = 0; i < TIERS; ++i) {
    struct accumulator *a = &tiers[i].active;
    a->sum = reserve((unsigned long)metrics * sizeof(*a->sum));
    a->count = reserve((unsigned long)metrics * sizeof(*a->count));
    a->peak = reserve((unsigned long)metrics * sizeof(*a->peak));
    a->low = reserve((unsigned long)low_count * sizeof(u16));
    a->high.small = reserve((unsigned long)small_count * sizeof(u16));
    a->high.net =
        reserve((unsigned long)interface_count * 2 * sizeof(u64));
  }
  disks = reserve(disk_capacity);
  if (service_count) {
    service_names =
        reserve((unsigned long)service_count * sizeof(*service_names));
    service_strings = reserve(service_string_bytes);
  }
}
static void initialize_topology(void) {
  /* Measure: count CPUs, frequency policies, interfaces and disks. */
  stream_rewind(cpu_fd);
  const char *p;
  u64 last = 0;
  int seen = 0;
  u32 maximum_mhz = 0;
  while ((p = stream_line())) {
    if (prefix(p, "cpu "))
      continue;
    p += 3;
    u64 id;
    if (!number(&p, &id) || id > 0xffffffffU || (seen && id <= last))
      fail("invalid CPU IDs");
    last = id;
    seen = 1;
    if (frequency_probe((u32)id, 0, &maximum_mhz) && ++frequency_count > 65535)
      fail("too many CPU frequency policies");
    if (++core_count > 1048576)
      fail("CPU count exceeds format limits");
  }
  if (!core_count)
    fail("no online CPUs found");
  id_bits = bits_needed((u32)last);
  frequency_bits = bits_needed(maximum_mhz);
  u32 name_bytes = discover_networks(0);
  id_bytes = (core_count * id_bits + frequency_count * (id_bits + 1) + 7) / 8;
  metadata_bytes = id_bytes + name_bytes;
  header_bytes = HEADER_BYTES + metadata_bytes;
  metrics = core_count + 10 + frequency_count + 2 * interface_count;
  small_count = core_count + 10 + frequency_count;
  overall_index = core_count;
  user_index = core_count + 1;
  system_index = core_count + 2;
  iowait_index = core_count + 3;
  pressure_index = core_count + 4;
  memory_index = core_count + 7;
  swap_index = core_count + 8;
  temp_index = core_count + 9;
  frequency_index = core_count + 10;
  value_bits = 10 * (core_count + 9) + 12 + frequency_count * frequency_bits +
               28 * interface_count;
  low_count = small_count - memory_index;
  low_bits = 20 + 12 + frequency_count * frequency_bits;
  latest_bytes = (metrics + value_bits + 7) / 8;
  live_capacity = LIVE_HEADER_BYTES + metadata_bytes + latest_bytes + 4;
  disk_capacity = measure_disks(&disk_slots);
  for (u32 i = 0; i < TIERS; ++i) {
    struct tier *t = &tiers[i];
    t->width = (spans[i] + points - 2) / (points - 1);
    if (i == 0 && t->width < (interval + 999) / 1000)
      t->width = (interval + 999) / 1000;
    t->offset_bits = (u8)bits_needed(t->width - 1);
    t->record_bytes = (32 + metrics + 2 * value_bits + low_bits +
                       metrics * t->offset_bits + 7) /
                          8 +
                      4;
    if (t->record_bytes > max_record)
      max_record = t->record_bytes;
    live_capacity += t->record_bytes;
  }
  pending_capacity = PENDING_BYTES / tiers[0].record_bytes;
  if (!pending_capacity)
    pending_capacity = 1;
  live_capacity += pending_capacity * tiers[0].record_bytes;
  /* Flags, four u64 varints, detail mask and four 17-bit packed values.
   * Names already include one byte each for their wire length in
   * string_bytes. */
  live_capacity += 1 + sizeof(host_name) + service_string_bytes +
                   service_count * SERVICE_ENTRY_OVERHEAD;
  /* Allocate: lay out once to size the arena, map it, lay out again. */
  memory_layout();
  unsigned long size = arena_used;
  long mapped = call6(N_mmap, 0, (long)size, 3, 0x22, -1, 0);
  check_mapping(mapped, "cannot allocate CPU working storage");
  arena_base = (u8 *)mapped;
  arena_used = 0;
  memory_layout();
  /* Populate: fill the arena; fail if devices changed since measuring. */
  stream_rewind(cpu_fd);
  u32 n = 0, position = 0;
  while ((p = stream_line())) {
    if (prefix(p, "cpu "))
      continue;
    p += 3;
    u64 id;
    if (n == core_count || !number(&p, &id) || id > 0xffffffffU ||
        id >= (1ULL << id_bits) || (n && id <= cpu_ids[n - 1]))
      fail("CPU topology changed during startup");
    cpu_ids[n++] = (u32)id;
    put(id_wire, &position, id, id_bits);
  }
  if (n != core_count)
    fail("CPU topology changed during startup");
  u32 f = 0, maximum = 0;
  position = core_count * id_bits;
  for (u32 i = 0; i < core_count; ++i) {
    struct frequency candidate;
    if (!frequency_probe(cpu_ids[i], &candidate, &maximum))
      continue;
    if (f == frequency_count || maximum >= (1U << frequency_bits))
      fail("CPU frequency topology changed during startup");
    frequencies[f++] = candidate;
    put(id_wire, &position, candidate.cpu, id_bits);
    put(id_wire, &position, candidate.hardware, 1);
  }
  if (f != frequency_count || discover_networks(1) != name_bytes)
    fail("device topology changed during startup");
  u32 at = id_bytes;
  for (u32 i = 0; i < interface_count; ++i) {
    u32 bytes = (u32)length(networks[i].name);
    id_wire[at++] = (u8)bytes;
    memcpy(id_wire + at, networks[i].name, bytes);
    at += bytes;
  }
  discover_disks();
}
