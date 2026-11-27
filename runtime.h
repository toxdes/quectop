/* Freestanding memory, parsing, I/O, and wire primitives. */

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
static inline __attribute__((always_inline)) int
try_write_all(long fd, const void *data, unsigned long bytes) {
  const char *p = data;
  while (bytes) {
    long n = call4(N_write, fd, (long)p, bytes, 0);
    if (n == -4)
      continue;
    if (n <= 0)
      return 0;
    p += n;
    bytes -= (unsigned long)n;
  }
  return 1;
}
static void write_all(long fd, const void *data, unsigned long bytes) {
  if (!try_write_all(fd, data, bytes)) {
    const char m[] = PROGRAM_NAME ": write failed\n";
    call4(N_write, 2, (long)m, sizeof(m) - 1, 0);
    finish(1);
  }
}
static void fail(const char *s) __attribute__((noreturn));
static void fail(const char *s) {
  write_all(2, PROGRAM_NAME ": ", sizeof(PROGRAM_NAME) + 1);
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
static long temporary_output_mode(long directory, const char *name, long mode) {
  /* Recreate leftovers instead of truncating hardlinks or opening FIFOs. */
  long result = call4(N_unlink, directory, (long)name, 0, 0);
  if (result < 0 && result != -2)
    return result;
  return openat(directory, name, WRONLY | CREATE | EXCL | NOFOLLOW, mode);
}
static long temporary_output(long directory, const char *name) {
  return temporary_output_mode(directory, name, 0644);
}
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

static inline __attribute__((always_inline)) int
try_positioned(long fd, void *data, unsigned long bytes, u64 offset, int writing) {
  char *p = data;
  while (bytes) {
    long n =
        file_io(writing ? N_pwrite : N_pread, fd, p, bytes, offset);
    if (n == -4)
      continue;
    if (n <= 0)
      return 0;
    bytes -= (unsigned long)n;
    offset += (u64)n;
    p += n;
  }
  return 1;
}
static void positioned(long fd, void *data, unsigned long bytes, u64 offset,
                       int writing) {
  if (!try_positioned(fd, data, bytes, offset, writing))
    fail(writing ? "history write failed" : "history header unreadable");
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
