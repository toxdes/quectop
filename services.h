/* Opt-in system service snapshots. No helpers, libdbus or subscriptions.
 * D-Bus framing reuses input only after proc/mount parsing has finished.
 * Each publication has one 100 ms deadline, including connection setup.
 * Protocol: https://dbus.freedesktop.org/doc/dbus-specification.html
 * Properties: org.freedesktop.systemd1.Unit and .Service. */
enum service_backend { SERVICE_NONE, SERVICE_SYSTEMD, SERVICE_OPENRC };
enum service_status {
  SERVICE_UNAVAILABLE = 0,
  SERVICE_INACTIVE = 1,
  SERVICE_ACTIVE = 2,
  SERVICE_FAILED = 3,
  SERVICE_ACTIVATING = 4,
  SERVICE_STOPPING = 5,
  SERVICE_RELOADING = 6,
  SERVICE_UNKNOWN = 7
};
enum service_flag {
  SERVICE_STATUS_MASK = 7,
  SERVICE_HAS_MEMORY = 8,
  SERVICE_HAS_UPTIME = 16,
  SERVICE_HAS_MAPPED = 32,
  SERVICE_IS_OPENRC = 64,
  SERVICE_PROCESS_AGE = 128
};
enum service_metric {
  SERVICE_RSS,
  SERVICE_PSS,
  SERVICE_USS,
  SERVICE_ANON,
  SERVICE_FILE,
  SERVICE_SHMEM,
  SERVICE_MAPPED_COUNT
};
#define SERVICE_MAPPED_ALL ((1U << SERVICE_MAPPED_COUNT) - 1)
#define SERVICE_ENTRY_OVERHEAD (1 + 4 * 10 + 1 + (4 * 17 + 7) / 8)
static enum service_backend service_backend;
static const char *services_option;
static char **service_names, *service_strings;
static u32 service_count, service_string_bytes;
static long service_openrc = -1, service_proc = -1, service_cgroups = -1;
static unsigned long service_clock_ticks;
static u32 service_first;
static u64 service_deadline;

struct service_observation {
  u64 memory, uptime;
  u64 mapped[SERVICE_MAPPED_COUNT]; /* KiB, indexed by service_metric. */
  u8 mapped_valid;
  u8 flags;
};

static int service_exists(long directory, const char *name) {
  if (clock_ns(1) >= service_deadline)
    return 0;
  long fd = openat(directory, name, PATH_ONLY | NOFOLLOW, 0);
  if (fd < 0)
    return 0;
  call4(N_close, fd, 0, 0, 0);
  return 1;
}

static void service_detect(char **env) {
  long fd = openat(AT_CWD, "/run/systemd/system", RDONLY | DIRECTORY, 0);
  if (fd >= 0) {
    service_backend = SERVICE_SYSTEMD;
    call4(N_close, fd, 0, 0, 0);
  } else {
    service_openrc = openat(AT_CWD, "/run/openrc", RDONLY | DIRECTORY, 0);
    if (service_openrc >= 0)
      service_backend = SERVICE_OPENRC;
  }
  service_proc = openat(AT_CWD, "/proc", RDONLY | DIRECTORY, 0);
  service_cgroups = openat(AT_CWD, "/sys/fs/cgroup", RDONLY | DIRECTORY, 0);
  while (*env)
    ++env;
  for (unsigned long *aux = (unsigned long *)(env + 1); aux[0]; aux += 2)
    if (aux[0] == 17) /* AT_CLKTCK, independent of kernel CONFIG_HZ. */
      service_clock_ticks = aux[1];
}

