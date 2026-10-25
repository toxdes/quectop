#include "linux.h"
#include ".build/assets.h"
#define TIERS 5
#define TEMP_BIAS 2048
#define HEADER_BYTES 64
#define LIVE_HEADER_BYTES 144
#define PENDING_BYTES 1024
#define QUECTOP_VERSION "0.1.0"
#define QUECTOP_SCHEMA_VERSION 7
#define QUECTOP_DISK_SCHEMA_VERSION 2
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
#define HISTORY_SUFFIX ".v" STRINGIFY(QUECTOP_SCHEMA_VERSION) ".bin"
#define LIVE_FILE "live" HISTORY_SUFFIX
#define DISKS_FILE "disks.v" STRINGIFY(QUECTOP_DISK_SCHEMA_VERSION) ".bin"

static const u32 spans[TIERS] = {3600, 86400, 604800, 2592000, 7776000};
static const char *const filenames[TIERS] = {
    "1h" HISTORY_SUFFIX, "1d" HISTORY_SUFFIX, "7d" HISTORY_SUFFIX,
    "30d" HISTORY_SUFFIX, "90d" HISTORY_SUFFIX};
struct values {
  u16 *small;
  u64 *net;
};
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
struct disk {
  long fd;
  u64 identity;
  char *source, *mount;
  u8 valid;
};
struct sample {
  struct values value;
  u32 sec;
  u8 *valid;
};
struct accumulator {
  u64 *sum;
  struct values low, high;
  u32 *count, *peak, start;
};
struct tier {
  struct accumulator active;
  long fd;
  u32 width, record_bytes;
  u8 offset_bits;
};
static struct tier tiers[TIERS];
static struct sample latest;
static char input[4096];
static u8 *arena, *wire, *live, *id_wire, *pending, *cpu_state;
static u32 *cpu_ids;
static struct net_state *networks;
static struct frequency *frequencies;
static struct disk *disks;
static void *disk_mapping;
static unsigned long disk_capacity, disk_mapping_bytes;
static u32 interface_count, frequency_count, frequency_bits, metadata_bytes,
    value_bits, disk_count, disk_slots;
static u64 *previous_core;
static unsigned long arena_used;
static u32 core_count, metrics, small_count, id_bits, id_bytes, latest_bytes,
    header_bytes;
static u32 overall_index, user_index, system_index, memory_index, swap_index,
    temp_index, frequency_index;
static u32 max_record, live_capacity, pending_capacity, pending_count,
    pending_first, pending_last, committed[TIERS];
static long directory_fd, live_directory_fd, cpu_fd, mem_fd, net_fd, temp_fd,
    mounts_fd;
static const char *interface, *disk_path;
static const char *services_option;
static char **service_names, *service_strings;
static u32 service_count, service_string_bytes;
static u32 interval = 5000, publish = 30000, points = 1000,
           disk_refresh_ms = 600000;
static u64 previous_cpu[8], last_mono, memory_kib, swap_kib, last_disk,
    disks_generated_ms;
static int aggregate_valid;

