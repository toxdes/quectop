/* Hub name lookup, the libc way minus plugins: /etc/hosts, then one A query
 * over UDP to a nameserver from /etc/resolv.conf. No search domains. */
#define HOST_NAME_TEXT 254 /* 253 + NUL */
#define NAMESERVERS 3      /* resolv.conf's MAXNS */
#define DNS_PORT 0x3500    /* 53, network byte order */

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
/* Advances past a possibly compressed name; 0 when malformed. */
static u32 skip_name(const u8 *m, u32 bytes, u32 p) {
  while (p < bytes) {
    u8 n = m[p];
    if (!n)
      return p + 1;
    if ((n & 0xc0) == 0xc0)
      return p + 2 <= bytes ? p + 2 : 0;
    if (n > 63)
      return 0;
    p += 1u + n;
  }
  return 0;
}
/* First A record of a successful reply to id. Resolvers follow CNAMEs. */
static int dns_answer(const u8 *m, u32 bytes, u16 id, u8 *address) {
  if (bytes < 12 || m[0] != (u8)(id >> 8) || m[1] != (u8)id ||
      !(m[2] & 0x80) || (m[2] & 0x78) || (m[3] & 0x0f))
    return 0;
  u32 questions = (u32)m[4] << 8 | m[5], answers = (u32)m[6] << 8 | m[7],
      p = 12;
  while (questions--) {
    p = skip_name(m, bytes, p);
    if (!p || p + 4 > bytes)
      return 0;
    p += 4;
  }
  while (answers-- && (p = skip_name(m, bytes, p)) && p + 10 <= bytes) {
    u32 type = (u32)m[p] << 8 | m[p + 1], class = (u32)m[p + 2] << 8 | m[p + 3],
        data = (u32)m[p + 8] << 8 | m[p + 9];
    p += 10;
    if (p + data > bytes)
      return 0;
    if (type == 1 && class == 1 && data == 4) {
      memcpy(address, m + p, 4);
      return 1;
    }
    p += data;
  }
  return 0;
}
