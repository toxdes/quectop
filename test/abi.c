/* Exercise kernel ABI and compiler integer helpers without libc. */
#define entry agent_entry
#include "../main.c"
#undef entry

static u64 read_le64(const u8 *p) {
  return load_le32(p) | (u64)load_le32(p + 4) << 32;
}

void entry(long *stack) {
  if (stack[0] != 3)
    fail("ABI test needs a data directory and division vectors");
  char **argv = (char **)(stack + 1);
  long vectors = openat(AT_CWD, argv[2], RDONLY, 0);
  check(vectors, "division vectors");
  u8 record[32];
  for (;;) {
    long n = call4(N_read, vectors, (long)record, sizeof(record), 0);
    if (n == -4)
      continue;
    if (!n)
      break;
    if (n != sizeof(record))
      fail("division vector read failed");
    volatile u64 dividend = read_le64(record), divisor = read_le64(record + 8);
    u64 quotient = read_le64(record + 16), remainder = read_le64(record + 24);
    if (dividend / divisor != quotient || dividend % divisor != remainder)
      fail("64-bit division/remainder failed");
    if (!(dividend >> 32) && !(divisor >> 32)) {
      volatile u32 small_n = (u32)dividend, small_d = (u32)divisor;
      if (small_n / small_d != quotient || small_n % small_d != remainder)
        fail("32-bit division/remainder failed");
    }
  }
  call4(N_close, vectors, 0, 0, 0);
  directory_fd = openat(AT_CWD, argv[1], RDONLY | DIRECTORY, 0);
  check(directory_fd, "ABI test directory");
  long fd = openat(directory_fd, "abi.sparse", RDWR | CREATE | TRUNC, 0600);
  check(fd, "sparse offset test");
  const u64 bytes = (1ULL << 33) + 1234;
  check(truncate_file(fd, bytes), "64-bit truncate");
  if (seek_file(fd, 0, 2) != (i64)bytes)
    fail("64-bit seek-to-end failed");
  static const u64 offsets[] = {0, (1ULL << 31) + 7, (1ULL << 32) + 13,
                               (1ULL << 33) + 1000};
  for (u32 i = 0; i < 4; ++i) {
    u8 expected[8], actual[8];
    store_le64(expected, offsets[i] ^ 0xa573dead5a123456ULL);
    positioned(fd, expected, sizeof(expected), offsets[i], 1);
    positioned(fd, actual, sizeof(actual), offsets[i], 0);
    for (u32 j = 0; j < 8; ++j)
      if (actual[j] != expected[j])
        fail("64-bit positioned I/O failed");
    if (seek_file(fd, offsets[i], 0) != (i64)offsets[i])
      fail("64-bit absolute seek failed");
    if (call4(N_read, fd, (long)actual, sizeof(actual), 0) != sizeof(actual))
      fail("read after seek failed");
    for (u32 j = 0; j < 8; ++j)
      if (actual[j] != expected[j])
        fail("seek result mismatch");
  }
  check(call4(N_close, fd, 0, 0, 0), "sparse close");
  struct fsinfo fs;
  check(filesystem_info(argv[1], &fs), "filesystem ABI");
  u8 stats[48];
  store_le64(stats, fs.bsize);
  store_le64(stats + 8, fs.blocks);
  store_le64(stats + 16, fs.bfree);
  store_le64(stats + 24, fs.bavail);
  store_le64(stats + 32, fs.files);
  store_le64(stats + 40, fs.ffree);
  fd = openat(directory_fd, "abi.fs.bin", WRONLY | CREATE | TRUNC, 0600);
  check(fd, "filesystem ABI output");
  write_all(fd, stats, sizeof(stats));
  check(call4(N_close, fd, 0, 0, 0), "filesystem output close");
  u64 deadline = clock_ns(1) + 1000000;
  if (clock_ns(0) < 1600000000000000000ULL)
    fail("time64 realtime failed");
  sleep_until(deadline);
  if (clock_ns(1) < deadline)
    fail("time64 absolute sleep failed");
  struct timespec zero = {0, 0};
  if (call6(N_ppoll, 0, 0, (long)&zero, 0, 8, 0))
    fail("time64 nonblocking poll failed");
  long mapped = call6(N_mmap,
#if defined(__arm__)
                      (long)0x90000000U,
#else
                      0,
#endif
                      4096, 3, 0x22, -1, 0);
  check_mapping(mapped, "memory mapping ABI");
#if defined(__arm__)
  if ((unsigned long)mapped < 0x80000000U)
    fail("high-address mapping was not exercised");
#endif
  volatile u8 *memory = (u8 *)mapped;
  memory[0] = 0x5a;
  memory[4095] = 0xa5;
  if (memory[0] != 0x5a || memory[4095] != 0xa5)
    fail("memory mapping access failed");
  check(call4(N_unmap, mapped, 4096, 0, 0), "memory unmap ABI");
  write_all(1, "ABI checks passed\n", 18);
  finish(0);
}
