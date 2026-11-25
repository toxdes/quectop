/* Bounded proc/sysfs input; streams share one scratch buffer. */
static char input[4096];
static long cpu_fd, mem_fd, net_fd, temp_fd, mounts_fd;
/* cpu, memory, io: absent without kernel PSI support. */
static long pressure_fd[3];

static char *read_proc(long fd) {
  if (fd < 0)
    return 0;
  long n;
  do {
    n = file_io(N_pread, fd, input, sizeof(input) - 1, 0);
  } while (n == -4);
  if (n <= 0 || n == (long)sizeof(input) - 1)
    return 0;
  input[n] = 0;
  return input;
}

/* /proc/stat can be much larger than our fixed scratch buffer. Stop at the
 * first non-CPU row. */
static u32 stream_at, stream_end;
static int mounts_truncated;
static long stream_fd;
static void stream_rewind(long fd) {
  stream_fd = fd;
  if (seek_file(fd, 0, 0) < 0)
    fail("cannot rewind proc input");
  stream_at = stream_end = 0;
}
static const char *stream_line(void) {
  for (;;) {
    if (stream_fd == cpu_fd && stream_end - stream_at >= 4 &&
        !prefix(input + stream_at, "cpu"))
      return 0;
    for (u32 i = stream_at; i < stream_end; ++i)
      if (input[i] == '\n') {
        input[i] = 0;
        const char *line = input + stream_at;
        stream_at = i + 1;
        return line;
      }
    u32 left = stream_end - stream_at;
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
        stream_end = (u32)n;
        for (stream_at = 0; stream_at < stream_end; ++stream_at)
          if (input[stream_at] == '\n') {
            ++stream_at;
            found = 1;
            break;
          }
      }
      continue;
    }
    for (u32 i = 0; i < left; ++i)
      input[i] = input[stream_at + i];
    stream_at = 0;
    stream_end = left;
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
    stream_end += (u32)n;
    input[stream_end] = 0;
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