/* A freestanding compiler can still emit calls here for struct copies. */
void *memcpy(void *dst, const void *src, unsigned long n) {
  volatile u8 *d = dst;
  const u8 *s = src;
  while (n--)
    *d++ = *s++;
  return dst;
}
void *memset(void *dst, int value, unsigned long n) {
  volatile u8 *d = dst;
  while (n--)
    *d++ = (u8)value;
  return dst;
}
static unsigned long length(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (unsigned long)(p - s);
}
static int equal(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static void finish(int code) __attribute__((noreturn));
static void finish(int code) {
  call4(N_exit, code, 0, 0, 0);
  __builtin_unreachable();
}
static void write_all(long fd, const void *data, unsigned long bytes) {
  const char *p = data;
  while (bytes) {
    long n = call4(N_write, fd, (long)p, bytes, 0);
    if (n == -4)
      continue;
    if (n <= 0) {
      const char m[] = "quectop: write failed\n";
      call4(N_write, 2, (long)m, sizeof(m) - 1, 0);
      finish(1);
    }
    p += n;
    bytes -= (unsigned long)n;
  }
}
static void fail(const char *s) __attribute__((noreturn));
static void fail(const char *s) {
  write_all(2, "quectop: ", 7);
  write_all(2, s, length(s));
  write_all(2, "\n", 1);
  finish(1);
}
static void check(long result, const char *s) {
  if (result < 0)
    fail(s);
}
#if defined(__arm__)
static void check_mapping(long result, const char *s) {
  if ((unsigned long)result >= (unsigned long)-4095)
    fail(s);
}
#else
#define check_mapping check
#endif
static int number(const char **p, u64 *value) {
  u64 n = 0;
  while (**p == ' ' || **p == '\t')
    ++*p;
  if (**p < '0' || **p > '9')
    return 0;
  while (**p >= '0' && **p <= '9') {
    u32 d = (u32)(*(*p)++ - '0');
    if (n > (~0ULL - d) / 10)
      return 0;
    n = n * 10 + d;
  }
  *value = n;
  return 1;
}
static u32 option_number(const char *s, u32 min, u32 max) {
  u64 n;
  if (!number(&s, &n) || *s || n < min || n > max)
    fail("numeric option out of range");
  return (u32)n;
}
static int prefix(const char *s, const char *key) {
  while (*key)
    if (*s++ != *key++)
      return 0;
  return 1;
}
static char *read_proc(long fd) {
  if (fd < 0)
    return 0;
  long n;
  do {
    n = file_io(N_pread, fd, input, sizeof(input) - 1, 0);
  } while (n == -4);
  if (n <= 0 || (n == (long)sizeof(input) - 1 && fd != cpu_fd))
    return 0;
  input[n] = 0;
  return input;
}
static void positioned(long fd, void *data, unsigned long bytes, u64 offset,
                       int writing) {
  char *p = data;
  while (bytes) {
    long n =
        file_io(writing ? N_pwrite : N_pread, fd, p, bytes, offset);
    if (n == -4)
      continue;
    if (n <= 0)
      fail(writing ? "history write failed" : "history header unreadable");
    bytes -= (unsigned long)n;
    offset += (u64)n;
    p += n;
  }
}
static u32 hash_more(u32 h, const void *data, unsigned long bytes) {
  const u8 *p = data;
  while (bytes--)
    h = (h ^ *p++) * 16777619U;
  return h;
}
static u32 hash(const void *data, unsigned long bytes) {
  return hash_more(2166136261U, data, bytes);
}
/* Wire integers use fixed byte offsets and little endian, never native structs. */
static void store_le16(u8 *dest, u16 value) {
  dest[0] = (u8)value;
  dest[1] = (u8)(value >> 8);
}
static void store_le32(u8 *dest, u32 value) {
  dest[0] = (u8)value;
  dest[1] = (u8)(value >> 8);
  dest[2] = (u8)(value >> 16);
  dest[3] = (u8)(value >> 24);
}
static void store_le64(u8 *dest, u64 value) {
  store_le32(dest, (u32)value);
  store_le32(dest + 4, (u32)(value >> 32));
}
static u32 load_le32(const u8 *source) {
  return (u32)source[0] | (u32)source[1] << 8 | (u32)source[2] << 16 |
         (u32)source[3] << 24;
}
static void install_assets(void) {
  static const char revision[] = QUECTOP_ASSETS_REVISION;
  static const char *const names[] = {"style.css", "app.js", "index.html"};
  static const char *const temporary[] = {
      ".style.css.tmp", ".app.js.tmp", ".index.html.tmp"};
  long fd = openat(directory_fd, "assets.revision", RDONLY | NOFOLLOW, 0);
  int current = 0;
  if (fd >= 0) {
    long n;
    do {
      n = call4(N_read, fd, (long)input, sizeof(revision), 0);
    } while (n == -4);
    call4(N_close, fd, 0, 0, 0);
    current = n == sizeof(revision) - 1;
    for (u32 i = 0; current && i < sizeof(revision) - 1; ++i)
      current = input[i] == revision[i];
    /* Recover deleted assets too, without reading their contents. */
    for (u32 i = 0; current && i < 3; ++i) {
      fd = openat(directory_fd, names[i], RDONLY | NOFOLLOW, 0);
      current = fd >= 0;
      if (fd >= 0)
        call4(N_close, fd, 0, 0, 0);
    }
  }
  if (current)
    return;
  long source = openat(AT_CWD, "/proc/self/exe", RDONLY, 0);
  check(source, "cannot open executable for dashboard installation");
  i64 end = seek_file(source, 0, 2);
  u64 total = (u64)asset_sizes[0] + asset_sizes[1] + asset_sizes[2];
  if (end < 16 || (u64)end - 16 < total)
    fail("dashboard payload missing from executable");
  u8 footer[16];
  positioned(source, footer, sizeof(footer), (u64)end - 16, 0);
  static const char magic[] = "QUECTOPA";
  for (u32 i = 0; i < 8; ++i)
    if (footer[i] != magic[i])
      fail("invalid dashboard payload");
  if (((u64)load_le32(footer + 12) << 32 | load_le32(footer + 8)) != total)
    fail("invalid dashboard payload size");
  u64 offset = (u64)end - 16 - total;
  for (u32 i = 0; i < 3; ++i) {
    fd = openat(directory_fd, temporary[i],
                WRONLY | CREATE | TRUNC | NOFOLLOW, 0644);
    check(fd, "cannot create dashboard asset");
    u32 remaining = asset_sizes[i];
    while (remaining) {
      u32 bytes = remaining < sizeof(input) ? remaining : sizeof(input);
      positioned(source, input, bytes, offset, 0);
      write_all(fd, input, bytes);
      offset += bytes;
      remaining -= bytes;
    }
    check(call4(N_close, fd, 0, 0, 0), "dashboard asset close failed");
  }
  check(call4(N_close, source, 0, 0, 0), "dashboard payload close failed");
  /* Publish the HTML last, then commit the revision for restart recovery. */
  for (u32 i = 0; i < 3; ++i)
    check(call4(N_rename, directory_fd, (long)temporary[i], directory_fd,
                (long)names[i]),
          "dashboard asset rename failed");
  fd = openat(directory_fd, ".assets.revision.tmp",
              WRONLY | CREATE | TRUNC | NOFOLLOW, 0644);
  check(fd, "cannot create dashboard revision");
  write_all(fd, revision, sizeof(revision) - 1);
  check(call4(N_close, fd, 0, 0, 0), "dashboard revision close failed");
  check(call4(N_rename, directory_fd, (long)".assets.revision.tmp", directory_fd,
              (long)"assets.revision"),
        "dashboard revision rename failed");
}
static u32 bits_needed(u64 n) {
  u32 bits = 0;
  do {
    ++bits;
    n >>= 1;
  } while (n);
  return bits;
}
static void put(u8 *dest, u32 *position, u64 value, u32 bits) {
  while (bits) {
    u32 shift = *position & 7, n = 8 - shift;
    if (n > bits)
      n = bits;
    dest[*position >> 3] |= (u8)((value & ((1U << n) - 1)) << shift);
    value >>= n;
    bits -= n;
    *position += n;
  }
}
static u64 value_get(const struct values *v, int k) {
  return k < (int)small_count ? v->small[k] : v->net[k - small_count];
}
static void value_set(struct values *v, int k, u64 n) {
  if (k < (int)small_count)
    v->small[k] = (u16)n;
  else
    v->net[k - small_count] = n;
}
static u16 percentage(u64 used, u64 total) {
  if (used > total || !total)
    fail("invalid percentage counters");
  return (u16)((used * 1000 + total / 2) / total);
}
/* Internal network unit: 0.001 B/s. Wire: mantissa(10), decimals(2), SI
 * unit(2). */
static u16 network_pack(u64 milli) {
  u64 factor = 1000;
  u32 unit = 0;
  while (unit < 3 && milli / factor >= 1000) {
    factor *= 1000;
    ++unit;
  }
  for (;;) {
    u64 integer = milli / factor;
    u32 decimals = integer >= 100 ? 0 : integer >= 10 ? 1 : 2;
    u32 scale = decimals == 2 ? 100 : decimals == 1 ? 10 : 1;
    u64 divisor = factor / scale;
    u64 mantissa = milli / divisor + (milli % divisor >= (divisor + 1) / 2);
    if (mantissa <= 999)
      return (u16)(mantissa | (decimals << 10) | (unit << 12));
    if (decimals) {
      --decimals;
      scale = decimals ? 10 : 1;
      divisor = factor / scale;
      mantissa = milli / divisor + (milli % divisor >= (divisor + 1) / 2);
      if (mantissa <= 999)
        return (u16)(mantissa | (decimals << 10) | (unit << 12));
    }
    if (unit == 3)
      fail("network rate exceeds 999 GB/s format limit");
    factor *= 1000;
    ++unit;
  }
}
static u32 value_width(u32 k) {
  return k < temp_index    ? 10
         : k == temp_index ? 12
         : k < small_count ? frequency_bits
                           : 14;
}
static int is_valid(u32 k) { return (latest.valid[k >> 3] >> (k & 7)) & 1; }
static void set_valid(u32 k) { latest.valid[k >> 3] |= (u8)(1U << (k & 7)); }
static void pack_values(u8 *dest, u32 *position, const struct values *v) {
  for (u32 k = 0; k < metrics; ++k)
    put(dest, position,
        k < small_count ? value_get(v, k) : network_pack(value_get(v, k)),
        value_width(k));
}
static void encode_bucket(const struct tier *tier, u8 *dest) {
  const struct accumulator *a = &tier->active;
  memset(dest, 0, tier->record_bytes);
  u32 position = 0;
  put(dest, &position, a->start, 32);
  for (u32 k = 0; k < metrics; ++k)
    put(dest, &position, a->count[k] != 0, 1);
  for (u32 k = 0; k < metrics; ++k) {
    u64 n = a->count[k] ? a->sum[k] / a->count[k] + (a->sum[k] % a->count[k] >=
                                                     (a->count[k] + 1ULL) / 2)
                        : 0;
    put(dest, &position, k < small_count ? n : network_pack(n), value_width(k));
  }
  pack_values(dest, &position, &a->low);
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
  } else {
    positioned(t->fd, wire, t->record_bytes,
               header_bytes + (u64)slot * t->record_bytes, 1);
    committed[i] = t->active.start;
  }
}
static void reset_accumulator(struct accumulator *a) {
  memset(a->sum, 0, metrics * sizeof(*a->sum));
  memset(a->count, 0, metrics * sizeof(*a->count));
  memset(a->peak, 0, metrics * sizeof(*a->peak));
  memset(a->low.small, 0, small_count * sizeof(u16));
  memset(a->high.small, 0, small_count * sizeof(u16));
  memset(a->low.net, 0, (unsigned long)interface_count * 2 * sizeof(u64));
  memset(a->high.net, 0, (unsigned long)interface_count * 2 * sizeof(u64));
  a->start = 0;
}
static void update_tiers(void) {
  for (u32 i = 0; i < TIERS; ++i) {
    struct tier *t = &tiers[i];
    struct accumulator *a = &t->active;
    u32 start = latest.sec / t->width * t->width;
    if (a->start && start != a->start) {
      store_bucket(i);
      reset_accumulator(a);
    }
    a->start = start;
    for (u32 k = 0; k < metrics; ++k)
      if (is_valid(k)) {
        u64 v = value_get(&latest.value, k);
        if (!a->count[k] || v < value_get(&a->low, k))
          value_set(&a->low, k, v);
        if (!a->count[k] || v > value_get(&a->high, k)) {
          value_set(&a->high, k, v);
          a->peak[k] = latest.sec - start;
        }
        if (a->sum[k] > ~0ULL - v || a->count[k] == 0xffffffffU)
          fail("aggregation counter overflow");
        a->sum[k] += v;
        ++a->count[k];
      }
  }
}
/* /proc/stat can be much larger than our fixed scratch buffer. Stop at the
 * first non-CPU row. */
