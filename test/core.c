#ifdef QUECTOP_TEST_FREESTANDING
#define entry probe_entry
#endif
/* The wall-clock guard asks the kernel whether NTP has synchronized the
 * clock; tests choose the answer. */
#define QUECTOP_CLOCK_SYNCHRONIZED
static int test_synchronized;
static int clock_synchronized(void) { return test_synchronized; }
#include "../main.c"
#ifdef QUECTOP_TEST_FREESTANDING
#undef entry
#endif

static long fixture_input(const char *name, const char *contents) {
  long fd = temporary_output(directory_fd, name);
  check(fd, "sampling fixture output");
  write_all(fd, contents, length(contents));
  call4(N_close, fd, 0, 0, 0);
  fd = openat(directory_fd, name, RDONLY, 0);
  check(fd, "sampling fixture input");
  return fd;
}

static void check_sampling(void) {
  if (frequency_count)
    frequencies[0].fd = fixture_input(".frequency", "1001\n");
  for (u32 step = 0; step < 4; ++step) {
    mem_fd = fixture_input(
        ".memory",
        step == 0
            ? "MemTotal: 1000 kB\nMemAvailable: 250 kB\nSwapTotal: 500 "
              "kB\nSwapFree: 125 kB\n"
            : "MemTotal: 1000 kB\nMemAvailable: 500 kB\nSwapTotal: 500 kB\n");
    temp_fd =
        fixture_input(".temperature", step == 0 ? "-12345\n" : "unavailable\n");
    char line[128] = {0};
    if (interface_count && step != 2) {
      unsigned long n = length(networks[0].name);
      const char *counters = step == 0
                                 ? ": 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n"
                                 : ": 3000 0 0 0 0 0 0 0 5000 0 0 0 0 0 0 0\n";
      memcpy(line, networks[0].name, n);
      memcpy(line + n, counters, length(counters) + 1);
    }
    net_fd = fixture_input(".network", line);
    /* cpu: 10% stalled; memory: jump beyond the interval caps at 100%;
     * io: a counter that runs backwards is unavailable. Step 2 removes PSI,
     * so step 3 needs a fresh baseline. */
    static const char *const pressure[4][3] = {
        {"some avg10=0.00 avg60=0.00 avg300=0.00 total=1000\nfull avg10=0.00 "
         "avg60=0.00 avg300=0.00 total=7\n",
         "some avg10=0.00 avg60=0.00 avg300=0.00 total=1000\n",
         "some avg10=0.00 avg60=0.00 avg300=0.00 total=9000\n"},
        {"some avg10=9.00 avg60=0.00 avg300=0.00 total=101000\nfull avg10=0.00 "
         "avg60=0.00 avg300=0.00 total=7\n",
         "some avg10=0.00 avg60=0.00 avg300=0.00 total=9000000\n",
         "some avg10=0.00 avg60=0.00 avg300=0.00 total=8000\n"},
        {"", "unavailable\n", "full avg10=0.00 total=1\n"},
        {"some avg10=0.00 avg60=0.00 avg300=0.00 total=200000\n",
         "some avg10=0.00 avg60=0.00 avg300=0.00 total=200000\n",
         "some avg10=0.00 avg60=0.00 avg300=0.00 total=200000\n"}};
    for (u32 i = 0; i < 3; ++i)
      pressure_fd[i] = fixture_input(".pressure", pressure[step][i]);
    last_mono = clock_ns(7) - 1000000000ULL;
    collect();
    if (is_valid(pressure_index) != (step == 1) ||
        (step == 1 && latest.value.small[pressure_index] != 100) ||
        is_valid(pressure_index + 1) != (step == 1) ||
        (step == 1 && latest.value.small[pressure_index + 1] != 1000) ||
        is_valid(pressure_index + 2))
      fail("PSI stall shares, clamping, regression and missing data");
    if (!is_valid(memory_index) || memory_kib != 1000 ||
        latest.value.small[memory_index] != (step == 0 ? 750 : 500))
      fail("memory sampling and replacement");
    if (swap_kib != 500 || is_valid(swap_index) != (step == 0) ||
        (step == 0 && latest.value.small[swap_index] != 750))
      fail("swap sampling and missing free counter");
    if (is_valid(temp_index) != (step == 0) ||
        (step == 0 && latest.value.small[temp_index] != TEMP_BIAS - 123))
      fail("negative temperature and unavailable replacement");
    if (frequency_count && (!is_valid(frequency_index) ||
                            latest.value.small[frequency_index] != 1))
      fail("frequency rounding");
    if (interface_count &&
        (is_valid(small_count) != (step == 1) ||
         is_valid(small_count + 1) != (step == 1) ||
         (step == 1 && (!latest.value.net[0] || !latest.value.net[1]))))
      fail("network baselines, rates, disappearance and return");
  }
}