static int service_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}
static u32 service_name_size(const char *s, u32 n) {
  if (!n)
    fail("--services contains an empty name");
  for (u32 i = 0; i < n; ++i) {
    char c = s[i];
    if (c == '\\') {
      if (i + 3 >= n || s[i + 1] != 'x' || !service_hex(s[i + 2]) ||
          !service_hex(s[i + 3]))
        fail("invalid escape in service name");
      i += 3;
    } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '.' ||
                 c == '@' || c == '-'))
      fail("invalid character in service name");
  }
  u32 bytes = n;
  static const char suffix[] = ".service";
  int match = n >= 8;
  for (u32 i = 0; match && i < 8; ++i)
    match = s[n - 8 + i] == suffix[i];
  if (!match && service_backend == SERVICE_SYSTEMD)
    bytes += 8;
  if (bytes > 255)
    fail("service name exceeds 255 bytes including .service");
  return bytes;
}
/* Length of one --services entry, up to the next comma. */
static inline __attribute__((always_inline)) u32
service_token(const char *s) {
  u32 n = 0;
  while (s[n] && s[n] != ',')
    ++n;
  return n;
}
static void measure_services(char **env) {
  if (!services_option)
    return;
  service_detect(env);
  const char *s = services_option;
  for (;;) {
    u32 n = service_token(s);
    service_string_bytes += service_name_size(s, n) + 1;
    if (++service_count > 65535)
      fail("too many services");
    if (!s[n])
      break;
    s += n + 1;
  }
}
static void initialize_services(void) {
  if (!services_option)
    return;
  const char *s = services_option;
  char *out = service_strings;
  service_count = 0;
  for (;;) {
    u32 n = service_token(s);
    u32 size = service_name_size(s, n);
    memcpy(out, s, n);
    if (size > n)
      memcpy(out + n, ".service", 8);
    out[size] = 0;
    u32 i = 0;
    while (i < service_count && !equal(service_names[i], out))
      ++i;
    if (i == service_count) {
      service_names[service_count++] = out;
      out += size + 1;
    }
    if (!s[n])
      break;
    s += n + 1;
  }
}
#include "service_dbus.h"

static u32 service_varint(u8 *dest, u32 at, u64 value) {
  do {
    u8 byte = (u8)(value & 127);
    value >>= 7;
    dest[at++] = byte | (value ? 128 : 0);
  } while (value);
  return at;
}
static char *service_read(long directory, const char *name) {
  if (directory < 0 || clock_ns(1) >= service_deadline)
    return 0;
  long fd = openat(directory, name, RDONLY | NOFOLLOW | 2048, 0);
  char *result = read_proc(fd);
  if (fd >= 0)
    call4(N_close, fd, 0, 0, 0);
  return result;
}

static int service_counter(long directory, const char *name, u64 *value) {
  const char *s = service_read(directory, name);
  if (!s || !number(&s, value))
    return 0;
  while (*s == ' ' || *s == '\n' || *s == '\t')
    ++s;
  return !*s;
}

static int service_path(char *dest, unsigned long capacity, const char *prefix,
                        const char *name, const char *suffix) {
  const char *parts[] = {prefix, name, suffix};
  unsigned long at = 0;
  for (int i = 0; i < 3; ++i)
    for (const char *p = parts[i]; *p; ++p) {
      if (at + 1 == capacity)
        return 0;
      dest[at++] = *p;
    }
  dest[at] = 0;
  return 1;
}

static long service_openrc_group(const char *name, char *path, u32 capacity,
                                 const char **membership, int *legacy) {
  static const char *const prefixes[] = {"/openrc.", "/unified/openrc.", "/openrc/"};
  static const u8 mount_bytes[] = {0, sizeof("/unified") - 1, sizeof("/openrc") - 1};
  for (u32 i = 0; i < 3; ++i) {
    if (clock_ns(1) >= service_deadline)
      return -1;
    if (!service_path(path, capacity, prefixes[i], name, ""))
      return -1;
    long fd = openat(service_cgroups, path + 1, RDONLY | DIRECTORY | NOFOLLOW, 0);
    if (fd >= 0) {
      *membership = path + mount_bytes[i];
      *legacy = i == 2;
      return fd;
    }
  }
  return -1;
}

#include "service_memory.h"