static u32 cpu_at, cpu_end;
static int mounts_truncated;
static long stream_fd;
static void stream_rewind(long fd) {
  stream_fd = fd;
  if (seek_file(fd, 0, 0) < 0)
    fail("cannot rewind proc input");
  cpu_at = cpu_end = 0;
}
static const char *stream_line(void) {
  for (;;) {
    if (stream_fd == cpu_fd && cpu_end - cpu_at >= 4 &&
        !prefix(input + cpu_at, "cpu"))
      return 0;
    for (u32 i = cpu_at; i < cpu_end; ++i)
      if (input[i] == '\n') {
        input[i] = 0;
        const char *line = input + cpu_at;
        cpu_at = i + 1;
        return line;
      }
    u32 left = cpu_end - cpu_at;
    if (left >= sizeof(input) - 1) {
      if (stream_fd == cpu_fd)
        fail("CPU row exceeds 4095-byte scratch limit");
      if (stream_fd == mounts_fd)
        mounts_truncated = 1;
      /* Ignore an oversized proc row without growing the scratch buffer. */
      int found = 0;
      while (!found) {
        long n;
        do {
          n = call4(N_read, stream_fd, (long)input, sizeof(input) - 1, 0);
        } while (n == -4);
        if (n < 0)
          fail("cannot read proc input");
        if (!n)
          return 0;
        cpu_end = (u32)n;
        for (cpu_at = 0; cpu_at < cpu_end; ++cpu_at)
          if (input[cpu_at] == '\n') {
            ++cpu_at;
            found = 1;
            break;
          }
      }
      continue;
    }
    for (u32 i = 0; i < left; ++i)
      input[i] = input[cpu_at + i];
    cpu_at = 0;
    cpu_end = left;
    long n;
    do {
      n = call4(N_read, stream_fd, (long)(input + left),
                sizeof(input) - 1 - left, 0);
    } while (n == -4);
    if (n < 0)
      fail("cannot read proc input");
    if (!n) {
      if (left)
        fail("truncated proc row");
      return 0;
    }
    cpu_end += (u32)n;
    input[cpu_end] = 0;
  }
}
/* All proc streams share input[]; sysfs scalar reads use a small local buffer.
 */
