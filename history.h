/* History rings, tier aggregation, and atomic live snapshots. */

struct accumulator {
  u64 *sum; /* followed by one u32 carry per network metric */
  u16 *low; /* low_count entries from memory_index */
  struct values high;
  u32 *count, *peak, start;
};
struct tier {
  struct accumulator active;
  long fd;
  u32 width, record_bytes;
  u8 offset_bits;
};
static struct tier tiers[TIERS];
static u8 *wire, *live, *pending;
/* Identifies the device in live snapshots; read once at startup. */
static char host_name[64];
static u8 host_name_bytes;
static u32 max_record, live_capacity, pending_capacity, pending_count,
    pending_first, pending_last, committed[TIERS];
/* Newest window start stored per tier, from the rings at startup. */
static u32 newest[TIERS];

static inline u32 *network_carries(const struct accumulator *a) {
  return (u32 *)(a->sum + metrics);
}

static inline unsigned long sum_bytes(void) {
  return (unsigned long)metrics * sizeof(u64) + (metrics - small_count) * sizeof(u32);
}

static u64 rounded_mean(u64 sum, u32 carry, u32 count) {
  u64 prefix = 0;
  if (carry) {
    /* Long division by count in 32-bit limbs. carry < count. */
    u64 top = (u64)carry << 32 | (sum >> 32);
    prefix = (top / count) << 32;
    sum = (top % count) << 32 | (u32)sum;
  }
  return prefix + sum / count + (sum % count >= (count + 1ULL) / 2);
}

static void encode_bucket(const struct tier *tier, u8 *dest) {
  const struct accumulator *a = &tier->active;
  memset(dest, 0, tier->record_bytes);
  u32 position = 0;
  put(dest, &position, a->start, 32);
  for (u32 k = 0; k < metrics; ++k)
    put(dest, &position, a->count[k] != 0, 1);
  for (u32 k = 0; k < metrics; ++k) {
    u32 carry = k < small_count ? 0 : network_carries(a)[k - small_count];
    u64 n = a->count[k] ? rounded_mean(a->sum[k], carry, a->count[k]) : 0;
    put(dest, &position, k < small_count ? n : network_pack(n), value_width(k));
  }
  for (u32 k = memory_index; k < small_count; ++k)
    put(dest, &position, a->low[k - memory_index], value_width(k));
  pack_values(dest, &position, &a->high);
  for (u32 k = 0; k < metrics; ++k)
    put(dest, &position, a->peak[k], tier->offset_bits);
  store_le32(dest + tier->record_bytes - 4,
             hash(dest, tier->record_bytes - 4));
}
static void flush_pending(void) {
  if (!pending_count)
    return;
  struct tier *t = &tiers[0];
  positioned(t->fd, pending, (unsigned long)pending_count * t->record_bytes,
             header_bytes + (u64)pending_first * t->record_bytes, 1);
  committed[0] = pending_last;
  pending_count = 0;
}
static void store_bucket(u32 i) {
  struct tier *t = &tiers[i];
  u32 slot = (t->active.start / t->width) % (points - 1);
  encode_bucket(t, wire);
  uplink_record(i, wire);
  if (i == 0) {
    if (pending_count && (slot != pending_first + pending_count ||
                          pending_count == pending_capacity))
      flush_pending();
    if (!pending_count)
      pending_first = slot;
    memcpy(pending + (unsigned long)pending_count * t->record_bytes, wire,
           t->record_bytes);
    ++pending_count;
    pending_last = t->active.start;
    newest[0] = t->active.start;
  } else {
    positioned(t->fd, wire, t->record_bytes,
               header_bytes + (u64)slot * t->record_bytes, 1);
    committed[i] = t->active.start;
    newest[i] = t->active.start;
  }
}
/* A window no newer than the newest stored one means the wall clock went
 * back, as after booting without a battery-backed clock. Storing it would
 * overwrite real history, so it is dropped until NTP synchronizes the clock;
 * once synchronized, the clock wins over stored windows dated in the future. */
