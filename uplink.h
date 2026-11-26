/* Optional replication to quectop-hub (--hub). Without it, the main loop only
 * sleeps and the hooks store a length or return.
 *
 * The probe opens a Noise handshake keyed by the device key (--key-file,
 * protocol.h); frames go out only after the hub proves it holds the same key.
 * After the ring headers, the hub replies with the newest bucket it holds per
 * tier. Each tier then backfills oldest-first from the local rings,
 * so an interrupted backfill resumes where it stopped. A tier still
 * backfilling ignores new buckets; its cursor reaches them. A hub name is
 * looked up again before every connect, without blocking sampling. */
#define OUTBOX_BYTES 32768
#define HUB_RETRY_MIN_NS 1000000000ULL
#define HUB_RETRY_MAX_NS 60000000000ULL
#define HUB_RESOLVE_NS 3000000000ULL
enum { HUB_OFF, HUB_IDLE, HUB_RESOLVING, HUB_CONNECTING, HUB_GREETING,
       HUB_RESUMING, HUB_STREAMING };
enum { OWE_LIVE = 1, OWE_DISKS = 2 };

static struct endpoint hub_endpoint;
static char hub_name[HOST_NAME_TEXT]; /* empty: hub_endpoint is literal */
static u32 hub_name_bytes, hub_nameserver;
static u16 hub_query;
static long hub_fd = -1;
static u8 hub_state, hub_owed, hub_received;
/* Noise message 2, then the sealed RESUME. */
#define HUB_REPLY_MAX (MESSAGE_HEADER + NOISE_HANDSHAKE_MIN)
#define SEALED_EXTRA (MESSAGE_HEADER + NOISE_TAG)
static u8 hub_reply[HUB_REPLY_MAX];
static struct noise hub_handshake;
static struct noise_cipher to_hub, from_hub;
/* Untouched .bss pages stay unmapped unless --hub is used. */
static u8 outbox[OUTBOX_BYTES];
static u32 outbox_start, outbox_end, live_bytes;
/* Next bucket start to send per tier; 0 once caught up. */
static u32 hub_cursor[TIERS];
/* Next connect attempt; while resolving, the lookup's deadline. */
static u64 hub_retry_at, hub_backoff = HUB_RETRY_MIN_NS;
static char host_id[HOST_ID_MAX];
static u8 host_id_bytes;
static const char *key_file;
static u8 device_key[KEY_BYTES]; /* zero without --key-file */

/* The key file's "ID HEX" line sets both the host id and the key. */
static void read_key_file(void) {
  struct lines r = open_lines(key_file);
  check(r.fd, "cannot open --key-file");
  if (!private_file(r.fd))
    fail("--key-file is readable by other users; run chmod 600 on it");
  const char *line, *id;
  u32 bytes, id_bytes = 0;
  while (next_line(&r, &line, &bytes) &&
         !parse_key_line(line, bytes, &id, &id_bytes, device_key))
    id_bytes = 0;
  close_lines(&r);
  if (!id_bytes)
    fail("--key-file needs an \"ID HEX\" line from quectop-hub --add-device");
  memcpy(host_id, id, id_bytes);
  host_id_bytes = (u8)id_bytes;
}
/* Host id: /etc/machine-id when present, otherwise the hostname. */
static void read_host_id(void) {
  long fd = openat(AT_CWD, "/etc/machine-id", RDONLY, 0);
  long n = fd < 0 ? -1 : call4(N_read, fd, (long)input, 33, 0);
  if (fd >= 0)
    call4(N_close, fd, 0, 0, 0);
  int hex = n >= 32;
  for (u32 i = 0; hex && i < 32; ++i)
    hex = (input[i] >= '0' && input[i] <= '9') ||
          (input[i] >= 'a' && input[i] <= 'f');
  host_id_bytes = hex ? 32 : host_name_bytes;
  memcpy(host_id, hex ? input : host_name, host_id_bytes);
  if (!host_id_bytes)
    fail("--hub needs /etc/machine-id or a hostname");
}

