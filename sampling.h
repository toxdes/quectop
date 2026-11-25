/* Device discovery, counter baselines, and resource sampling. */
enum counter_state { COUNTER_BASELINE = 1, COUNTER_SEEN = 2 };
struct cpu_counters {
  u64 total, idle, iowait;
};
_Static_assert(sizeof(struct cpu_counters) == 3 * sizeof(u64),
               "CPU baseline storage size");
struct net_state {
  u64 previous[2];
  char name[16];
  u8 state;
};
struct frequency {
  long fd;
  u32 cpu;
  u8 hardware;
};
static u32 *cpu_ids;
static u8 *cpu_state;
static struct cpu_counters *previous_core;
static struct net_state *networks;
static struct frequency *frequencies;
static u32 interface_count, frequency_count;
static const char *interface;
static u64 previous_cpu[8], previous_pressure[3], last_mono, memory_kib,
    swap_kib;
static int aggregate_valid;
static u8 pressure_seen;

static void frequency_path(char *dest, u32 cpu, const char *name) {
  const char base[] = "/sys/devices/system/cpu/cpu";
  memcpy(dest, base, sizeof(base) - 1);
  char *p = decimal(dest + sizeof(base) - 1, cpu);
  const char mid[] = "/cpufreq/";
  memcpy(p, mid, sizeof(mid) - 1);
  p += sizeof(mid) - 1;
  memcpy(p, name, length(name) + 1);
}
static int frequency_probe(u32 cpu, struct frequency *out, u32 *maximum) {
  char path[128];
  u64 first, khz;
  frequency_path(path, cpu, "affected_cpus");
  if (scalar_path(path, &first) && first != cpu)
    return 0; /* Read each shared-clock policy only once. */
  frequency_path(path, cpu, "cpuinfo_max_freq");
  if (!scalar_path(path, &khz) || !khz || khz > 65535000ULL)
    return 0;
  u32 mhz = (u32)((khz + 999) / 1000);
  if (mhz > *maximum)
    *maximum = mhz;
  if (out) {
    out->cpu = cpu;
    out->hardware = 1;
    frequency_path(path, cpu, "cpuinfo_cur_freq");
    out->fd = openat(AT_CWD, path, RDONLY, 0);
    if (out->fd < 0) {
      out->hardware = 0;
      frequency_path(path, cpu, "scaling_cur_freq");
      out->fd = openat(AT_CWD, path, RDONLY, 0);
    }
  }
  return 1;
}
static int network_name(const char **line, char name[16]) {
  const char *p = *line;
  while (*p == ' ' || *p == '\t')
    ++p;
  u32 n = 0;
  while (*p && *p != ':' && n < 15)
    name[n++] = *p++;
  if (*p != ':' || !n)
    return 0;
  name[n] = 0;
  *line = p + 1;
  return 1;
}
/* Default: device-backed interfaces only, skipping bridges, veth and tunnels. */
static int tracked_interface(const char *name) {
  if (interface)
    return equal(interface, name);
  char path[48];
  const char base[] = "/sys/class/net/", leaf[] = "/device";
  unsigned long n = length(name);
  memcpy(path, base, sizeof(base) - 1);
  memcpy(path + sizeof(base) - 1, name, n);
  memcpy(path + sizeof(base) - 1 + n, leaf, sizeof(leaf));
  long fd = openat(AT_CWD, path, RDONLY | DIRECTORY, 0);
  if (fd < 0)
    return 0;
  call4(N_close, fd, 0, 0, 0);
  return 1;
}
static u32 discover_networks(int populate) {
  stream_rewind(net_fd);
  const char *p;
  u32 n = 0, bytes = 0;
  while ((p = stream_line())) {
    char name[16];
    if (!network_name(&p, name) || !tracked_interface(name))
      continue;
    if (populate) {
      if (n == interface_count)
        fail("network topology changed during startup");
      memcpy(networks[n].name, name, length(name) + 1);
    }
    bytes += 1 + (u32)length(name);
    if (++n > 65535)
      fail("too many network interfaces");
  }
  if (populate && n != interface_count)
    fail("network topology changed during startup");
  if (!populate)
    interface_count = n;
  return bytes;
}