static void observe_openrc(const char *name, struct service_observation *row) {
  row->flags = SERVICE_IS_OPENRC;
  if (clock_ns(1) >= service_deadline)
    return;
  static const char *const states[] = {"failed/", "stopping/", "starting/",
                                       "inactive/", "started/"};
  static const u8 status[] = {SERVICE_FAILED, SERVICE_STOPPING,
                              SERVICE_ACTIVATING, SERVICE_INACTIVE,
                              SERVICE_ACTIVE};
  char path[300];
  for (u32 i = 0; i < 5; ++i) {
    if (!service_path(path, sizeof(path), states[i], name, ""))
      return;
    if (service_exists(service_openrc, path)) {
      row->flags |= status[i];
      break;
    }
  }
  if (!(row->flags & SERVICE_STATUS_MASK)) {
    if (service_path(path, sizeof(path), "/etc/init.d/", name, "") &&
        service_exists(AT_CWD, path))
      row->flags |= SERVICE_INACTIVE;
  }
  if ((row->flags & SERVICE_STATUS_MASK) == SERVICE_ACTIVE &&
      service_path(path, sizeof(path), "options/", name, "/child_pid")) {
    u64 pid;
    int found = service_counter(service_openrc, path, &pid);
    if (!found && service_path(path, sizeof(path), "daemons/", name, "/001")) {
      const char *line = service_read(service_openrc, path);
      while (line && *line) {
        if (prefix(line, "pidfile=")) {
          line += 8;
          const char *end = line;
          while (*end && *end != '\n')
            ++end;
          unsigned long n = (unsigned long)(end - line);
          if (n && n < sizeof(path) && *line == '/') {
            memcpy(path, line, n);
            path[n] = 0;
            found = service_counter(AT_CWD, path, &pid);
          }
          break;
        }
        while (*line && *line != '\n')
          ++line;
        if (*line)
          ++line;
      }
    }
    if (found && pid && pid <= 0xffffffffU)
      observe_process_uptime((u32)pid, row);
  }
  const char *membership;
  int legacy;
  long group = service_openrc_group(name, path, sizeof(path), &membership, &legacy);
  if (group >= 0) {
    if (service_counter(group, "memory.current", &row->memory) ||
        service_counter(group, "memory.usage_in_bytes", &row->memory))
      row->flags |= SERVICE_HAS_MEMORY;
    observe_service_memory(group, membership, legacy, row);
    call4(N_close, group, 0, 0, 0);
  }
  if (!(row->flags & SERVICE_HAS_MEMORY) &&
      service_path(path, sizeof(path), "memory/openrc_", name,
                   "/memory.usage_in_bytes") &&
      service_counter(service_cgroups, path, &row->memory)) {
    row->flags |= SERVICE_HAS_MEMORY;
  }
}

static void observe_systemd(const char *name, struct service_observation *row) {
  static const char unit_interface[] = "org.freedesktop.systemd1.Unit";
  static const char service_interface[] = "org.freedesktop.systemd1.Service";
  if (service_call("/org/freedesktop/systemd1",
                   "org.freedesktop.systemd1.Manager", "GetUnit", "s", name,
                   0) != 1)
    return;
  char path[800]; /* At most 3 escaped bytes per byte of a 255-byte name. */
  u32 path_bytes;
  const char *unit_path = service_reply_string(service_body, &path_bytes);
  if (!unit_path || path_bytes >= sizeof(path))
    return;
  memcpy(path, unit_path, path_bytes + 1);

  const char *state =
      service_property_string(path, unit_interface, "ActiveState");
  if (state) {
    static const char *const states[] = {
        "",           "inactive",     "active",   "failed",
        "activating", "deactivating", "reloading"};
    row->flags = SERVICE_UNKNOWN;
    for (u32 k = SERVICE_INACTIVE; k <= SERVICE_RELOADING; ++k)
      if (equal(state, states[k]))
        row->flags = (u8)k;
  }
  u64 start;
  if ((row->flags == SERVICE_ACTIVE || row->flags == SERVICE_RELOADING) &&
      service_property_counter(path, unit_interface,
                               "ActiveEnterTimestampMonotonic", &start)) {
    u64 now = clock_ns(1) / 1000;
    if (start && start <= now) {
      row->uptime = (now - start) / 1000000;
      row->flags |= SERVICE_HAS_UPTIME;
    }
  }
  if (service_property_counter(path, service_interface, "MemoryCurrent",
                               &row->memory) &&
      row->memory != ~0ULL)
    row->flags |= SERVICE_HAS_MEMORY;

  const char *group_path =
      service_property_string(path, service_interface, "ControlGroup");
  if (!group_path || group_path[0] != '/' || !group_path[1])
    return;
  long group =
      openat(service_cgroups, group_path + 1, RDONLY | DIRECTORY | NOFOLLOW, 0);
  if (group >= 0) {
    observe_service_memory(group, group_path, 0, row);
    call4(N_close, group, 0, 0, 0);
  }
}