/* After publication: a wall clock behind stored history must not overwrite it
 * until the clock is synchronized. Runs last; it stores nothing. */
static void check_clock_guard(void) {
  for (u32 i = 1; i < TIERS; ++i)
    if (!committed[i] || newest_stored(&tiers[i]) != committed[i] ||
        newest[i] != committed[i])
      fail("startup scan finds the newest stored window");
  test_synchronized = 0;
  for (u32 i = 0; i < TIERS; ++i)
    if (may_store(i, newest[i]) || may_store(i, newest[i] - tiers[i].width) ||
        !may_store(i, newest[i] + tiers[i].width))
      fail("unsynchronized clock behind stored history");
  /* The clock steps back a day; open windows are discarded first so that
   * every window closed below is dated before stored history. */
  for (u32 i = 0; i < TIERS; ++i)
    tiers[i].active.start = 0;
  u32 back = newest[TIERS - 1] - 86400, before[TIERS],
      pending_before = pending_count;
  memcpy(before, committed, sizeof(before));
  for (u32 step = 0; step < 4; ++step) {
    latest.sec = back + step * tiers[TIERS - 1].width;
    update_tiers();
  }
  for (u32 i = 0; i < TIERS; ++i)
    if (before[i] != committed[i] || pending_count != pending_before)
      fail("windows dated before stored history were stored");
  test_synchronized = 1;
  if (!may_store(0, newest[0] - tiers[0].width))
    fail("synchronized clock stores");
}