static int may_store(u32 i, u32 start) {
  return start > newest[i] || clock_synchronized();
}
static void reset_accumulator(struct accumulator *a) {
  memset(a->sum, 0, sum_bytes());
  memset(a->count, 0, metrics * sizeof(*a->count));
  memset(a->peak, 0, metrics * sizeof(*a->peak));
  memset(a->low, 0, low_count * sizeof(u16));
  memset(a->high.small, 0, small_count * sizeof(u16));
  memset(a->high.net, 0, (unsigned long)interface_count * 2 * sizeof(u64));
  a->start = 0;
}
static void update_tiers(void) {
  for (u32 i = 0; i < TIERS; ++i) {
    struct tier *t = &tiers[i];
    struct accumulator *a = &t->active;
    u32 start = latest.sec / t->width * t->width;
    if (a->start && start != a->start) {
      if (may_store(i, a->start))
        store_bucket(i);
      reset_accumulator(a);
    }
    a->start = start;
    for (u32 k = memory_index; k < small_count; ++k) {
      u16 v = latest.value.small[k], *low = &a->low[k - memory_index];
      if (is_valid(k) && (!a->count[k] || v < *low))
        *low = v;
    }
    for (u32 k = 0; k < metrics; ++k)
      if (is_valid(k) && a->count[k] != 0xffffffffU) {
        u64 v = value_get(&latest.value, k);
        if (!a->count[k] || v > value_get(&a->high, k)) {
          value_set(&a->high, k, v);
          a->peak[k] = latest.sec - start;
        }
        /* A u16 metric cannot overflow a u64 sum with a u32 sample count. */
        if ((a->sum[k] += v) < v)
          ++network_carries(a)[k - small_count];
        ++a->count[k];
      }
  }
}