/* Absolute KiB value with four significant digits: mantissa(14), one
 * optional decimal digit, binary unit(2) for KiB/MiB/GiB/TiB. */
static u32 service_pack(u64 kib) {
  u64 factor = 1;
  u32 unit = 0;
  while (unit < 3 && kib / factor >= 10000) {
    factor *= 1024;
    ++unit;
  }
  for (;;) {
    u64 integer = kib / factor, scaled = kib * 10;
    u32 decimals = integer <= 999 ? 1 : 0;
    u64 mantissa = decimals ? scaled / factor + (scaled % factor * 2 >= factor)
                            : integer + ((kib % factor) * 2 >= factor);
    if (mantissa <= 9999)
      return (mantissa << 3) | ((u32)decimals << 2) | unit;
    if (decimals) {
      mantissa = integer + ((kib % factor) * 2 >= factor);
      if (mantissa <= 9999)
        return (mantissa << 3) | unit;
    }
    if (unit == 3)
      fail("service memory exceeds packed format limit");
    factor *= 1024;
    ++unit;
  }
}
static u32 encode_service(u8 *dest, const char *name,
                          struct service_observation *row) {
  u32 n = (u32)length(name), at = 0;
  u8 mask = row->mapped_valid;
  for (u32 i = SERVICE_USS; i < SERVICE_MAPPED_COUNT; ++i)
    if (!(mask & (1U << SERVICE_PSS)) ||
        row->mapped[i] > row->mapped[SERVICE_PSS])
      mask &= (u8) ~(1 << i);
  dest[at++] = (u8)n;
  dest[at++] = row->flags | (mask ? SERVICE_HAS_MAPPED : 0);
  memcpy(dest + at, name, n);
  at += n;
  if (row->flags & SERVICE_HAS_MEMORY)
    at = service_varint(dest, at, row->memory);
  if (row->flags & SERVICE_HAS_UPTIME)
    at = service_varint(dest, at, row->uptime);
  if (mask) {
    dest[at++] = mask;
    for (u32 i = SERVICE_RSS; i <= SERVICE_PSS; ++i)
      if (mask & (1 << i))
        at = service_varint(dest, at, row->mapped[i]);
    u32 bit = 0;
    /* Up to four 17-bit packed values; put() ORs into zeroed bytes. */
    memset(dest + at, 0, 9);
    for (u32 i = SERVICE_USS; i < SERVICE_MAPPED_COUNT; ++i)
      if (mask & (1 << i))
        put(dest + at, &bit, service_pack(row->mapped[i]), 17);
    at += (bit + 7) / 8;
  }
  return at;
}

static u32 encode_services(u8 *dest) {
  if (!service_count)
    return 0;
  service_deadline = clock_ns(1) + 100000000ULL;
  if (service_backend == SERVICE_SYSTEMD)
    service_connect();
  u32 at = 0;
  for (u32 i = 0; i < service_count; ++i) {
    const char *name = service_names[(service_first + i) % service_count];
    struct service_observation row = {0};
    row.flags = service_backend == SERVICE_OPENRC ? SERVICE_IS_OPENRC : 0;
    if (clock_ns(1) < service_deadline) {
      if (service_backend == SERVICE_SYSTEMD)
        observe_systemd(name, &row);
      else if (service_backend == SERVICE_OPENRC)
        observe_openrc(name, &row);
    }
    at += encode_service(dest + at, name, &row);
  }
  service_first = (service_first + 1) % service_count;
  return at;
}