/* Returns room for a frame of bytes, sealed in place by outbox_seal, compacting
 * first if needed. */
static u8 *outbox_reserve(u32 bytes) {
  bytes += SEALED_EXTRA;
  if (OUTBOX_BYTES - outbox_end < bytes) {
    u32 used = outbox_end - outbox_start;
    if (OUTBOX_BYTES - used < bytes)
      return 0;
    memcpy(outbox, outbox + outbox_start, used); /* forward copy; dst < src */
    outbox_start = 0;
    outbox_end = used;
  }
  return outbox + outbox_end + MESSAGE_HEADER;
}
static void outbox_seal(u32 bytes) {
  u8 *m = outbox + outbox_end;
  u32 sealed = bytes + NOISE_TAG;
  m[0] = (u8)sealed;
  m[1] = (u8)(sealed >> 8);
  noise_seal(&to_hub, 0, 0, m + MESSAGE_HEADER, bytes);
  outbox_end += MESSAGE_HEADER + sealed;
}
static int outbox_frame(u8 type, const void *head, u32 head_bytes,
                        const void *body, u32 body_bytes) {
  u32 bytes = head_bytes + body_bytes;
  u8 *p = outbox_reserve(FRAME_HEADER + bytes);
  if (!p)
    return 0;
  frame_header(p, type, bytes);
  memcpy(p + FRAME_HEADER, head, head_bytes);
  memcpy(p + FRAME_HEADER + head_bytes, body, body_bytes);
  outbox_seal(FRAME_HEADER + bytes);
  return 1;
}

