/* Probe streams (protocol.h). A device counts as connected once its first
 * sealed frame opens, since Noise message 1 alone can be replayed. Each frame
 * is validated before it touches the host's replica; any violation closes the
 * stream. */
#define MAX_PROBES 64
#define PROBE_BUFFER (MESSAGE_HEADER + NOISE_MESSAGE_MAX)
#define PROBE_MAPPING (sizeof(struct greeting) + PROBE_BUFFER)
#define RING_MAX_BYTES (256u << 20)
#define HELLO_TIMEOUT_MS 10000 /* frees slots held by unproven connections */
/* Oldest probe versions this hub handles. Raise one when its handling goes. */
#define PROTOCOL_OLDEST 3
#define SCHEMA_OLDEST 1
#define DISK_SCHEMA_OLDEST 1
_Static_assert(SCHEMA_OLDEST == QUECTOP_SCHEMA_VERSION &&
                   DISK_SCHEMA_OLDEST == QUECTOP_DISK_SCHEMA_VERSION,
               "an older data format needs its own replica names and decoding "
               "in web/codec.js");

struct ring {
  long fd;
  u32 points, width, record_bytes, header_bytes, newest;
};
/* Handshake state, mapped with the buffer so idle slots cost nothing. */
struct greeting {
  struct noise handshake;
  u32 publish_ms;
  u8 id_bytes, name_bytes;
  char id[HOST_ID_MAX], name[HOST_NAME_MAX];
};
struct probe {
  long fd;
  struct host *host;
  struct greeting *greeting; /* PROBE_MAPPING bytes, mapped while connected */
  u8 *buffer;                /* PROBE_BUFFER bytes after the greeting */
  u32 have, timeout_ms;
  u8 rings; /* tiers whose RING arrived */
  u8 phase; /* PHASE_* */
  struct noise_cipher send, receive;
  u64 last_ns;
  struct ring ring[TIERS];
};
enum { PHASE_OPEN, PHASE_HANDSHAKE, PHASE_SEALED };
static struct probe probes[MAX_PROBES];
static u8 scan[65536];

static void close_probe(struct probe *a) {
  call4(N_close, a->fd, 0, 0, 0);
  for (u32 i = 0; i < TIERS; ++i)
    if (a->rings >> i & 1)
      call4(N_close, a->ring[i].fd, 0, 0, 0);
  crypto_wipe(a->greeting, sizeof(*a->greeting));
  call4(N_unmap, (long)a->greeting, PROBE_MAPPING, 0, 0);
  int listed = a->host && a->host->probe == a;
  if (listed)
    a->host->probe = 0;
  crypto_wipe(a, sizeof(*a));
  a->fd = -1;
  if (listed)
    write_hosts();
}
static void accept_probe(long listener) {
  long fd = call4(N_accept, listener, 0, 0, 0x80800); /* NONBLOCK|CLOEXEC */
  if (fd < 0)
    return;
  for (u32 i = 0; i < MAX_PROBES; ++i)
    if (probes[i].fd < 0) {
      long mapped = call6(N_mmap, 0, PROBE_MAPPING, 3, 0x22, -1, 0);
      if ((unsigned long)mapped >= (unsigned long)-4095)
        break;
      struct probe *a = &probes[i];
      a->fd = fd;
      a->greeting = (struct greeting *)mapped;
      a->buffer = (u8 *)(a->greeting + 1);
      a->timeout_ms = HELLO_TIMEOUT_MS;
      a->last_ns = clock_ns(1);
      return;
    }
  call4(N_close, fd, 0, 0, 0);
}

