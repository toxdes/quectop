/* Probe streams (protocol.h). A device counts as connected once its first
 * sealed frame opens, since Noise message 1 alone can be replayed. Each frame
 * is validated before it touches the host's replica; any violation closes the
 * stream. */
#define MAX_PROBES 64
#define PROBE_BUFFER (MESSAGE_HEADER + NOISE_MESSAGE_MAX)
#define PROBE_MAPPING (sizeof(struct greeting) + PROBE_BUFFER)
#define RING_MAX_BYTES (256u << 20)
#define HELLO_TIMEOUT_MS 10000 /* frees slots held by unproven connections */
#define FRAME_TIMEOUT_NS 10000000000ULL
#define ALL_RINGS ((1 << TIERS) - 1)
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
  u32 points, width, record_bytes, header_bytes, newest, scan_at;
};
struct replica_layout {
  u32 points, interval, cores, metrics, id_bits, interfaces, frequencies,
      frequency_bits, metadata_bytes, metadata_hash;
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
  u8 scan_tier;
  u8 phase; /* PHASE_* */
  struct noise_cipher send, receive;
  u64 last_ns; /* accept time until the first authenticated frame */
  u64 frame_deadline, resume_deadline;
  struct replica_layout layout;
  struct ring ring[TIERS];
};
enum { PHASE_OPEN, PHASE_HANDSHAKE, PHASE_SEALED };
static struct probe probes[MAX_PROBES];
static u8 scan[65536];

static u64 probe_deadline(const struct probe *a) {
  u64 idle = a->last_ns + (u64)a->timeout_ms * 1000000;
  if (a->resume_deadline && a->resume_deadline < idle)
    idle = a->resume_deadline;
  return a->frame_deadline && a->frame_deadline < idle ? a->frame_deadline : idle;
}

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
  call4(N_write, 2, (long)line, (unsigned long)(p - line), 0);
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
  u64 now = clock_ns(1);
  if (h->retry_at && now < h->retry_at)
    return 0;
  h->retry_at = 0;
  struct probe *previous = h->probe;
  h->probe = a;
  a->host = h;
  if (previous)
    close_probe(previous);
  a->timeout_ms = transport_timeout_ms(g->publish_ms);
  a->resume_deadline = now + RESUME_TIMEOUT_MS * 1000000ULL;
  memcpy(h->name, g->name, g->name_bytes);
  h->name_bytes = g->name_bytes;
  write_hosts();
  return 1;
}

static int replica_failed(struct probe *a, const char *reason) {
  a->host->retry_at = clock_ns(1) + STORAGE_RETRY_NS;
  log_rejected(a->greeting, reason);
  return 0;
}

static int scan_ring(struct ring *r) {
  u32 chunk = sizeof(scan) / r->record_bytes * r->record_bytes;
  u32 end = r->header_bytes + (r->points - 1) * r->record_bytes;
  u32 at = r->scan_at, bytes = end - at < chunk ? end - at : chunk;
  if (!try_positioned(r->fd, scan, bytes, at, 0))
    return 0;
  u32 slot = (at - r->header_bytes) / r->record_bytes;
  for (u32 k = 0; k < bytes; k += r->record_bytes, ++slot) {
    const u8 *record = scan + k;
    u32 start = load_le32(record);
    if (start > r->newest && start % r->width == 0 &&
        start / r->width % (r->points - 1) == slot &&
        load_le32(record + r->record_bytes - 4) == hash(record, r->record_bytes - 4))
      r->newest = start;
  }
  r->scan_at = at + bytes < end ? at + bytes : 0;
  return 1;
}

/* Returns -1 on I/O error, 0 for a different layout, or 1 for a match. */
static int ring_matches(const struct ring *r, const u8 *header, u64 size) {
  i64 current = seek_file(r->fd, 0, 2);
  if (current < 0)
    return -1;
  if ((u64)current != size)
    return 0;
  if (!try_positioned(r->fd, scan, r->header_bytes, 0, 0))
    return -1;
  for (u32 i = 0; i < r->header_bytes; ++i)
    if (scan[i] != header[i])
      return 0;
  return 1;
}

static int prepare_ring(struct ring *r, long dir, const char *name,
                        const u8 *header, u64 size) {
  int mode = 0644;
  r->fd = openat(dir, name, RDWR | NONBLOCK | NOFOLLOW, 0);
  if (r->fd >= 0) {
    u32 links;
    mode = regular_file(r->fd, &links);
    int same = mode < 0 ? -1 : links == 1 ? ring_matches(r, header, size) : 0;
    if (same > 0) {
      r->scan_at = r->header_bytes;
      return 1;
    }
    call4(N_close, r->fd, 0, 0, 0);
    if (same)
      return 0;
  } else if (r->fd != -2) { /* ENOENT */
    return 0;
  }

  const char *temporary = ".ring.tmp";
  long fd = temporary_output_mode(dir, temporary, mode & 0777);
  if (fd < 0)
    return 0;
  int complete = truncate_file(fd, size) == 0 &&
                 try_write_all(fd, header, r->header_bytes);
  if (!publish_output(dir, temporary, name, fd, complete))
    return 0;
  r->newest = 0;
  r->fd = openat(dir, name, RDWR | NONBLOCK | NOFOLLOW, 0);
  return r->fd >= 0;
}