static int scalar_path(const char *path, u64 *value) {
  long fd = openat(AT_CWD, path, RDONLY, 0);
  if (fd < 0)
    return 0;
  char buf[64];
  long n;
  do {
    n = call4(N_read, fd, (long)buf, sizeof(buf) - 1, 0);
  } while (n == -4);
  call4(N_close, fd, 0, 0, 0);
  if (n <= 0)
    return 0;
  buf[n] = 0;
  const char *p = buf;
  return number(&p, value);
}
static char *decimal(char *p, u32 value) {
  char digits[10];
  u32 n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  while (n)
    *p++ = digits[--n];
  return p;
}
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
  return interface ? equal(interface, name) : !equal(name, "lo");
}
static u32 discover_networks(int populate) {
  stream_rewind(net_fd);
  const char *p;
  u32 n = 0, bytes = 0;
  while ((p = stream_line())) {
    char name[16];
    if (!network_name(&p, name))
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
static char *field_token(char **line) {
  char *p = *line;
  while (*p == ' ' || *p == '\t')
    ++p;
  if (!*p)
    return 0;
  char *start = p;
  while (*p && *p != ' ' && *p != '\t')
    ++p;
  if (*p)
    *p++ = 0;
  *line = p;
  return start;
}
static void unescape_mount(char *s) {
  char *dest = s;
  while (*s) {
    if (*s == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' &&
        s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
      *dest++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + s[3] - '0');
      s += 4;
    } else
      *dest++ = *s++;
  }
  *dest = 0;
}
static int local_mount(char *line, char **source, char **mount) {
  *source = field_token(&line);
  *mount = field_token(&line);
  char *type = field_token(&line);
  if (!*source || !*mount || !type ||
      (!prefix(*source, "/dev/") && !equal(type, "zfs")))
    return 0;
  unescape_mount(*source);
  unescape_mount(*mount);
  return !disk_path || equal(disk_path, *mount);
}
static unsigned long measure_disks(u32 *slots) {
  *slots = 0;
  unsigned long bytes = 0;
  stream_rewind(mounts_fd);
  const char *line;
  while ((line = stream_line())) {
    char *source, *mount;
    if (local_mount((char *)line, &source, &mount)) {
      bytes += sizeof(struct disk) + length(source) + length(mount) + 2;
      if (++*slots > 65535)
        fail("too many mounted filesystems");
    }
  }
  return bytes;
}
static int mounts_retry;
static void discover_disks(void) {
  u32 slots;
  mounts_truncated = 0;
  unsigned long wanted = measure_disks(&slots);
  for (u32 i = 0; i < disk_count; ++i)
    if (disks[i].fd >= 0)
      call4(N_close, disks[i].fd, 0, 0, 0);
  disk_count = 0;
  if (wanted > disk_capacity) {
    long mapped = call6(N_mmap, 0, (long)wanted, 3, 0x22, -1, 0);
    check_mapping(mapped, "cannot resize mounted-filesystem storage");
    if (disk_mapping)
      call4(N_unmap, (long)disk_mapping, (long)disk_mapping_bytes, 0, 0);
    disks = disk_mapping = (void *)mapped;
    disk_capacity = disk_mapping_bytes = wanted;
  }
  disk_slots = slots;
  char *names = (char *)(disks + slots), *end = (char *)disks + disk_capacity;
  stream_rewind(mounts_fd);
  const char *line;
  mounts_retry = 0;
  while ((line = stream_line())) {
    char *source, *mount;
    if (!local_mount((char *)line, &source, &mount))
      continue;
    unsigned long source_bytes = length(source) + 1,
                  mount_bytes = length(mount) + 1;
    if (disk_count == slots ||
        (unsigned long)(end - names) < source_bytes + mount_bytes) {
      mounts_retry = 1;
      continue;
    }
    long fd = openat(AT_CWD, mount, RDONLY | DIRECTORY | NOFOLLOW, 0);
    struct fsinfo fs;
    int ok = fd >= 0 && filesystem_info(fd, &fs) >= 0;
    u64 identity = ok ? (u64)(u32)fs.fsid[0] | ((u64)(u32)fs.fsid[1] << 32) : 0;
    int duplicate = 0;
    for (u32 i = 0; i < disk_count; ++i)
      if ((identity && disks[i].identity == identity) ||
          equal(disks[i].source, source))
        duplicate = 1;
    if (duplicate) {
      if (fd >= 0)
        call4(N_close, fd, 0, 0, 0);
      continue;
    }
    struct disk *d = &disks[disk_count++];
    memset(d, 0, sizeof(*d));
    d->fd = fd;
    d->identity = identity;
    d->source = names;
    memcpy(names, source, source_bytes);
    names += source_bytes;
    d->mount = names;
    memcpy(names, mount, mount_bytes);
    names += mount_bytes;
  }
}
static int mounts_changed(void) {
  struct {
    int fd;
    short events, revents;
  } descriptor = {(int)mounts_fd, 2, 0};
  struct timespec zero = {0, 0};
  long result = call6(N_ppoll, (long)&descriptor, 1, (long)&zero, 0, 8, 0);
  return mounts_retry || result < 0 || (descriptor.revents & 10);
}
static void publish_disks(void) {
  long fd = openat(live_directory_fd, "." DISKS_FILE ".tmp",
                   WRONLY | CREATE | TRUNC | NOFOLLOW, 0644);
  check(fd, "cannot create filesystem snapshot");
  disks_generated_ms = clock_ns(0) / 1000000;
  u8 h[28] = {0};
  memcpy(h, "QUECTOPD", 8);
  store_le32(h + 8, QUECTOP_DISK_SCHEMA_VERSION);
  store_le64(h + 12, disks_generated_ms);
  store_le32(h + 20, disk_count);
  store_le32(h + 24, (u32)mounts_retry | ((u32)mounts_truncated << 1));
  write_all(fd, h, sizeof(h));
  u32 checksum = hash(h, sizeof(h));
  for (u32 i = 0; i < disk_count; ++i) {
    struct disk *d = &disks[i];
    if (d->fd < 0)
      d->fd = openat(AT_CWD, d->mount, RDONLY | DIRECTORY | NOFOLLOW, 0);
    struct fsinfo fs;
    d->valid = d->fd >= 0 && filesystem_info(d->fd, &fs) >= 0 &&
               fs.blocks && fs.bfree <= fs.blocks && fs.bavail <= fs.bfree &&
               fs.bsize > 0 && fs.blocks <= ~0ULL / (u64)fs.bsize;
    u8 record[29];
    memset(record, 0, sizeof(record));
    u32 position = 0;
    put(record, &position, length(d->source), 16);
    put(record, &position, length(d->mount), 16);
    u64 values[3] = {0, 0, 0};
    if (d->valid) {
      values[0] = fs.blocks * (u64)fs.bsize / 1024;
      values[1] = (fs.blocks - fs.bfree) * (u64)fs.bsize / 1024;
      values[2] = fs.bavail * (u64)fs.bsize / 1024;
    }
    u32 bits = bits_needed(values[0]);
    put(record, &position, d->valid | ((bits - 1) << 1), 8);
    for (u32 k = 0; k < 3; ++k)
      put(record, &position, values[k], bits);
    u32 record_bytes = (position + 7) / 8;
    write_all(fd, record, record_bytes);
    write_all(fd, d->source, length(d->source));
    write_all(fd, d->mount, length(d->mount));
    checksum = hash_more(hash_more(hash_more(checksum, record, record_bytes),
                                   d->source, length(d->source)),
                         d->mount, length(d->mount));
  }
  u8 footer[4];
  store_le32(footer, checksum);
  write_all(fd, footer, sizeof(footer));
  check(call4(N_close, fd, 0, 0, 0), "filesystem snapshot close failed");
  check(call4(N_rename, live_directory_fd, (long)"." DISKS_FILE ".tmp",
              live_directory_fd, (long)DISKS_FILE),
        "filesystem snapshot rename failed");
}

#include "services.h"

static void *reserve(unsigned long bytes) {
  arena_used = (arena_used + 7) & ~7UL;
  unsigned long at = arena_used;
  arena_used += bytes;
  return arena ? arena + at : 0;
}
static void memory_layout(void) {
  cpu_ids = reserve((unsigned long)core_count * sizeof(*cpu_ids));
  previous_core =
      reserve((unsigned long)core_count * 3 * sizeof(*previous_core));
  cpu_state = reserve(core_count);
  networks = reserve((unsigned long)interface_count * sizeof(*networks));
  frequencies = reserve((unsigned long)frequency_count * sizeof(*frequencies));
  latest.value.small = reserve((unsigned long)small_count * sizeof(u16));
  latest.value.net = reserve((unsigned long)interface_count * 2 * sizeof(u64));
  latest.valid = reserve((metrics + 7) / 8);
  id_wire = reserve(metadata_bytes);
  wire = reserve(max_record);
  live = reserve(live_capacity);
  pending = reserve((unsigned long)pending_capacity * tiers[0].record_bytes);
  for (u32 i = 0; i < TIERS; ++i) {
    struct accumulator *a = &tiers[i].active;
    a->sum = reserve((unsigned long)metrics * sizeof(*a->sum));
    a->count = reserve((unsigned long)metrics * sizeof(*a->count));
    a->peak = reserve((unsigned long)metrics * sizeof(*a->peak));
    a->low.small = reserve((unsigned long)small_count * sizeof(u16));
    a->high.small = reserve((unsigned long)small_count * sizeof(u16));
    a->low.net = reserve((unsigned long)interface_count * 2 * sizeof(u64));
    a->high.net = reserve((unsigned long)interface_count * 2 * sizeof(u64));
  }
  disks = reserve(disk_capacity);
  if (service_count) {
    service_names = reserve((unsigned long)service_count * sizeof(*service_names));
    service_strings = reserve(service_string_bytes);
  }
}
static void initialize_cpus(void) {
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
  metrics = core_count + 6 + frequency_count + 2 * interface_count;
  small_count = core_count + 6 + frequency_count;
  overall_index = core_count;
  user_index = core_count + 1;
  system_index = core_count + 2;
  memory_index = core_count + 3;
  swap_index = core_count + 4;
  temp_index = core_count + 5;
  frequency_index = core_count + 6;
  value_bits = 10 * (core_count + 5) + 12 + frequency_count * frequency_bits +
               28 * interface_count;
  latest_bytes = (metrics + value_bits + 7) / 8;
  live_capacity = LIVE_HEADER_BYTES + metadata_bytes + latest_bytes + 4;
  disk_capacity = measure_disks(&disk_slots);
  for (u32 i = 0; i < TIERS; ++i) {
    struct tier *t = &tiers[i];
    t->width = (spans[i] + points - 2) / (points - 1);
    if (i == 0 && t->width < (interval + 999) / 1000)
      t->width = (interval + 999) / 1000;
    t->offset_bits = (u8)bits_needed(t->width - 1);
    t->record_bytes =
        (32 + metrics + 3 * value_bits + metrics * t->offset_bits + 7) / 8 + 4;
    if (t->record_bytes > max_record)
      max_record = t->record_bytes;
    live_capacity += t->record_bytes;
  }
  pending_capacity = PENDING_BYTES / tiers[0].record_bytes;
  if (!pending_capacity)
    pending_capacity = 1;
  live_capacity += pending_capacity * tiers[0].record_bytes;
  /* Name, flags and up to two lossless unsigned 64-bit base-128 varints. */
  live_capacity += service_string_bytes + service_count * 21;
  memory_layout();
  unsigned long size = arena_used;
  long mapped = call6(N_mmap, 0, (long)size, 3, 0x22, -1, 0);
  check_mapping(mapped, "cannot allocate CPU working storage");
  arena = (u8 *)mapped;
  arena_used = 0;
  memory_layout();
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
    cpu_state[i] &= 1;
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
        set_valid(overall_index);
        set_valid(user_index);
        set_valid(system_index);
      }
      memcpy(previous_cpu, current, sizeof(current));
      aggregate_valid = 1;
    } else {
      u64 *old = previous_core + (unsigned long)slot * 3;
      if ((cpu_state[slot] & 1) && total >= old[0] && current[3] >= old[1]) {
        u64 elapsed =
            total - old[0] + (old[2] > current[4] ? old[2] - current[4] : 0);
        u64 idle = current[3] - old[1] +
                   (current[4] >= old[2] ? current[4] - old[2] : 0);
        if (elapsed && idle <= elapsed) {
          latest.value.small[slot] = percentage(elapsed - idle, elapsed);
          set_valid(slot);
        }
      }
      old[0] = total;
      old[1] = current[3];
      old[2] = current[4];
      cpu_state[slot] |= 2;
      ++slot;
    }
  }
  if (!aggregate)
    fail("aggregate CPU counters missing");
  for (u32 i = 0; i < core_count; ++i)
    cpu_state[i] = (cpu_state[i] & 2) ? 1 : 0;
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
  if (!last_disk || mono - last_disk >= (u64)disk_refresh_ms * 1000000) {
    publish_disks();
    last_disk = mono;
  }
  p = read_proc(temp_fd);
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
  for (u32 i = 0; i < frequency_count; ++i) {
    p = read_proc(frequencies[i].fd);
    u64 khz;
    if (p && number(&p, &khz) && khz <= 65535000ULL) {
      u32 mhz = (u32)((khz + 500) / 1000);
      if (mhz >= (1U << frequency_bits))
        fail("CPU frequency exceeds packed width; use a new data directory");
      latest.value.small[frequency_index + i] = (u16)mhz;
      set_valid(frequency_index + i);
    }
  }
  for (u32 i = 0; i < interface_count; ++i)
    networks[i].state &= 1;
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
    if ((net->state & 1) && elapsed && values[0] >= net->previous[0] &&
        values[8] >= net->previous[1]) {
      latest.value.net[2 * i] =
          network_rate(values[0], net->previous[0], elapsed);
      latest.value.net[2 * i + 1] =
          network_rate(values[8], net->previous[1], elapsed);
      set_valid(small_count + 2 * i);
      set_valid(small_count + 2 * i + 1);
    }
    net->previous[0] = values[0];
    net->previous[1] = values[8];
    net->state |= 2;
  }
  for (u32 i = 0; i < interface_count; ++i)
    networks[i].state = (networks[i].state & 2) ? 1 : 0;
  last_mono = mono;
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
  bytes += encode_services(live + bytes);
  store_le32(live + bytes, hash(live, bytes));
  bytes += 4;
  long fd = openat(live_directory_fd, "." LIVE_FILE ".tmp",
                   WRONLY | CREATE | TRUNC | NOFOLLOW, 0644);
  check(fd, "cannot create live snapshot");
  write_all(fd, live, bytes);
  check(call4(N_close, fd, 0, 0, 0), "live snapshot close failed");
  check(call4(N_rename, live_directory_fd, (long)"." LIVE_FILE ".tmp",
              live_directory_fd, (long)LIVE_FILE),
        "live snapshot rename failed");
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
    } else {
      check(truncate_file(t->fd, wanted), "cannot size history ring");
      positioned(t->fd, expected, sizeof(expected), 0, 1);
      positioned(t->fd, id_wire, metadata_bytes, HEADER_BYTES, 1);
    }
  }
}
static void help(void) {
  const char s[] =
      "quectop " QUECTOP_VERSION
      ": Linux "
#if defined(__arm__)
      "ARMv6"
#else
      "ARM64/x86-64"
#endif
      " collector, no libc\nUsage: quectop "
      "[options]\n"
      "  --live-dir DIR   live file directory; prefer RAM storage (default "
      "output)\n"
      "  --data-dir DIR   existing data directory (required)\n"
      "  --output, -o DIR aliases for --data-dir\n"
      "  --interval-ms N  sample period, 100..3600000 (default 5000)\n"
      "  --publish-ms N   live publication, 100..3600000 (default 30000)\n"
      "  --points N       buckets per range, 16..10000 (default 1000)\n"
      "  --disk-refresh-ms N  capacity check period, 1000..3600000 (default "
      "600000)\n"
      "  --disk PATH      restrict capacities to this mounted filesystem\n"
      "  --thermal PATH   millidegree sensor (default thermal_zone0/temp)\n"
      "  --interface IF   restrict tracking (default all except loopback)\n"
      "  --services LIST comma-separated system services (default none)\n"
      "  --version, -v    show version\n"
      "  --help           show help\n"
      "Writes " LIVE_FILE ", " DISKS_FILE " and 1h/1d/7d/30d/90d" HISTORY_SUFFIX
      ". Installs matching dashboard assets at startup. HTTP serving is "
      "external.\n"
      "No fsync; active summaries and recent writes can be lost on "
      "interruption.\n"
      "A changed interval or point count requires a new output directory.\n";
  write_all(1, s, sizeof(s) - 1);
}
void entry(long *stack) __attribute__((noreturn));
void entry(long *stack) {
  int argc = (int)stack[0];
  char **argv = (char **)(stack + 1);
  const char *directory = 0, *live_directory = 0,
             *thermal = "/sys/class/thermal/thermal_zone0/temp";
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (equal(arg, "--help")) {
      help();
      finish(0);
    }
    if (equal(arg, "--version") || equal(arg, "-v")) {
      static const char version[] = "quectop " QUECTOP_VERSION "\n";
      write_all(1, version, sizeof(version) - 1);
      finish(0);
    }
    if (++i == argc)
      fail("option requires a value; see --help");
    const char *v = argv[i];
    if (equal(arg, "--interval-ms"))
      interval = option_number(v, 100, 3600000);
    else if (equal(arg, "--publish-ms"))
      publish = option_number(v, 100, 3600000);
    else if (equal(arg, "--disk-refresh-ms"))
      disk_refresh_ms = option_number(v, 1000, 3600000);
    else if (equal(arg, "--points"))
      points = option_number(v, 16, 10000);
    else if (equal(arg, "--data-dir") || equal(arg, "-o") ||
             equal(arg, "--output"))
      directory = v;
    else if (equal(arg, "--live-dir"))
      live_directory = v;
    else if (equal(arg, "--disk"))
      disk_path = v;
    else if (equal(arg, "--thermal"))
      thermal = v;
    else if (equal(arg, "--interface"))
      interface = v;
    else if (equal(arg, "--services")) {
      if (services_option)
        fail("use one --services option with comma-separated names");
      services_option = v;
    }
    else
      fail("unknown option; see --help");
  }
  if (!directory)
    fail("data directory required; use --data-dir DIR");
  measure_services();
