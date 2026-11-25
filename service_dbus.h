/* Bounded system-bus transport. Replies borrow input until the next query. */
static long service_bus = -1;
static u32 service_serial, service_body, service_end;

static void service_disconnect(void) {
  if (service_bus >= 0)
    call4(N_close, service_bus, 0, 0, 0);
  service_bus = -1;
}
static int service_wait(short events) {
  struct {
    int fd;
    short events, revents;
  } descriptor = {(int)service_bus, events, 0};
  for (;;) {
    u64 now = clock_ns(1);
    if (!now || now >= service_deadline)
      return 0;
    u64 left = service_deadline - now;
    struct timespec timeout = {left / 1000000000ULL, left % 1000000000ULL};
    long result = call6(N_ppoll, (long)&descriptor, 1, (long)&timeout, 0, 8, 0);
    if (result == -4)
      continue;
    return result > 0 && (descriptor.revents & events);
  }
}
static int service_transfer(void *data, u32 bytes, int writing) {
  u8 *p = data;
  while (bytes) {
    if (clock_ns(1) >= service_deadline)
      return 0;
    long n = writing ? call6(N_send, service_bus, (long)p, bytes, 0x4000, 0, 0)
                     : call4(N_read, service_bus, (long)p, bytes, 0);
    if (n == -4)
      continue;
    if (n == -11 && service_wait(writing ? 4 : 1))
      continue;
    if (n <= 0)
      return 0;
    p += n;
    bytes -= (u32)n;
  }
  return 1;
}
static u32 service_string(u32 at, const char *s) {
  at = (at + 3) & ~3U;
  u32 n = (u32)length(s);
  store_le32((u8 *)input + at, n);
  memcpy(input + at + 4, s, n + 1);
  return at + 5 + n;
}
static u32 service_field(u32 at, u8 field, char type, const char *s) {
  at = (at + 7) & ~7U;
  input[at++] = (char)field;
  input[at++] = 1;
  input[at++] = type;
  input[at++] = 0;
  if (type != 'g')
    return service_string(at, s);
  u32 n = (u32)length(s);
  input[at++] = (char)n;
  memcpy(input + at, s, n + 1);
  return at + n + 1;
}
/* A property reply is small. Reject larger/unrecognized frames and reconnect
 * on the next publication rather than allocating from untrusted wire lengths.
 */