static u32 layout_values(const struct replica_layout *l) {
  return 10 * (l->cores + 9) + 12 + l->frequencies * l->frequency_bits +
         28 * l->interfaces;
}

static int valid_layout(const struct replica_layout *l, const u8 *metadata) {
  if (l->points < 16 || l->points > 10000 || l->interval < 100 ||
      l->interval > OPTION_MAX_MS || !l->cores || l->cores > 1048576 ||
      l->frequencies > l->cores || l->frequencies > 65535 || l->interfaces > 65535 ||
      !l->id_bits || l->id_bits > 32 || !l->frequency_bits || l->frequency_bits > 16 ||
      l->metrics != l->cores + 10 + l->frequencies + 2 * l->interfaces)
    return 0;
  u32 at = (l->cores * l->id_bits + l->frequencies * (l->id_bits + 1) + 7) / 8;
  if (at > l->metadata_bytes || l->metadata_bytes - at > l->interfaces * 16)
    return 0;
  for (u32 i = 0; i < l->interfaces; ++i) {
    if (at >= l->metadata_bytes)
      return 0;
    u32 bytes = metadata[at++];
    if (!bytes || bytes > 15 || bytes > l->metadata_bytes - at)
      return 0;
    at += bytes;
  }
  return at == l->metadata_bytes;
}

static int same_layout(const struct replica_layout *a, const struct replica_layout *b) {
  return a->points == b->points && a->interval == b->interval && a->cores == b->cores &&
         a->metrics == b->metrics && a->id_bits == b->id_bits && a->interfaces == b->interfaces &&
         a->frequencies == b->frequencies && a->frequency_bits == b->frequency_bits &&
         a->metadata_bytes == b->metadata_bytes && a->metadata_hash == b->metadata_hash;
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
  struct replica_layout layout = {
      load_le32(h + 12), load_le32(h + 24), load_le32(h + 36), load_le32(h + 40),
      load_le32(h + 48), load_le32(h + 52), h[56] | (u32)h[57] << 8, h[58],
      header_bytes - HEADER_BYTES, hash(h + HEADER_BYTES, header_bytes - HEADER_BYTES)};
  if (h[59] || load_le32(h + 16) != spans[tier] ||
      !valid_layout(&layout, h + HEADER_BYTES) || (a->rings && !same_layout(&layout, &a->layout)))
    return 0;
  struct ring *r = &a->ring[tier];
  r->points = layout.points;
  r->width = load_le32(h + 20);
  r->record_bytes = load_le32(h + 28);
  r->header_bytes = header_bytes;
  u64 size = header_bytes + (u64)(r->points - 1) * r->record_bytes;
  if (r->width != history_width(layout.points, layout.interval, tier) ||
      r->record_bytes != bucket_bytes(layout.metrics, layout_values(&layout),
                                    32 + layout.frequencies * layout.frequency_bits,
                                    bits_needed(r->width - 1)) ||
      r->record_bytes > FRAME_MAX - 1 || size > RING_MAX_BYTES)
    return 0;
  if (!prepare_ring(r, a->host->dir, filenames[tier], h, size))
    return replica_failed(a, ": cannot prepare replica ring. Will retry.");
  a->layout = layout;
  a->rings |= (u8)(1 << tier);
  return 1;
}

static int resume_probe(struct probe *a) {
  while (a->scan_tier < TIERS) {
    struct ring *r = &a->ring[a->scan_tier];
    if (r->scan_at) {
      if (!scan_ring(r))
        return replica_failed(a, ": cannot scan replica ring. Will retry.");
      return 1;
    }
    ++a->scan_tier;
  }
  a->resume_deadline = 0;
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
  if (a->resume_deadline || tier >= TIERS)
    return 0;
  const struct ring *r = &a->ring[tier];
  const u8 *record = p + 1;
  if (bytes != 1 + r->record_bytes)
    return 0;
  u32 start = load_le32(record);
  if (!start || start % r->width ||
      load_le32(record + r->record_bytes - 4) != hash(record, r->record_bytes - 4))
    return 0;
  u32 links;
  if (regular_file(r->fd, &links) < 0 || links != 1)
    return replica_failed(a, ": replica ring changed. Will retry.");
  u32 slot = start / r->width % (r->points - 1);
  if (!try_positioned(r->fd, (void *)record, r->record_bytes,
                      r->header_bytes + (u64)slot * r->record_bytes, 1))
    return replica_failed(a, ": cannot write replica record. Will retry.");
  return 1;
}

