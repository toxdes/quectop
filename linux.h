#ifndef QUECTOP_LINUX_H
#define QUECTOP_LINUX_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed long long i64;
typedef signed short i16;
_Static_assert(sizeof(u32) == 4 && sizeof(u64) == 8, "fixed-width integers");
#if !defined(__arm__)
_Static_assert(sizeof(long) == 8, "64-bit Linux ABI required");
#endif

#if defined(__x86_64__)
enum {
  N_read = 0,
  N_write = 1,
  N_getdents = 217,
  N_close = 3,
  N_seek = 8,
  N_pread = 17,
  N_pwrite = 18,
  N_mmap = 9,
  N_unmap = 11,
  N_socket = 41,
  N_connect = 42,
  N_bind = 49,
  N_listen = 50,
  N_accept = 288, /* accept4 */
  N_setsockopt = 54,
  N_send = 44,
  N_geteuid = 107,
  N_ppoll = 271,
  N_flock = 73,
  N_truncate = 77,
  N_statfs = 137,
  N_uname = 63,
  N_clock = 228,
  N_sleep = 230,
  N_exit = 231,
  N_open = 257,
  N_mkdir = 258,
  N_unlink = 263,
  N_rename = 264,
  N_getrandom = 318,
  N_statx = 332,
  N_mprotect = 10
};
static inline long call4(long n, long a, long b, long c, long d) {
  register long r10 __asm__("r10") = d;
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}
static inline long call6(long n, long a, long b, long c, long d, long e,
                         long f) {
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}
#elif defined(__aarch64__)
enum {
  N_read = 63,
  N_write = 64,
  N_getdents = 61,
  N_close = 57,
  N_seek = 62,
  N_pread = 67,
  N_pwrite = 68,
  N_mmap = 222,
  N_unmap = 215,
  N_socket = 198,
  N_connect = 203,
  N_bind = 200,
  N_listen = 201,
  N_accept = 242, /* accept4 */
  N_setsockopt = 208,
  N_send = 206,
  N_geteuid = 175,
  N_ppoll = 73,
  N_flock = 32,
  N_truncate = 46,
  N_statfs = 43,
  N_uname = 160,
  N_clock = 113,
  N_sleep = 115,
  N_exit = 94,
  N_open = 56,
  N_mkdir = 34,
  N_unlink = 35,
  N_rename = 38,
  N_getrandom = 278,
  N_statx = 291,
  N_mprotect = 226
};
static inline long call4(long n, long a, long b, long c, long d) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  __asm__ volatile("svc 0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory", "cc");
  return x0;
}
static inline long call6(long n, long a, long b, long c, long d, long e,
                         long f) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  __asm__ volatile("svc 0"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory", "cc");
  return x0;
}
#elif defined(__arm__) && defined(__ARM_EABI__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ARM32 supports little-endian EABI only"
#endif
_Static_assert(sizeof(long) == 4, "32-bit ARM EABI required");
enum {
  N_read = 3,
  N_write = 4,
  N_getdents = 217,
  N_close = 6,
  N_seek = 140, /* _llseek: result is returned through a u64 pointer. */
  N_pread = 180,
  N_pwrite = 181,
  N_mmap = 192, /* mmap2; only anonymous mappings with offset zero are used. */
  N_unmap = 91,
  N_socket = 281,
  N_connect = 283,
  N_bind = 282,
  N_listen = 284,
  N_accept = 366, /* accept4 */
  N_setsockopt = 294,
  N_send = 290,
  N_geteuid = 201,
  N_ppoll = 414, /* ppoll_time64, Linux >= 5.1 */
  N_flock = 143,
  N_truncate = 194,
  N_statfs = 266, /* statfs64: takes the structure size. */
  N_uname = 122,
  N_clock = 403,
  N_sleep = 407,
  N_exit = 248,
  N_open = 322,
  N_mkdir = 323,
  N_unlink = 328,
  N_rename = 329,
  N_getrandom = 384,
  N_statx = 397,
  N_mprotect = 125
};
static inline long call4(long n, long a, long b, long c, long d) {
  register long r7 __asm__("r7") = n;
  register long r0 __asm__("r0") = a;
  register long r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c;
  register long r3 __asm__("r3") = d;
  __asm__ volatile("svc 0"
                   : "+r"(r0)
                   : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
                   : "memory", "cc");
  return r0;
}
static inline long call6(long n, long a, long b, long c, long d, long e,
                         long f) {
  register long r7 __asm__("r7") = n;
  register long r0 __asm__("r0") = a;
  register long r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c;
  register long r3 __asm__("r3") = d;
  register long r4 __asm__("r4") = e;
  register long r5 __asm__("r5") = f;
  __asm__ volatile("svc 0"
                   : "+r"(r0)
                   : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                   : "memory", "cc");
  return r0;
}
#else
#error "Supported targets: Linux ARM EABI, ARM64 and x86-64"
#endif