static u64 network_rate(u64 current, u64 old, u64 elapsed_ms) {
  u64 delta = current - old;
  if (delta / elapsed_ms > ~0ULL / 1000000ULL)
    fail("network counter overflow");
  return delta / elapsed_ms * 1000000ULL +
         delta % elapsed_ms * 1000000ULL / elapsed_ms;
}
static void collect_cpu(void) {
  stream_rewind(cpu_fd);
  const char *p;
  u32 slot = 0;
  int aggregate = 0;
  for (u32 i = 0; i < core_count; ++i)
    cpu_state[i] &= COUNTER_BASELINE;
  while ((p = stream_line())) {
    int whole = prefix(p, "cpu ");
    if (whole) {
      p += 4;
      aggregate = 1;
    } else {
      p += 3;
      u64 id;
      if (!number(&p, &id))
        fail("invalid CPU ID");
      while (slot < core_count && cpu_ids[slot] < id)
        ++slot;
      if (slot == core_count || cpu_ids[slot] != id)
        fail("CPU added; restart with a new output directory");
    }
    u64 current[8], total = 0;
    for (u32 k = 0; k < 8; ++k) {
      if (!number(&p, &current[k]))
        fail("invalid CPU counters");
      total += current[k];
    }
    if (whole) {
      u64 delta[8], elapsed = 0;
      int valid = aggregate_valid;
      for (u32 k = 0; k < 8; ++k) {
        if (current[k] < previous_cpu[k] && k != 4)
          valid = 0;
        delta[k] =
            current[k] >= previous_cpu[k] ? current[k] - previous_cpu[k] : 0;
        elapsed += delta[k];
      }
      if (valid && elapsed) {
        latest.value.small[overall_index] =
            percentage(elapsed - delta[3] - delta[4], elapsed);
        latest.value.small[user_index] =
            percentage(delta[0] + delta[1], elapsed);
        latest.value.small[system_index] =
            percentage(delta[2] + delta[5] + delta[6], elapsed);
        latest.value.small[iowait_index] = percentage(delta[4], elapsed);
        set_valid(overall_index);
        set_valid(user_index);
        set_valid(system_index);
        set_valid(iowait_index);
      }
      memcpy(previous_cpu, current, sizeof(current));
      aggregate_valid = 1;
    } else {
      struct cpu_counters *old = &previous_core[slot];
      if ((cpu_state[slot] & COUNTER_BASELINE) && total >= old->total &&
          current[3] >= old->idle) {
        u64 elapsed = total - old->total +
                      (old->iowait > current[4] ? old->iowait - current[4] : 0);
        u64 idle = current[3] - old->idle +
                   (current[4] >= old->iowait ? current[4] - old->iowait : 0);
        if (elapsed && idle <= elapsed) {
          latest.value.small[slot] = percentage(elapsed - idle, elapsed);
          set_valid(slot);
        }
      }
      old->total = total;
      old->idle = current[3];
      old->iowait = current[4];
      cpu_state[slot] |= COUNTER_SEEN;
      ++slot;
    }
  }
  if (!aggregate)
    fail("aggregate CPU counters missing");
  for (u32 i = 0; i < core_count; ++i)
    cpu_state[i] = (cpu_state[i] & COUNTER_SEEN) ? COUNTER_BASELINE : 0;
}
static void collect_memory(void) {
  const char *p = read_proc(mem_fd);
  u64 available = 0, memory = 0, swap_total = 0, swap_free = 0;
  int swap_found = 0, swap_free_found = 0;
  int found = 0;
  while (p && *p) {
    if (prefix(p, "MemTotal:")) {
      const char *v = p + 9;
      number(&v, &memory);
    }
    if (prefix(p, "MemAvailable:")) {
      const char *v = p + 13;
      found = number(&v, &available);
    }
    if (prefix(p, "SwapTotal:")) {
      const char *v = p + 10;
      swap_found = number(&v, &swap_total);
    }
    if (prefix(p, "SwapFree:")) {
      const char *v = p + 9;
      swap_free_found = number(&v, &swap_free);
    }
    while (*p && *p != '\n')
      ++p;
    if (*p)
      ++p;
  }
  if (!memory || !found || available > memory)
    fail("MemTotal/MemAvailable unavailable");
  memory_kib = memory;
  latest.value.small[memory_index] = percentage(memory - available, memory);
  set_valid(memory_index);
  swap_kib = swap_found ? swap_total : 0;
  if (swap_found && swap_free_found && swap_total && swap_free <= swap_total) {
    latest.value.small[swap_index] =
        percentage(swap_total - swap_free, swap_total);
    set_valid(swap_index);
  }
}