static int compatible_live(const struct probe *a, const u8 *p, u32 bytes) {
  u32 metadata = load_le32(p + 128);
  if (metadata > bytes - LIVE_HEADER_BYTES - 4)
    return 0;
  struct replica_layout layout = {
      load_le32(p + 12), load_le32(p + 16), load_le32(p + 52), load_le32(p + 60),
      load_le32(p + 56), load_le32(p + 108), load_le32(p + 120), load_le32(p + 124),
      metadata, hash(p + LIVE_HEADER_BYTES, metadata)};
  if (!same_layout(&layout, &a->layout))
    return 0;
  u32 at = LIVE_HEADER_BYTES + metadata + (layout.metrics + layout_values(&layout) + 7) / 8;
  for (u32 i = 0; i < TIERS; ++i) {
    if (load_le32(p + 64 + 4 * i) != a->ring[i].width)
      return 0;
    at += a->ring[i].record_bytes;
  }
  u32 pending = load_le32(p + 104), capacity = PENDING_BYTES / a->ring[0].record_bytes;
  if (pending > (capacity ? capacity : 1))
    return 0;
  at += pending * a->ring[0].record_bytes;
  return at < bytes - 4 && p[at] <= HOST_NAME_MAX && at + 1 + p[at] <= bytes - 4 &&
         load_le32(p + 132) <= 65535;
}

static int on_snapshot(struct probe *a, u8 type, const u8 *p, u32 bytes) {
  int live = type == FRAME_LIVE;
  if (a->resume_deadline || bytes < (live ? LIVE_HEADER_BYTES : 28) + 4 ||
      !prefix((const char *)p, live ? "QUECTOPL" : "QUECTOPD") ||
      !(live ? supported(a->greeting, "live format version ", load_le32(p + 8),
                         SCHEMA_OLDEST, QUECTOP_SCHEMA_VERSION)
             : supported(a->greeting, "disk format version ", load_le32(p + 8),
                         DISK_SCHEMA_OLDEST, QUECTOP_DISK_SCHEMA_VERSION)) ||
      load_le32(p + bytes - 4) != hash(p, bytes - 4) ||
      (live ? !compatible_live(a, p, bytes)
            : load_le32(p + 20) > (bytes - 32) / 8 || load_le32(p + 24) > 3))
    return 0;
  if (live) {
    u32 publish = load_le32(p + 20);
    if (publish < 100 || publish > OPTION_MAX_MS)
      return 0;
    u32 idle = transport_timeout_ms(publication_spacing(a->layout.interval, publish));
    if (idle > a->timeout_ms)
      a->timeout_ms = idle;
  }
  const char *temporary = live ? "." LIVE_FILE ".tmp" : "." DISKS_FILE ".tmp";
  if (!replace_file(a->host->dir, temporary, live ? LIVE_FILE : DISKS_FILE, p, bytes))
    return replica_failed(a, ": cannot publish replica snapshot. Will retry.");
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
static int message_size_ok(u8 phase, u32 bytes) {
  if (phase == PHASE_OPEN)
    return bytes >= 7 && bytes <= 6 + HOST_ID_MAX;
  if (phase == PHASE_HANDSHAKE)
    return bytes >= NOISE_HANDSHAKE_MIN + 5 &&
           bytes <= NOISE_HANDSHAKE_MIN + 5 + HOST_NAME_MAX;
  return bytes >= FRAME_HEADER + NOISE_TAG;
}
static void read_probe(struct probe *a) {
  u64 now = clock_ns(1);
  if (now >= probe_deadline(a)) {
    close_probe(a);
    return;
  }
  long n = call4(N_read, a->fd, (long)(a->buffer + a->have),
                 PROBE_BUFFER - a->have, 0);
  if (n == -4 || n == -11)
    return;
  if (n <= 0) {
    close_probe(a);
    return;
  }
  a->have += (u32)n;
  u32 at = 0;
  while (a->have - at >= MESSAGE_HEADER) {
    u8 *m = a->buffer + at + MESSAGE_HEADER;
    u32 bytes = (u32)m[-2] | (u32)m[-1] << 8;
    if (!message_size_ok(a->phase, bytes)) {
      close_probe(a);
      return;
    }
    if (a->have - at < MESSAGE_HEADER + bytes)
      break;
    if (!on_message(a, m, bytes)) {
      close_probe(a);
      return;
    }
    if (a->host)
      a->last_ns = now;
    at += MESSAGE_HEADER + bytes;
  }
  a->have -= at;
  if (!a->host || !a->have)
    a->frame_deadline = 0;
  else if (at || !a->frame_deadline)
    a->frame_deadline = now + FRAME_TIMEOUT_NS;
  memcpy(a->buffer, a->buffer + at, a->have); /* forward copy; dst < src */
}
