/* Hub name lookup, the libc way minus plugins: /etc/hosts, then one A query
 * over UDP to a nameserver from /etc/resolv.conf. No search domains. */
#define HOST_NAME_TEXT 254 /* 253 + NUL */
#define NAMESERVERS 3      /* resolv.conf's MAXNS */
#define DNS_PORT 0x3500    /* 53, network byte order */
#define DNS_PACKET_BYTES 512 /* UDP without EDNS */
#define DNS_NAME_BYTES 255

static int same_name(const char *a, u32 a_bytes, const char *b, u32 b_bytes) {
  if (a_bytes && a[a_bytes - 1] == '.')
    --a_bytes;
  if (a_bytes != b_bytes)
    return 0;
  for (u32 i = 0; i < a_bytes; ++i)
    if ((a[i] | 0x20) != (b[i] | 0x20))
      return 0;
  return 1;
}
/* An IPv4 word: exactly a.b.c.d. */
static int ipv4_word(const char *s, u32 bytes, u8 *address) {
  const char *p = s;
  return bytes && bytes < 16 && parse_ipv4(&p, address) && p == s + bytes;
}

/* Hostname, without a trailing dot: labels of [A-Za-z0-9_-], 1..63 bytes. */
static int valid_hostname(const char *s, u32 bytes) {
  if (!bytes || bytes >= HOST_NAME_TEXT)
    return 0;
  for (u32 i = 0, label = 0; i <= bytes; ++i) {
    if (i == bytes || s[i] == '.') {
      if (!label || label > 63)
        return 0;
      label = 0;
      continue;
    }
    char c = s[i];
    if (!((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ||
          c == '-' || c == '_'))
      return 0;
    ++label;
  }
  return 1;
}

static int hosts_lookup(const char *name, u32 bytes, u8 *address) {
  struct lines r = open_lines("/etc/hosts");
  const char *line, *w;
  u32 n, found = 0;
  while (!found && next_line(&r, &line, &n)) {
    u32 at = 0, w_bytes = word(line, n, &at, &w);
    u8 candidate[4];
    if (!ipv4_word(w, w_bytes, candidate))
      continue;
    while (!found && (w_bytes = word(line, n, &at, &w)))
      found = same_name(w, w_bytes, name, bytes);
    if (found)
      memcpy(address, candidate, 4);
  }
  close_lines(&r);
  return (int)found;
}
/* The index-th IPv4 nameserver, wrapping; 127.0.0.1 when none is listed. */
static void nameserver(u32 index, struct endpoint *e) {
  u8 servers[NAMESERVERS][4] = {{127, 0, 0, 1}};
  u32 count = 0;
  struct lines r = open_lines("/etc/resolv.conf");
  const char *line, *w;
  u32 n;
  while (count < NAMESERVERS && next_line(&r, &line, &n)) {
    u32 at = 0, w_bytes = word(line, n, &at, &w);
    if (same_name(w, w_bytes, "nameserver", 10)) {
      w_bytes = word(line, n, &at, &w);
      count += (u32)ipv4_word(w, w_bytes, servers[count]);
    }
  }
  close_lines(&r);
  memset(e, 0, sizeof(*e));
  e->family = 2;
  e->port = DNS_PORT;
  memcpy(e->address, servers[count ? index % count : 0], 4);
}

/* An A/IN query with recursion desired; returns its length. */
static u32 dns_query(u8 *out, u16 id, const char *name, u32 bytes) {
  static const u8 flags[10] = {1, 0, 0, 1}; /* RD; 1 question */
  out[0] = (u8)(id >> 8);
  out[1] = (u8)id;
  memcpy(out + 2, flags, sizeof(flags));
  u32 n = 12;
  for (u32 i = 0; i < bytes;) {
    u32 label = 0;
    while (i + label < bytes && name[i + label] != '.')
      ++label;
    out[n++] = (u8)label;
    memcpy(out + n, name + i, label);
    n += label;
    i += label + 1;
  }
  static const u8 tail[5] = {0, 0, 1, 0, 1}; /* root, type A, class IN */
  memcpy(out + n, tail, sizeof(tail));
  return n + sizeof(tail);
}
/* Expands a wire name and returns the next offset, or zero on failure. */
static u32 dns_name(const u8 *m, u32 bytes, u32 at, u8 *name, u32 *written) {
  u32 next = 0, used = 0, limit = bytes;
  while (at < limit) {
    u32 n = m[at++];
    if ((n & 0xc0) == 0xc0) {
      if (at == limit)
        return 0;
      u32 target = ((n & 63) << 8) | m[at];
      if (target < 12 || target >= at - 1)
        return 0;
      if (!next)
        next = at + 1;
      limit = at - 1;
      at = target;
      continue;
    }
    if (n > 63 || n > limit - at || used + 1 + n > DNS_NAME_BYTES)
      return 0;
    name[used++] = (u8)n;
    if (!n) {
      *written = used;
      return next ? next : at;
    }
    memcpy(name + used, m + at, n);
    used += n;
    at += n;
  }
  return 0;
}

static int dns_equal(const u8 *a, const u8 *b, u32 bytes) {
  for (u32 i = 0; i < bytes; ++i) {
    u8 x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z')
      x |= 32;
    if (y >= 'A' && y <= 'Z')
      y |= 32;
    if (x != y)
      return 0;
  }
  return 1;
}

static int dns_answer(const u8 *m, u32 bytes, const u8 *query,
                      u32 query_name_bytes, u8 *address) {
  if (bytes < 12 || bytes > DNS_PACKET_BYTES || m[0] != query[0] ||
      m[1] != query[1] || (m[2] & 0xfa) != 0x80 || (m[3] & 0x0f) ||
      m[4] || m[5] != 1)
    return 0;
  u8 wanted[DNS_NAME_BYTES], owner[DNS_NAME_BYTES];
  u32 wanted_bytes, first = dns_name(m, bytes, 12, wanted, &wanted_bytes);
  if (!first || first + 4 > bytes || wanted_bytes != query_name_bytes ||
      !dns_equal(wanted, query + 12, wanted_bytes) || m[first] ||
      m[first + 1] != 1 || m[first + 2] || m[first + 3] != 1)
    return 0;
  first += 4;
  /* Restart from the first answer for each alias, regardless of record order. */
  for (u32 depth = 0; depth <= 8; ++depth) {
    u32 alias = 0, alias_end = 0, at = first;
    for (u32 n = (u32)m[6] << 8 | m[7]; n; --n) {
      u32 owner_bytes;
      at = dns_name(m, bytes, at, owner, &owner_bytes);
      if (!at || at + 10 > bytes)
        return 0;
      u32 type = (u32)m[at] << 8 | m[at + 1];
      u32 klass = (u32)m[at + 2] << 8 | m[at + 3];
      u32 length = (u32)m[at + 8] << 8 | m[at + 9];
      at += 10;
      if (length > bytes - at)
        return 0;
      if (klass == 1 && owner_bytes == wanted_bytes && dns_equal(owner, wanted, owner_bytes)) {
        if (type == 1 && length == 4) {
          memcpy(address, m + at, 4);
          return 1;
        }
        if (type == 5) {
          if (alias)
            return 0;
          alias = at;
          alias_end = at + length;
        }
      }
      at += length;
    }
    if (!alias || dns_name(m, bytes, alias, wanted, &wanted_bytes) != alias_end)
      return 0;
  }
  return 0;
}