#define AT_CWD (-100)
#define RDONLY 0
#define WRONLY 1
#define RDWR 2
#define CREATE 64
#define EXCL 128
#define APPEND 0x400
#define TRUNC 512
#define PATH_ONLY 0x200000
#if defined(__aarch64__) || defined(__arm__)
#define DIRECTORY (1 << 14)
#define NOFOLLOW (1 << 15)
#else
#define DIRECTORY (1 << 16)
#define NOFOLLOW (1 << 17)
#endif

struct timespec {
#if defined(__arm__)
  i64 sec, nsec;
#else
  long sec, nsec;
#endif
};
_Static_assert(sizeof(struct timespec) == 16, "kernel time64 ABI");
struct fsinfo {
  long type, bsize;
  u64 blocks, bfree, bavail, files, ffree;
  int fsid[2];
  long namelen, frsize, flags, spare[4];
}
#if defined(__arm__)
    __attribute__((packed, aligned(4)))
#endif
    ;
_Static_assert(sizeof(struct fsinfo) ==
#if defined(__arm__)
                   84,
#else
                   120,
#endif
               "kernel filesystem ABI");
static long openat(long dir, const char *name, long flags, long mode) {
#if defined(__arm__)
  flags |= 1 << 17; /* O_LARGEFILE */
#endif
  return call4(N_open, dir, (long)name, flags, mode);
}
static inline long file_io(long n, long fd, void *data, unsigned long bytes,
                           u64 offset) {
#if defined(__arm__)
  /* EABI aligns the 64-bit offset to r4/r5, leaving r3 unused. */
  return call6(n, fd, (long)data, bytes, 0, (u32)offset, (u32)(offset >> 32));
#else
  return call4(n, fd, (long)data, bytes, (long)offset);
#endif
}
static inline i64 seek_file(long fd, u64 offset, int whence) {
#if defined(__arm__)
  u64 value;
  long result = call6(N_seek, fd, (u32)(offset >> 32), (u32)offset,
                      (long)&value, whence, 0);
  return result < 0 ? result : (i64)value;
#else
  return call4(N_seek, fd, (long)offset, whence, 0);
#endif
}
static inline long truncate_file(long fd, u64 bytes) {
#if defined(__arm__)
  return call4(N_truncate, fd, 0, (u32)bytes, (u32)(bytes >> 32));
#else
  return call4(N_truncate, fd, (long)bytes, 0, 0);
#endif
}
static inline long filesystem_info(const char *path, struct fsinfo *fs) {
#if defined(__arm__)
  return call4(N_statfs, (long)path, sizeof(*fs), (long)fs, 0);
#else
  return call4(N_statfs, (long)path, (long)fs, 0, 0);
#endif
}
static u64 clock_ns(int which) {
  struct timespec t;
  if (call4(N_clock, which, (long)&t, 0, 0) < 0)
    return 0;
  return (u64)t.sec * 1000000000ULL + (u64)t.nsec;
}
static void sleep_until(u64 ns) {
  struct timespec t = {ns / 1000000000ULL, ns % 1000000000ULL};
  while (call4(N_sleep, 1, 1, (long)&t, 0) == -4) {
  }
}
#endif