static void hub_disconnect(void) {
  if (hub_fd >= 0)
    call4(N_close, hub_fd, 0, 0, 0);
  hub_fd = -1;
  hub_state = HUB_IDLE;
  outbox_start = outbox_end = hub_received = 0;
  crypto_wipe(&hub_handshake, sizeof(hub_handshake));
  crypto_wipe(&to_hub, sizeof(to_hub));
  crypto_wipe(&from_hub, sizeof(from_hub));
  hub_retry_at = clock_ns(1) + hub_backoff;
  hub_backoff = hub_backoff * 2 < HUB_RETRY_MAX_NS ? hub_backoff * 2
                                                   : HUB_RETRY_MAX_NS;
}
/* "HOST:PORT", HOST being IPv4 or a name. */
static int parse_hub(const char *s) {
  u32 colon = (u32)length(s);
  while (colon && s[colon - 1] != ':')
    --colon;
  if (!colon || !parse_port(s + colon, &hub_endpoint))
    return 0;
  u32 bytes = colon - 1;
  hub_endpoint.family = 2;
  if (ipv4_word(s, bytes, hub_endpoint.address))
    return 1;
  if (bytes && s[bytes - 1] == '.')
    --bytes;
  if (!valid_hostname(s, bytes))
    return 0;
  memcpy(hub_name, s, bytes);
  hub_name_bytes = bytes;
  return 1;
}
static void hub_open(void) {
  hub_fd = call4(N_socket, 2, SOCKET_STREAM, 0, 0);
  if (hub_fd < 0) {
    hub_disconnect();
    return;
  }
  int one = 1; /* IPPROTO_TCP, TCP_NODELAY: frames are already batched. */
  call6(N_setsockopt, hub_fd, 6, 1, (long)&one, sizeof(one), 0);
  /* TCP_USER_TIMEOUT: a hub that stops acknowledging, as on a lost route,
   * drops the stream after the hub's own idle limit instead of after the
   * kernel's retransmission limit of about 15 minutes. */
  u32 unacknowledged_ms = 3 * publish + 60000;
  call6(N_setsockopt, hub_fd, 6, 18, (long)&unacknowledged_ms,
        sizeof(unacknowledged_ms), 0);
  long result = call4(N_connect, hub_fd, (long)&hub_endpoint,
                      sizeof(hub_endpoint), 0);
  if (result < 0 && result != -115) /* EINPROGRESS */
    hub_disconnect();
  else
    hub_state = HUB_CONNECTING;
}
/* The next lookup asks the next nameserver. */
static void hub_resolve_failed(void) {
  ++hub_nameserver;
  hub_disconnect();
}
static void hub_connect(void) {
  if (!hub_name_bytes || hosts_lookup(hub_name, hub_name_bytes,
                                      hub_endpoint.address)) {
    hub_open();
    return;
  }
  struct endpoint server;
  nameserver(hub_nameserver, &server);
  hub_fd = call4(N_socket, 2, SOCKET_DATAGRAM, 0, 0);
  hub_query = (u16)(clock_ns(1) ^ clock_ns(0) >> 7);
  u32 n = dns_query(outbox, hub_query, hub_name, hub_name_bytes);
  if (hub_fd < 0 ||
      call4(N_connect, hub_fd, (long)&server, sizeof(server), 0) < 0 ||
      call6(N_send, hub_fd, (long)outbox, n, SEND_FLAGS, 0, 0) != (long)n) {
    hub_resolve_failed();
    return;
  }
  hub_state = HUB_RESOLVING;
  hub_retry_at = clock_ns(1) + HUB_RESOLVE_NS;
}
static void hub_resolved(void) {
  long n = call4(N_read, hub_fd, (long)outbox, OUTBOX_BYTES, 0);
  if (n == -4 || n == -11)
    return;
  if (n <= 0 || !dns_answer(outbox, (u32)n, hub_query, hub_endpoint.address)) {
    hub_resolve_failed();
    return;
  }
  call4(N_close, hub_fd, 0, 0, 0);
  hub_open();
}
/* Once connected: OPEN, then Noise message 1 carrying publish ms and hostname. */
static void hub_greet(void) {
  u8 *open = outbox + MESSAGE_HEADER, secret[NOISE_KEY];
  u32 open_bytes = 6 + host_id_bytes;
  memcpy(open, "QTOP", 4);
  open[4] = PROTOCOL_VERSION;
  open[5] = host_id_bytes;
  memcpy(open + 6, host_id, host_id_bytes);
  outbox[0] = (u8)open_bytes;
  outbox[1] = 0;
  u8 payload[5 + sizeof(host_name)], *first = open + open_bytes;
  store_le32(payload, publish);
  payload[4] = host_name_bytes;
  memcpy(payload + 5, host_name, host_name_bytes);
  noise_start(&hub_handshake, open, open_bytes, device_key);
  if (call4(N_getrandom, (long)secret, NOISE_KEY, 0, 0) != NOISE_KEY) {
    hub_disconnect();
    return;
  }
  u32 n = noise_write(&hub_handshake, secret, payload, 5u + host_name_bytes,
                      first + MESSAGE_HEADER);
  crypto_wipe(secret, sizeof(secret));
  first[0] = (u8)n;
  first[1] = (u8)(n >> 8);
  outbox_start = 0;
  outbox_end = MESSAGE_HEADER + open_bytes + MESSAGE_HEADER + n;
  hub_state = HUB_GREETING;
}
/* Noise message 2 proves the hub holds the key; only then do frames flow. */
static void hub_handshaken(void) {
  if (noise_read(&hub_handshake, hub_reply + MESSAGE_HEADER, NOISE_HANDSHAKE_MIN) != 0) {
    hub_disconnect();
    return;
  }
  noise_split(&hub_handshake, 1, &to_hub, &from_hub);
  for (u32 i = 0; i < TIERS; ++i) {
    u8 *p = outbox_reserve(FRAME_HEADER + 1 + header_bytes);
    frame_header(p, FRAME_RING, 1 + header_bytes);
    p[FRAME_HEADER] = (u8)i;
    positioned(tiers[i].fd, p + FRAME_HEADER + 1, header_bytes, 0, 0);
    outbox_seal(FRAME_HEADER + 1 + header_bytes);
  }
  hub_state = HUB_RESUMING;
}
static void hub_resume(void) {
  u8 *f = hub_reply + MESSAGE_HEADER;
  if (!noise_open(&from_hub, 0, 0, f, RESUME_BYTES + NOISE_TAG) ||
      f[0] != FRAME_RESUME || f[1] != 4 * TIERS || f[2] || f[3]) {
    hub_disconnect();
    return;
  }
  for (u32 i = 0; i < TIERS; ++i) {
    const struct tier *t = &tiers[i];
    u32 held = load_le32(f + FRAME_HEADER + 4 * i),
        span = (points - 1) * t->width, active = t->active.start,
        oldest = active > span ? active - span : t->width,
        from = held && held + t->width > oldest ? held + t->width : oldest;
    hub_cursor[i] = from < active ? from : 0;
  }
  hub_owed = OWE_LIVE | OWE_DISKS;
  hub_state = HUB_STREAMING;
  hub_backoff = HUB_RETRY_MIN_NS;
}
static void hub_receive(void) {
  u32 expect = hub_state == HUB_GREETING   ? MESSAGE_HEADER + NOISE_HANDSHAKE_MIN
               : hub_state == HUB_RESUMING ? MESSAGE_HEADER + RESUME_BYTES + NOISE_TAG
                                           : 1;
  long n = call4(N_read, hub_fd, (long)(hub_reply + hub_received),
                 expect - hub_received, 0);
  if (n == -4 || n == -11)
    return;
  /* EOF, an error, or bytes after RESUME: the hub sends nothing else. */
  if (n <= 0 || hub_state == HUB_STREAMING) {
    hub_disconnect();
    return;
  }
  hub_received += (u8)n;
  if (hub_received < expect)
    return;
  hub_received = 0;
  if (hub_reply[0] != (u8)(expect - MESSAGE_HEADER) || hub_reply[1])
    hub_disconnect();
  else if (hub_state == HUB_RESUMING)
    hub_resume();
  else
    hub_handshaken();
}

