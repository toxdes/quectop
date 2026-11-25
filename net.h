/* IPv4 endpoints. Socket types are nonblocking and close-on-exec. */
#define SOCKET_STREAM 0x80801
#define SOCKET_DATAGRAM 0x80802
#define SEND_FLAGS 0x4040 /* MSG_NOSIGNAL | MSG_DONTWAIT */
#define POLL_IN 1
#define POLL_OUT 4
#define POLL_FAILED 0x18 /* POLLERR | POLLHUP */

struct endpoint {
  u16 family, port; /* port and address in network byte order */
  u8 address[4], zero[8];
};
struct poll_entry {
  int fd;
  short events, revents;
};

/* Parses "a.b.c.d" and advances *s past it. */
static int parse_ipv4(const char **s, u8 *address) {
  for (u32 i = 0; i < 4; ++i) {
    u64 n;
    if ((i && *(*s)++ != '.') || !number(s, &n) || n > 255)
      return 0;
    address[i] = (u8)n;
  }
  return 1;
}
static int parse_port(const char *s, struct endpoint *e) {
  u64 n;
  if (!number(&s, &n) || *s || !n || n > 65535)
    return 0;
  e->port = (u16)(n >> 8 | (n & 255) << 8);
  return 1;
}
static int wait_events(struct poll_entry *entries, u32 count, u64 deadline) {
  u64 now = clock_ns(1), left = deadline > now ? deadline - now : 0;
  struct timespec timeout = {left / 1000000000ULL, left % 1000000000ULL};
  long result = call6(N_ppoll, (long)entries, count, (long)&timeout, 0, 8, 0);
  return result == -4 ? 0 : (int)result;
}