/* PSI "some" line: cumulative microseconds at least one task stalled. */
static int pressure_total(const char *p, u64 *total) {
  if (!prefix(p, "some "))
    return 0;
  for (; *p && *p != '\n'; ++p)
    if (prefix(p, "total=")) {
      p += 6;
      return number(&p, total);
    }
  return 0;
}
static void collect_pressure(u64 elapsed_us) {
  for (u32 i = 0; i < 3; ++i) {
    const char *p = read_proc(pressure_fd[i]);
    u64 total;
    if (!p || !pressure_total(p, &total)) {
      pressure_seen &= (u8)~(1U << i);
      continue;
    }
    if ((pressure_seen >> i & 1) && elapsed_us &&
        total >= previous_pressure[i]) {
      u64 stalled = total - previous_pressure[i];
      latest.value.small[pressure_index + i] =
          percentage(stalled < elapsed_us ? stalled : elapsed_us, elapsed_us);
      set_valid(pressure_index + i);
    }
    previous_pressure[i] = total;
    pressure_seen |= (u8)(1U << i);
  }
}

static void collect_temperature(void) {
  const char *p = read_proc(temp_fd);
  if (p) {
    int negative = *p == '-';
    if (negative)
      ++p;
    u64 n;
    if (number(&p, &n) && n <= 204700) {
      int tenths = (int)((n + 50) / 100);
      latest.value.small[temp_index] =
          (u16)(TEMP_BIAS + (negative ? -tenths : tenths));
      set_valid(temp_index);
    }
  }
}

static void collect_frequencies(void) {
  for (u32 i = 0; i < frequency_count; ++i) {
    const char *p = read_proc(frequencies[i].fd);
    u64 khz;
    if (p && number(&p, &khz) && khz <= 65535000ULL) {
      u32 mhz = (u32)((khz + 500) / 1000);
      if (mhz >= (1U << frequency_bits))
        fail("CPU frequency exceeds packed width; use a new data directory");
      latest.value.small[frequency_index + i] = (u16)mhz;
      set_valid(frequency_index + i);
    }
  }
}

static void collect_networks(u64 elapsed) {
  const char *p;
  for (u32 i = 0; i < interface_count; ++i)
    networks[i].state &= COUNTER_BASELINE;
  stream_rewind(net_fd);
  while ((p = stream_line())) {
    char name[16];
    if (!network_name(&p, name))
      continue;
    u32 i;
    for (i = 0; i < interface_count && !equal(networks[i].name, name); ++i) {
    }
    if (i == interface_count)
      continue; /* New interfaces require a new history layout. */
    u64 values[16];
    int ok = 1;
    for (u32 k = 0; k < 16; ++k)
      if (!number(&p, &values[k])) {
        ok = 0;
        break;
      }
    if (!ok)
      continue;
    struct net_state *net = &networks[i];
    if ((net->state & COUNTER_BASELINE) && elapsed &&
        values[0] >= net->previous[0] && values[8] >= net->previous[1]) {
      latest.value.net[2 * i] =
          network_rate(values[0], net->previous[0], elapsed);
      latest.value.net[2 * i + 1] =
          network_rate(values[8], net->previous[1], elapsed);
      set_valid(small_count + 2 * i);
      set_valid(small_count + 2 * i + 1);
    }
    net->previous[0] = values[0];
    net->previous[1] = values[8];
    net->state |= COUNTER_SEEN;
  }
  for (u32 i = 0; i < interface_count; ++i)
    networks[i].state =
        (networks[i].state & COUNTER_SEEN) ? COUNTER_BASELINE : 0;
}

static void collect(void) {
  memset(latest.valid, 0, (metrics + 7) / 8);
  memset(latest.value.small, 0, small_count * sizeof(u16));
  memset(latest.value.net, 0, (unsigned long)interface_count * 2 * sizeof(u64));
  u64 now = clock_ns(0) / 1000000000ULL;
  if (now > 0xffffffffULL)
    fail("timestamp exceeds schema timestamp range");
  latest.sec = (u32)now;
  u64 mono = clock_ns(7),
      elapsed = mono > last_mono ? (mono - last_mono) / 1000000 : 0;
  collect_cpu();
  collect_memory();
  collect_pressure(mono > last_mono && last_mono ? (mono - last_mono) / 1000 : 0);
  refresh_disks(mono);
  collect_temperature();
  collect_frequencies();
  collect_networks(elapsed);
  last_mono = mono;
}