/* Queues backfill records until the outbox is full; 1 once caught up. */
static int hub_backfill(void) {
  for (u32 i = 0; i < TIERS; ++i) {
    const struct tier *t = &tiers[i];
    u32 bytes = t->record_bytes;
    while (hub_cursor[i]) {
      u32 start = hub_cursor[i];
      if (start >= t->active.start) {
        hub_cursor[i] = 0;
        break;
      }
      u8 *p = outbox_reserve(FRAME_HEADER + 1 + bytes);
      if (!p)
        return 0;
      hub_cursor[i] = start + t->width;
      u32 slot = start / t->width % (points - 1);
      u8 *record = p + FRAME_HEADER + 1;
      if (i == 0 && slot - pending_first < pending_count)
        memcpy(record, pending + (slot - pending_first) * bytes, bytes);
      else
        positioned(t->fd, record, bytes, header_bytes + (u64)slot * bytes, 0);
      if (load_le32(record) != start ||
          load_le32(record + bytes - 4) != hash(record, bytes - 4))
        continue;
      frame_header(p, FRAME_RECORD, 1 + bytes);
      p[FRAME_HEADER] = (u8)i;
      outbox_seal(FRAME_HEADER + 1 + bytes);
    }
  }
  return 1;
}
/* Snapshots are replaced by the next one, so a full outbox skips them. */
static void hub_send_snapshots(void) {
  if ((hub_owed & OWE_LIVE) && live_bytes &&
      outbox_frame(FRAME_LIVE, live, live_bytes, 0, 0))
    hub_owed &= (u8)~OWE_LIVE;
  if (!(hub_owed & OWE_DISKS))
    return;
  long fd = openat(live_directory_fd, DISKS_FILE, RDONLY | NOFOLLOW, 0);
  if (fd < 0)
    return;
  i64 size = seek_file(fd, 0, 2);
  u8 *p = 0;
  if (size <= 0 || size > OUTBOX_BYTES - FRAME_HEADER - SEALED_EXTRA)
    hub_owed &= (u8)~OWE_DISKS; /* unsendable; wait for the next snapshot */
  else if ((p = outbox_reserve(FRAME_HEADER + (u32)size)) &&
           file_io(N_pread, fd, p + FRAME_HEADER, (unsigned long)size, 0) == size) {
    frame_header(p, FRAME_DISKS, (u32)size);
    outbox_seal(FRAME_HEADER + (u32)size);
    hub_owed &= (u8)~OWE_DISKS;
  }
  call4(N_close, fd, 0, 0, 0);
}
static void hub_flush(void) {
  while (outbox_start < outbox_end) {
    long n = call6(N_send, hub_fd, (long)(outbox + outbox_start),
                   outbox_end - outbox_start, SEND_FLAGS, 0, 0);
    if (n == -4)
      continue;
    if (n == -11)
      return;
    if (n <= 0) {
      hub_disconnect();
      return;
    }
    outbox_start += (u32)n;
  }
  outbox_start = outbox_end = 0;
}