static void publish_live(void) {
  memset(live, 0, live_capacity);
  memcpy(live, "QUECTOPL", 8);
  store_le32(live + 8, QUECTOP_SCHEMA_VERSION);
  store_le32(live + 12, points);
  store_le32(live + 16, interval);
  store_le32(live + 20, publish);
  store_le64(live + 24, clock_ns(0) / 1000000);
  store_le64(live + 32, clock_ns(7) / 1000000);
  store_le64(live + 40, memory_kib);
  store_le32(live + 48, latest.sec);
  store_le32(live + 52, core_count);
  store_le32(live + 56, id_bits);
  store_le32(live + 60, metrics);
  store_le32(live + 104, pending_count);
  store_le32(live + 108, interface_count);
  store_le64(live + 112, swap_kib);
  store_le32(live + 120, frequency_count);
  store_le32(live + 124, frequency_bits);
  store_le32(live + 128, metadata_bytes);
  store_le32(live + 132, service_count);
  store_le64(live + 136, disks_generated_ms);
  for (u32 i = 0; i < TIERS; ++i) {
    store_le32(live + 64 + 4 * i, tiers[i].width);
    store_le32(live + 84 + 4 * i, committed[i]);
  }
  memcpy(live + LIVE_HEADER_BYTES, id_wire, metadata_bytes);
  u32 bytes = LIVE_HEADER_BYTES + metadata_bytes, bit = 0;
  for (u32 k = 0; k < metrics; ++k)
    put(live + bytes, &bit, is_valid(k), 1);
  pack_values(live + bytes, &bit, &latest.value);
  bytes += latest_bytes;
  for (u32 i = 0; i < TIERS; ++i) {
    encode_bucket(&tiers[i], live + bytes);
    bytes += tiers[i].record_bytes;
  }
  memcpy(live + bytes, pending,
         (unsigned long)pending_count * tiers[0].record_bytes);
  bytes += pending_count * tiers[0].record_bytes;
  live[bytes++] = host_name_bytes;
  memcpy(live + bytes, host_name, host_name_bytes);
  bytes += host_name_bytes;
  bytes += encode_services(live + bytes);
  store_le32(live + bytes, hash(live, bytes));
  bytes += 4;
  uplink_live(bytes);
  long fd = temporary_output(live_directory_fd, "." LIVE_FILE ".tmp");
  check(fd, "cannot create live snapshot");
  write_all(fd, live, bytes);
  check(call4(N_close, fd, 0, 0, 0), "live snapshot close failed");
  check(call4(N_rename, live_directory_fd, (long)"." LIVE_FILE ".tmp",
              live_directory_fd, (long)LIVE_FILE),
        "live snapshot rename failed");
}
/* Newest valid window start in a ring, read through live; 0 for none. */
static u32 newest_stored(const struct tier *t) {
  u32 per = live_capacity / t->record_bytes, found = 0;
  for (u32 slot = 0; slot < points - 1; slot += per) {
    u32 n = points - 1 - slot < per ? points - 1 - slot : per;
    positioned(t->fd, live, (unsigned long)n * t->record_bytes,
               header_bytes + (u64)slot * t->record_bytes, 0);
    for (u32 j = 0; j < n; ++j) {
      const u8 *r = live + (unsigned long)j * t->record_bytes;
      u32 start = load_le32(r);
      if (start > found && start / t->width % (points - 1) == slot + j &&
          load_le32(r + t->record_bytes - 4) == hash(r, t->record_bytes - 4))
        found = start;
    }
  }
  return found;
}
/* utsname: six 65-byte fields; nodename is the second. Uses proc scratch. */
static void read_host_name(void) {
  if (call4(N_uname, (long)input, 0, 0, 0) < 0)
    return;
  const char *node = input + 65;
  while (host_name_bytes < sizeof(host_name) && node[host_name_bytes])
    ++host_name_bytes;
  memcpy(host_name, node, host_name_bytes);
}
static void open_history(void) {
  for (int i = 0; i < TIERS; ++i) {
    struct tier *t = &tiers[i];
    u8 expected[HEADER_BYTES] = {0};
    memcpy(expected, "QUECTOPH", 8);
    store_le32(expected + 8, QUECTOP_SCHEMA_VERSION);
    store_le32(expected + 12, points);
    store_le32(expected + 16, spans[i]);
    store_le32(expected + 20, t->width);
    store_le32(expected + 24, interval);
    store_le32(expected + 28, t->record_bytes);
    store_le32(expected + 32, (u32)i);
    store_le32(expected + 36, core_count);
    store_le32(expected + 40, metrics);
    store_le32(expected + 44, header_bytes);
    store_le32(expected + 48, id_bits);
    store_le32(expected + 52, interface_count);
    store_le16(expected + 56, (u16)frequency_count);
    expected[58] = (u8)frequency_bits;
    store_le32(expected + 60,
               hash_more(hash(expected, HEADER_BYTES - 4), id_wire, metadata_bytes));
    t->fd = openat(directory_fd, filenames[i], RDWR | CREATE | NOFOLLOW, 0644);
    check(t->fd, "cannot open history ring");
    i64 size = seek_file(t->fd, 0, 2);
    if (size < 0)
      fail("cannot inspect history ring");
    u64 wanted = header_bytes + (u64)(points - 1) * t->record_bytes;
    if (size) {
      u8 existing[HEADER_BYTES];
      positioned(t->fd, existing, sizeof(existing), 0, 0);
      for (unsigned long j = 0; j < sizeof(expected) - 4; ++j)
        if (expected[j] != existing[j])
          fail(
              "history format/configuration differs; use a new data directory");
      positioned(t->fd, live, metadata_bytes, HEADER_BYTES, 0);
      if (load_le32(existing + 60) !=
          hash_more(hash(existing, HEADER_BYTES - 4), live, metadata_bytes))
        fail("damaged history header; use a new data directory");
      for (u32 j = 0; j < metadata_bytes; ++j)
        if (live[j] != id_wire[j])
          fail("device metadata differs; use a new data directory");
      if ((u64)size != wanted)
        fail("history ring has unexpected size");
      newest[i] = newest_stored(t);
    } else {
      check(truncate_file(t->fd, wanted), "cannot size history ring");
      positioned(t->fd, expected, sizeof(expected), 0, 1);
      positioned(t->fd, id_wire, metadata_bytes, HEADER_BYTES, 1);
    }
  }
}