#if defined(__arm__)
  if (!clock_ns(1))
    fail("ARM32 requires Linux 5.1+ with time64 syscalls enabled");
#endif
  directory_fd = openat(AT_CWD, directory, RDONLY | DIRECTORY, 0);
  check(directory_fd, "cannot open output directory");
  check(call4(N_flock, directory_fd, 6, 0, 0),
        "another collector owns this output directory");
  live_directory_fd = directory_fd;
  if (live_directory && !equal(live_directory, directory)) {
    live_directory_fd = openat(AT_CWD, live_directory, RDONLY | DIRECTORY, 0);
    check(live_directory_fd, "cannot open live directory");
    check(call4(N_flock, live_directory_fd, 6, 0, 0),
          "another collector owns live directory");
  }
  install_assets();
  cpu_fd = openat(AT_CWD, "/proc/stat", RDONLY, 0);
  check(cpu_fd, "cannot open /proc/stat");
  net_fd = openat(AT_CWD, "/proc/net/dev", RDONLY, 0);
  check(net_fd, "cannot open /proc/net/dev");
  mounts_fd = openat(AT_CWD, "/proc/self/mounts", RDONLY, 0);
  check(mounts_fd, "cannot open mount list");
  initialize_cpus();
  initialize_services();
  open_history();
  mem_fd = openat(AT_CWD, "/proc/meminfo", RDONLY, 0);
  temp_fd = openat(AT_CWD, thermal, RDONLY, 0);
  check(cpu_fd, "cannot open /proc/stat");
  check(mem_fd, "cannot open /proc/meminfo");
  u64 next = clock_ns(1), next_publish = next,
      next_flush = next + 60000000000ULL;
  for (;;) {
    collect();
    update_tiers();
    u64 now = clock_ns(1);
    if (now >= next_flush) {
      flush_pending();
      next_flush = now + 60000000000ULL;
    }
    if (now >= next_publish) {
      if (mounts_changed()) {
        discover_disks();
        publish_disks();
        last_disk = clock_ns(7);
      }
      publish_live();
      next_publish = now + (u64)publish * 1000000;
    }
    next += (u64)interval * 1000000;
    now = clock_ns(1);
    if (next <= now)
      next += ((now - next) / ((u64)interval * 1000000) + 1) *
              ((u64)interval * 1000000);
    sleep_until(next);
  }
}