/* Real CPU discovery/parsing and packed ring I/O, with deterministic inputs. */
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  directory_fd = openat(AT_CWD, argv[1], RDONLY | DIRECTORY, 0);
  check(directory_fd, "test directory");
  live_directory_fd = directory_fd;
  cpu_fd = openat(AT_CWD, argv[2], RDONLY, 0);
  check(cpu_fd, "CPU fixture");
  net_fd = openat(AT_CWD, "/proc/net/dev", RDONLY, 0);
  mounts_fd = openat(AT_CWD, "/proc/self/mounts", RDONLY, 0);
  check(net_fd, "network discovery input");
  check(mounts_fd, "mount discovery input");
  read_host_name();
  initialize_topology();
  open_history();
  collect_cpu();
  if (is_valid(overall_index))
    fail("first CPU sample must be unavailable");
  cpu_fd = openat(AT_CWD, argv[3], RDONLY, 0);
  check(cpu_fd, "second CPU fixture");
  collect_cpu();
  u64 even = (core_count + 1) / 2, odd = core_count / 2,
      total = even * 100 + odd * 900;
  if (!is_valid(overall_index) || latest.value.small[overall_index] !=
                                      percentage(even * 90 + odd * 10, total))
    fail("weighted overall CPU");
  for (u32 i = 0; i < core_count; ++i)
    if (!is_valid(i) || latest.value.small[i] != (i % 2 ? 11 : 900))
      fail("per-core CPU value");
  if (!is_valid(iowait_index) ||
      latest.value.small[iowait_index] != percentage(odd * 10, total))
    fail("aggregate I/O wait");
  cpu_fd = openat(AT_CWD, argv[4], RDONLY, 0);
  check(cpu_fd, "offline CPU fixture");
  memset(latest.valid, 0, (metrics + 7) / 8);
  collect_cpu();
  if (is_valid(core_count - 1) || !is_valid(overall_index))
    fail("offline CPU validity");
  cpu_fd = openat(AT_CWD, argv[5], RDONLY, 0);
  check(cpu_fd, "returning CPU fixture");
  memset(latest.valid, 0, (metrics + 7) / 8);
  collect_cpu();
  if (is_valid(core_count - 1) || !is_valid(overall_index))
    fail("returning CPU needs fresh baseline");
  check_sampling();
  if (tracked_interface("lo"))
    fail("default tracking excludes device-less interfaces");
  interface = "lo";
  if (!tracked_interface("lo") || tracked_interface("eth0"))
    fail("--interface selects exactly one interface");
  interface = 0;
  if (network_pack(8500000000ULL) != (850 | (2 << 10) | (2 << 12)))
    fail("8.50 MB/s encoding");
  if (network_pack(100000000ULL) != (100 | (1 << 12)))
    fail("100 KB/s encoding");
  if (network_pack(999950000ULL) != (100 | (2 << 10) | (2 << 12)))
    fail("network unit boundary");
  const u32 slow_bucket_samples = 5184000;
  u64 wide = 10000000000000ULL * slow_bucket_samples;
  if (rounded_mean(wide, 2, slow_bucket_samples) != 10000000000000ULL ||
      rounded_mean(wide + slow_bucket_samples / 2, 2, slow_bucket_samples) != 10000000000001ULL ||
      network_rate(~0ULL, 0, 1) != NETWORK_MILLI_MAX)
    fail("wide network mean and saturation");
  if (publication_spacing(3600000, 30000) != 3600000 ||
      publication_spacing(250, 1000) != 1000 || publication_spacing(300, 1000) != 1200)
    fail("publication spacing follows sample intervals");
  if (resume_cursor(20000, 10, 24000) != 20000 - (points - 1) * 10 ||
      resume_cursor(20000, 10, 19980) != 19990 || resume_cursor(20000, 10, 19990))
    fail("resume cursor rejects a future watermark");
  for (u8 state = HUB_CONNECTING; state <= HUB_RESUMING; ++state) {
    hub_state = state;
    hub_retry_at = 0;
    wait_for_sample(clock_ns(1) + 10000000ULL);
    if (hub_state != HUB_IDLE)
      fail("uplink setup phases must expire");
  }
  hub_state = HUB_OFF;
  memory_kib = core_count == 1 ? 65536 : 2147483648ULL;
  swap_kib = 1048576;
  u16 overall = percentage(even * 90 + odd * 10, total);
  for (u32 elapsed = 0; elapsed <= 20000; elapsed += 5) {
    memset(latest.valid, 255, (metrics + 7) / 8);
    latest.sec = 1700000000 + elapsed;
    for (u32 k = 0; k < small_count; ++k)
      latest.value.small[k] = (u16)(100 + (k % 8) * 10);
    latest.value.small[overall_index] = overall;
    latest.value.small[temp_index] = TEMP_BIAS + 542;
    for (u32 i = 0; i < interface_count; ++i) {
      latest.value.net[2 * i] = 8500000000ULL;
      latest.value.net[2 * i + 1] = 100000000ULL;
    }
    if (elapsed == 50 || elapsed == 19050)
      latest.value.small[0] = 1000;
    update_tiers();
  }
  publish_disks();
  publish_live();
  check_clock_guard();
  return 0;
}

#ifdef QUECTOP_TEST_FREESTANDING
void entry(long *stack) { finish(main((int)stack[0], (char **)(stack + 1))); }
#endif