/* Hooks called while persisting. */
static void uplink_record(u32 tier, const u8 *record) {
  if (hub_state != HUB_STREAMING || hub_cursor[tier])
    return;
  u8 t = (u8)tier;
  if (!outbox_frame(FRAME_RECORD, &t, 1, record, tiers[tier].record_bytes))
    hub_disconnect(); /* reconnecting resumes after the hub's newest bucket */
}
static void uplink_live(u32 bytes) {
  live_bytes = bytes;
  hub_owed |= OWE_LIVE;
}
static void uplink_disks(void) { hub_owed |= OWE_DISKS; }

static void check_uplink_layout(void) {
  if (SEALED_EXTRA + FRAME_HEADER + live_capacity > OUTBOX_BYTES / 2 ||
      TIERS * (SEALED_EXTRA + FRAME_HEADER + 1 + header_bytes) > OUTBOX_BYTES / 2)
    fail("device layout too large for --hub");
  if (key_file)
    read_key_file();
  else
    read_host_id();
}

/* Sleeps until the next sample, streaming to the hub meanwhile. */
static void wait_for_sample(u64 deadline) {
  if (hub_state == HUB_OFF) {
    sleep_until(deadline);
    return;
  }
  for (;;) {
    u64 now = clock_ns(1);
    if (now >= deadline)
      return;
    if (hub_state == HUB_IDLE) {
      if (now >= hub_retry_at)
        hub_connect();
      if (hub_state == HUB_IDLE) {
        sleep_until(hub_retry_at < deadline ? hub_retry_at : deadline);
        continue;
      }
    }
    if (hub_state == HUB_RESOLVING && now >= hub_retry_at) {
      hub_resolve_failed();
      continue;
    }
    if (hub_state == HUB_STREAMING && hub_backfill())
      hub_send_snapshots();
    struct poll_entry e = {(int)hub_fd, POLL_IN, 0};
    if (hub_state == HUB_CONNECTING || outbox_start < outbox_end)
      e.events |= POLL_OUT;
    u64 wake = hub_state == HUB_RESOLVING && hub_retry_at < deadline
                   ? hub_retry_at
                   : deadline;
    if (wait_events(&e, 1, wake) <= 0)
      continue;
    if (hub_state == HUB_RESOLVING) {
      if (e.revents & POLL_FAILED)
        hub_resolve_failed();
      else
        hub_resolved();
      continue;
    }
    if (e.revents & POLL_FAILED) {
      hub_disconnect();
      continue;
    }
    if (hub_state == HUB_CONNECTING) {
      hub_greet(); /* connected */
      continue;
    }
    if (e.revents & POLL_IN)
      hub_receive();
    if (hub_state != HUB_IDLE && (e.revents & POLL_OUT))
      hub_flush();
  }
}