static int service_reply(u32 serial) {
  for (;;) {
    if (!service_transfer(input, 16, 0) || input[0] != 'l' || input[3] != 1)
      break;
    u32 body = load_le32((u8 *)input + 4), header = load_le32((u8 *)input + 12);
    if (header > sizeof(input) - 16)
      break;
    service_body = (16 + header + 7) & ~7U;
    if (body > sizeof(input) - service_body)
      break;
    service_end = service_body + body;
    if (!service_transfer(input + 16, service_end - 16, 0))
      break;
    u32 reply = 0, at = 16;
    while (at < 16 + header) {
      at = (at + 7) & ~7U;
      if (at + 4 > 16 + header || input[at + 1] != 1 || input[at + 3])
        goto broken;
      u8 field = (u8)input[at], type = (u8)input[at + 2];
      at += 4;
      if (type == 'u') {
        if (at + 4 > 16 + header)
          goto broken;
        if (field == 5)
          reply = load_le32((u8 *)input + at);
        at += 4;
      } else if (type == 's' || type == 'o' || type == 'g') {
        u32 n;
        if (type == 'g') {
          if (at >= 16 + header)
            goto broken;
          n = (u8)input[at++];
        } else {
          if (at + 4 > 16 + header)
            goto broken;
          n = load_le32((u8 *)input + at);
          at += 4;
        }
        if (n >= 16 + header - at || input[at + n])
          goto broken;
        at += n + 1;
      } else
        goto broken;
    }
    if (reply == serial && (input[1] == 2 || input[1] == 3))
      return input[1] == 2;
    /* Hello may be interleaved with NameAcquired; no match rules are installed.
     */
  }
broken:
  service_disconnect();
  return -1;
}
static int service_call(const char *path, const char *interface_name,
                        const char *member, const char *signature,
                        const char *first, const char *second) {
  if (service_bus < 0)
    return -1;
  memset(input, 0, sizeof(input));
  input[0] = 'l';
  input[1] = 1;
  input[2] = 2; /* Do not activate a missing destination. */
  input[3] = 1;
  if (!++service_serial)
    ++service_serial;
  store_le32((u8 *)input + 8, service_serial);
  u32 at = service_field(16, 1, 'o', path);
  at = service_field(at, 2, 's', interface_name);
  at = service_field(at, 3, 's', member);
  at = service_field(at, 6, 's',
                     equal(member, "Hello") ? "org.freedesktop.DBus"
                                            : "org.freedesktop.systemd1");
  if (*signature)
    at = service_field(at, 8, 'g', signature);
  store_le32((u8 *)input + 12, at - 16);
  u32 body = (at + 7) & ~7U;
  at = body;
  if (first)
    at = service_string(at, first);
  if (second)
    at = service_string(at, second);
  store_le32((u8 *)input + 4, at - body);
  if (!service_transfer(input, at, 1)) {
    service_disconnect();
    return -1;
  }
  return service_reply(service_serial);
}
static int service_connect(void) {
  if (service_bus >= 0)
    return 1;
  /* AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC. */
  service_bus = call4(N_socket, 1, 0x80801, 0, 0);
  if (service_bus < 0)
    return 0;
  struct {
    u16 family;
    char path[sizeof("/run/dbus/system_bus_socket")];
  } address = {1, "/run/dbus/system_bus_socket"};
  if (call4(N_connect, service_bus, (long)&address, 2 + sizeof(address.path),
            0) < 0)
    goto failed;
  static const char auth[] = "\0AUTH EXTERNAL ";
  memcpy(input, auth, sizeof(auth) - 1);
  u32 at = sizeof(auth) - 1;
  char digits[10];
  u32 count = 0, uid = (u32)call4(N_geteuid, 0, 0, 0, 0);
  do {
    digits[count++] = (char)('0' + uid % 10);
    uid /= 10;
  } while (uid);
  while (count) {
    char c = digits[--count];
    input[at++] = '3';
    input[at++] = c;
  }
  input[at++] = '\r';
  input[at++] = '\n';
  if (!service_transfer(input, at, 1))
    goto failed;
  at = 0;
  do {
    if (at == 128 || !service_transfer(input + at, 1, 0))
      goto failed;
    ++at;
  } while (at < 2 || input[at - 2] != '\r' || input[at - 1] != '\n');
  if (at < 5 || input[0] != 'O' || input[1] != 'K' || input[2] != ' ')
    goto failed;
  memcpy(input, "BEGIN\r\n", 7);
  if (!service_transfer(input, 7, 1) ||
      service_call("/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", "",
                   0, 0) != 1)
    goto failed;
  return 1;
failed:
  service_disconnect();
  return 0;
}
static int service_property(const char *path, const char *interface_name,
                            const char *property, char type) {
  return service_call(path, "org.freedesktop.DBus.Properties", "Get", "ss",
                      interface_name, property) == 1 &&
         service_end >= service_body + 3 && input[service_body] == 1 &&
         input[service_body + 1] == type && !input[service_body + 2];
}

static inline __attribute__((always_inline)) const char *
service_reply_string(u32 at, u32 *bytes) {
  if (service_end < at + 5)
    return 0;
  u32 n = load_le32((u8 *)input + at);
  if (n >= service_end - at - 4 || input[at + 4 + n])
    return 0;
  *bytes = n;
  return input + at + 4;
}

static inline __attribute__((always_inline)) const char *
service_property_string(const char *path, const char *interface_name,
                        const char *property) {
  if (!service_property(path, interface_name, property, 's'))
    return 0;
  u32 bytes;
  return service_reply_string(service_body + 4, &bytes);
}

static inline __attribute__((always_inline)) int
service_property_counter(const char *path, const char *interface_name,
                         const char *property, u64 *value) {
  if (!service_property(path, interface_name, property, 't') ||
      service_end - service_body < 16)
    return 0;
  *value = (u64)load_le32((u8 *)input + service_body + 8) |
           (u64)load_le32((u8 *)input + service_body + 12) << 32;
  return 1;
}
