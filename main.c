#include "linux.h"
#define PROGRAM_NAME "quectop"
#define TEMP_BIAS 2048
#define PENDING_BYTES 1024
#include "protocol.h"

static u32 interval = 5000, publish = 30000, points = 1000;
static long directory_fd, live_directory_fd;
/* uplink.h hooks, called while persisting. */
static void uplink_record(u32 tier, const unsigned char *record);
static void uplink_live(u32 bytes);
static void uplink_disks(void);

#include "runtime.h"
#include "net.h"
#include "proc.h"
#include "metrics.h"
#include "disks.h"
#include "services.h"
#include "sampling.h"
#include "history.h"
#include "lines.h"
#include "resolve.h"
#include "auth.h"
#include "noise.h"
#include "uplink.h"
#include "topology.h"
#include "datadir.h"
#include "startup.h"

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
    else if (equal(arg, "--hub")) {
      for (const char *c = v; *c; ++c)
        if (prefix(c, "://"))
          fail("--hub takes HOST:PORT, not a URL");
      if (!parse_hub(v))
        fail("--hub needs HOST:PORT, HOST being IPv4 or a name");
      hub_state = HUB_IDLE;
    } else if (equal(arg, "--key-file"))
      key_file = v;
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
  measure_services(argv + argc + 1);
#if defined(__arm__)
  if (!clock_ns(1))
    fail("ARM32 requires Linux 5.1+ with time64 syscalls enabled");
#endif
  directory_fd = prepare_data_directory(&directory, argv + argc + 1);
  check(call4(N_flock, directory_fd, 6, 0, 0),
        "another quectop owns this data directory");
  live_directory_fd = directory_fd;
  if (live_directory && !equal(live_directory, directory)) {
    live_directory_fd = openat(AT_CWD, live_directory, RDONLY | DIRECTORY, 0);
    check(live_directory_fd, "cannot open live directory");
    check(call4(N_flock, live_directory_fd, 6, 0, 0),
          "another quectop owns this live directory");
  }
  cpu_fd = openat(AT_CWD, "/proc/stat", RDONLY, 0);
  check(cpu_fd, "cannot open /proc/stat");
  net_fd = openat(AT_CWD, "/proc/net/dev", RDONLY, 0);
  check(net_fd, "cannot open /proc/net/dev");
  mounts_fd = openat(AT_CWD, "/proc/self/mounts", RDONLY, 0);
  check(mounts_fd, "cannot open mount list");
  if (key_file && !hub_state)
    fail("--key-file needs --hub");
  read_host_name();
  initialize_topology();
  initialize_services();
  if (hub_state)
    check_uplink_layout();
  open_history();
  mem_fd = openat(AT_CWD, "/proc/meminfo", RDONLY, 0);
  temp_fd = openat(AT_CWD, thermal, RDONLY, 0);
  static const char *const pressure_paths[3] = {
      "/proc/pressure/cpu", "/proc/pressure/memory", "/proc/pressure/io"};
  for (u32 i = 0; i < 3; ++i)
    pressure_fd[i] = openat(AT_CWD, pressure_paths[i], RDONLY, 0);
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
      rediscover_disks();
      publish_live();
      next_publish = now + (u64)publish * 1000000;
    }
    next += (u64)interval * 1000000;
    now = clock_ns(1);
    if (next <= now)
      next += ((now - next) / ((u64)interval * 1000000) + 1) *
              ((u64)interval * 1000000);
    wait_for_sample(next);
  }
}