static char *append(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}
static void log_rejected(const struct greeting *g, const char *reason) {
  char line[160 + HOST_ID_MAX], *p = append(line, PROGRAM_NAME ": rejected device ");
  memcpy(p, g->id, g->id_bytes);
  p = append(p + g->id_bytes, reason);
  *p++ = '\n';
  write_all(2, line, (unsigned long)(p - line));
}
/* Accepts a version in [oldest, newest]; logs which side needs an upgrade. */
static int supported(const struct greeting *g, const char *what, u32 version,
                     u32 oldest, u32 newest) {
  if (version >= oldest && version <= newest)
    return 1;
  char reason[128], *p = append(reason, ": ");
  p = decimal(append(p, what), version);
  p = decimal(append(p, "; this hub accepts "), oldest);
  if (oldest != newest)
    p = decimal(append(p, " to "), newest);
  p = append(p, version > newest ? "; upgrade the hub" : "; upgrade the device");
  *p = 0;
  log_rejected(g, reason);
  return 0;
}
static const char key_rejected[] = ": not in the keys file, or wrong key";
/* OPEN: "QTOP", version, id; the id picks the key for the handshake. */
static int on_open(struct probe *a, const u8 *p, u32 bytes) {
  struct greeting *g = a->greeting;
  u32 id_bytes = bytes > 5 ? p[5] : 0;
  u8 key[KEY_BYTES];
  if (bytes != 6 + id_bytes || p[0] != 'Q' || p[1] != 'T' || p[2] != 'O' ||
      p[3] != 'P' || !valid_id((const char *)p + 6, id_bytes))
    return 0;
  memcpy(g->id, p + 6, id_bytes);
  g->id_bytes = (u8)id_bytes;
  if (!supported(g, "protocol version ", p[4], PROTOCOL_OLDEST, PROTOCOL_VERSION))
    return 0;
  if (!device_key(g->id, id_bytes, key)) {
    log_rejected(g, key_rejected);
    return 0;
  }
  noise_start(&g->handshake, p, bytes, key);
  crypto_wipe(key, sizeof(key));
  a->phase = PHASE_HANDSHAKE;
  return 1;
}
/* Noise message 1, then message 2 back; a wrong key fails here. */
static int on_handshake(struct probe *a, u8 *p, u32 bytes) {
  struct greeting *g = a->greeting;
  long got = noise_read(&g->handshake, p, bytes);
  const u8 *payload = p + NOISE_KEY;
  u32 name_bytes = got >= 5 ? payload[4] : 0;
  if (got < 0) {
    log_rejected(g, key_rejected);
    return 0;
  }
  if ((u32)got != 5 + name_bytes || !valid_name((const char *)payload + 5, name_bytes))
    return 0;
  g->publish_ms = load_le32(payload);
  memcpy(g->name, payload + 5, name_bytes);
  g->name_bytes = (u8)name_bytes;
  u8 secret[NOISE_KEY], reply[MESSAGE_HEADER + NOISE_HANDSHAKE_MIN];
  if (call4(N_getrandom, (long)secret, NOISE_KEY, 0, 0) != NOISE_KEY)
    return 0;
  u32 n = noise_write(&g->handshake, secret, 0, 0, reply + MESSAGE_HEADER);
  crypto_wipe(secret, sizeof(secret));
  if (!n)
    return 0;
  noise_split(&g->handshake, 0, &a->send, &a->receive);
  reply[0] = (u8)n;
  reply[1] = (u8)(n >> 8);
  a->phase = PHASE_SEALED;
  /* A fresh socket always has room for message 2. */
  return call6(N_send, a->fd, (long)reply, sizeof(reply), SEND_FLAGS, 0, 0) ==
         sizeof(reply);
}
/* The first frame that opens proves the device holds the transport keys. */
static int attach_host(struct probe *a) {
  const struct greeting *g = a->greeting;
  struct host *h = find_host(g->id, g->id_bytes);
  if (!h && !(h = add_host(g->id, g->id_bytes)))
    return 0;
  if (h->probe) /* a reconnect can arrive before the old stream times out */
    close_probe(h->probe);
  h->probe = a;
  a->host = h;
  a->timeout_ms = 3 * (g->publish_ms < 3600000 ? g->publish_ms : 3600000) + 60000;
  memcpy(h->name, g->name, g->name_bytes);
  h->name_bytes = g->name_bytes;
  write_hosts();
  return 1;
}

