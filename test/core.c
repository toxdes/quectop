#ifdef QUECTOP_TEST_FREESTANDING
#define entry collector_entry
#endif
#include "../main.c"
#ifdef QUECTOP_TEST_FREESTANDING
#undef entry
#endif

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
  initialize_cpus();
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
  if (network_pack(8500000000ULL) != (850 | (2 << 10) | (2 << 12)))
    fail("8.50 MB/s encoding");
  if (network_pack(100000000ULL) != (100 | (1 << 12)))
    fail("100 KB/s encoding");
  if (network_pack(999950000ULL) != (100 | (2 << 10) | (2 << 12)))
    fail("network unit boundary");
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
  return 0;
}

#ifdef QUECTOP_TEST_FREESTANDING
void entry(long *stack) {
  finish(main((int)stack[0], (char **)(stack + 1)));
}
#endif