static u32 newest_bucket(const struct ring *r) {
  u32 chunk = sizeof(scan) / r->record_bytes * r->record_bytes, newest = 0;
  u64 end = r->header_bytes + (u64)(r->points - 1) * r->record_bytes;
  for (u64 at = r->header_bytes; at < end; at += chunk) {
    u32 bytes = end - at < chunk ? (u32)(end - at) : chunk;
    positioned(r->fd, scan, bytes, at, 0);
    for (u32 k = 0; k < bytes; k += r->record_bytes) {
      const u8 *record = scan + k;
      u32 start = load_le32(record);
      if (start > newest &&
          load_le32(record + r->record_bytes - 4) ==
              hash(record, r->record_bytes - 4))
        newest = start;
    }
  }
  return newest;
}
/* Keeps a matching replica ring, otherwise recreates it from the header. */
static int on_ring(struct probe *a, const u8 *p, u32 bytes) {
  u32 tier = bytes ? p[0] : TIERS;
  const u8 *h = p + 1;
  u32 header_bytes = bytes - 1;
  if (!a->host || tier >= TIERS || (a->rings >> tier & 1) ||
      header_bytes < HEADER_BYTES || !prefix((const char *)h, "QUECTOPH") ||
      !supported(a->greeting, "history format version ", load_le32(h + 8),
                 SCHEMA_OLDEST, QUECTOP_SCHEMA_VERSION) ||
      load_le32(h + 32) != tier ||
      load_le32(h + 44) != header_bytes ||
      load_le32(h + 60) != hash_more(hash(h, HEADER_BYTES - 4),
                                     h + HEADER_BYTES, header_bytes - HEADER_BYTES))
    return 0;
  struct ring *r = &a->ring[tier];
  r->points = load_le32(h + 12);
  r->width = load_le32(h + 20);
  r->record_bytes = load_le32(h + 28);
  r->header_bytes = header_bytes;
  u64 size = header_bytes + (u64)(r->points - 1) * r->record_bytes;
  if (r->points < 16 || r->points > 10000 || !r->width || r->record_bytes < 8 ||
      r->record_bytes > FRAME_MAX - 1 || size > RING_MAX_BYTES)
    return 0;
  r->fd = openat(a->host->dir, filenames[tier], RDWR | CREATE | NOFOLLOW, 0644);
  if (r->fd < 0)
    return 0;
  a->rings |= (u8)(1 << tier);
  int same = (u64)seek_file(r->fd, 0, 2) == size;
  for (u32 at = 0; same && at < header_bytes; at += sizeof(scan)) {
    u32 n = header_bytes - at < sizeof(scan) ? header_bytes - at : sizeof(scan);
    positioned(r->fd, scan, n, at, 0);
    for (u32 k = 0; same && k < n; ++k)
      same = scan[k] == h[at + k];
  }
  if (same) {
    r->newest = newest_bucket(r);
  } else {
    check(truncate_file(r->fd, 0), "cannot reset replica ring");
    check(truncate_file(r->fd, size), "cannot size replica ring");
    positioned(r->fd, (void *)h, header_bytes, 0, 1);
    r->newest = 0;
  }
  if (a->rings != (1 << TIERS) - 1)
    return 1;
  u8 reply[MESSAGE_HEADER + RESUME_BYTES + NOISE_TAG], *resume = reply + MESSAGE_HEADER;
  reply[0] = RESUME_BYTES + NOISE_TAG;
  reply[1] = 0;
  frame_header(resume, FRAME_RESUME, 4 * TIERS);
  for (u32 i = 0; i < TIERS; ++i)
    store_le32(resume + FRAME_HEADER + 4 * i, a->ring[i].newest);
  noise_seal(&a->send, 0, 0, resume, RESUME_BYTES);
  /* The socket still has room: the hub sent only message 2 before. */
  return call6(N_send, a->fd, (long)reply, sizeof(reply), SEND_FLAGS, 0, 0) ==
         sizeof(reply);
}
static int on_record(struct probe *a, const u8 *p, u32 bytes) {
  u32 tier = bytes ? p[0] : TIERS;
  if (a->rings != (1 << TIERS) - 1 || tier >= TIERS)
    return 0;
  const struct ring *r = &a->ring[tier];
  const u8 *record = p + 1;
  u32 start = load_le32(record);
  if (bytes != 1 + r->record_bytes || start % r->width ||
      load_le32(record + r->record_bytes - 4) != hash(record, r->record_bytes - 4))
    return 0;
  u32 slot = start / r->width % (r->points - 1);
  positioned(r->fd, (void *)record, r->record_bytes,
             r->header_bytes + (u64)slot * r->record_bytes, 1);
  return 1;
}
static int on_snapshot(struct probe *a, u8 type, const u8 *p, u32 bytes) {
  int live = type == FRAME_LIVE;
  if (a->rings != (1 << TIERS) - 1 || bytes < (live ? LIVE_HEADER_BYTES : 28) + 4 ||
      !prefix((const char *)p, live ? "QUECTOPL" : "QUECTOPD") ||
      !(live ? supported(a->greeting, "live format version ", load_le32(p + 8),
                         SCHEMA_OLDEST, QUECTOP_SCHEMA_VERSION)
             : supported(a->greeting, "disk format version ", load_le32(p + 8),
                         DISK_SCHEMA_OLDEST, QUECTOP_DISK_SCHEMA_VERSION)) ||
      load_le32(p + bytes - 4) != hash(p, bytes - 4))
    return 0;
  if (live)
    replace_file(a->host->dir, "." LIVE_FILE ".tmp", LIVE_FILE, p, bytes);
  else
    replace_file(a->host->dir, "." DISKS_FILE ".tmp", DISKS_FILE, p, bytes);
  return 1;
}
static int on_frame(struct probe *a, u8 type, const u8 *p, u32 bytes) {
  switch (type) {
  case FRAME_RING:
    return on_ring(a, p, bytes);
  case FRAME_RECORD:
    return on_record(a, p, bytes);
  case FRAME_LIVE:
  case FRAME_DISKS:
    return on_snapshot(a, type, p, bytes);
  case FRAME_RESUME:
    return 0;
  default: /* a newer probe's optional frame */
    return 1;
  }
}
static int on_message(struct probe *a, u8 *m, u32 bytes) {
  if (a->phase == PHASE_OPEN)
    return on_open(a, m, bytes);
  if (a->phase == PHASE_HANDSHAKE)
    return on_handshake(a, m, bytes);
  if (!noise_open(&a->receive, 0, 0, m, bytes))
    return 0;
  u32 frame = bytes - NOISE_TAG;
  return frame >= FRAME_HEADER && load_le32(m) >> 8 == frame - FRAME_HEADER &&
         (a->host || attach_host(a)) &&
         on_frame(a, m[0], m + FRAME_HEADER, frame - FRAME_HEADER);
}
static void read_probe(struct probe *a) {
  long n = call4(N_read, a->fd, (long)(a->buffer + a->have),
                 PROBE_BUFFER - a->have, 0);
  if (n == -4 || n == -11)
    return;
  if (n <= 0) {
    close_probe(a);
    return;
  }
  a->have += (u32)n;
  a->last_ns = clock_ns(1);
  u32 at = 0;
  while (a->have - at >= MESSAGE_HEADER) {
    u8 *m = a->buffer + at + MESSAGE_HEADER;
    u32 bytes = (u32)m[-2] | (u32)m[-1] << 8;
    if (a->have - at < MESSAGE_HEADER + bytes)
      break;
    if (!on_message(a, m, bytes)) {
      close_probe(a);
      return;
    }
    at += MESSAGE_HEADER + bytes;
  }
  a->have -= at;
  memcpy(a->buffer, a->buffer + at, a->have); /* forward copy; dst < src */
}
