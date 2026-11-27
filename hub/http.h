/* Optional static file server for DATA (--http): GET and HEAD, one response
 * per connection. Any static server can serve DATA instead. Only these URL
 * shapes map to files, so temporary and hidden files are never served:
 *   /               -> index.html
 *   /<name.ext>     -> a top-level file
 *   /h/<id>/<file>  -> a host file
 * Every segment is [A-Za-z0-9_.-] and never starts with a dot. */
#define MAX_CLIENTS 32
#define CLIENT_BUFFER 2048
#define CLIENT_TIMEOUT_NS 10000000000ULL

struct client {
  long fd, file; /* file: body source when >= 0 */
  u64 deadline, offset, remaining;
  u32 have, head_bytes, head_sent;
  char *buffer; /* request, then response head */
};
static struct client clients[MAX_CLIENTS];
/* Apart from the slots, so idle buffers are never touched. */
static char client_buffers[MAX_CLIENTS][CLIENT_BUFFER];
static u8 chunk[16384];
static const char *cors_origin; /* --cors: Access-Control-Allow-Origin value */
#define CORS_MAX 256

/* One origin or "*": visible ASCII, short enough for the response head. */
static int valid_origin(const char *s) {
  u32 n = 0;
  for (; s[n]; ++n)
    if ((u8)s[n] <= 0x20 || (u8)s[n] >= 0x7f || n == CORS_MAX)
      return 0;
  return n > 0;
}

static void close_client(struct client *c) {
  call4(N_close, c->fd, 0, 0, 0);
  if (c->file >= 0)
    call4(N_close, c->file, 0, 0, 0);
  c->fd = c->file = -1;
}
static void accept_client(long listener) {
  long fd = call4(N_accept, listener, 0, 0, 0x80800); /* NONBLOCK|CLOEXEC */
  if (fd < 0)
    return;
  for (u32 i = 0; i < MAX_CLIENTS; ++i)
    if (clients[i].fd < 0) {
      struct client *c = &clients[i];
      c->fd = fd;
      c->file = -1;
      c->have = c->head_bytes = c->head_sent = 0;
      c->deadline = clock_ns(1) + CLIENT_TIMEOUT_NS;
      return;
    }
  call4(N_close, fd, 0, 0, 0);
}

static void respond(struct client *c, const char *status, const char *type,
                    u32 bytes, int head_only) {
  char *p = append(c->buffer, "HTTP/1.1 ");
  p = append(p, status);
  p = append(p, "\r\nContent-Length: ");
  p = decimal(p, bytes);
  if (type) {
    p = append(p, "\r\nContent-Type: ");
    p = append(p, type);
  }
  if (cors_origin) {
    p = append(p, "\r\nAccess-Control-Allow-Origin: ");
    p = append(p, cors_origin);
  }
  p = append(p, "\r\nCache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\n"
                "Content-Security-Policy: default-src 'self'; style-src 'self' "
                "'unsafe-inline'; frame-ancestors 'none'\r\n"
                "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n");
  c->head_bytes = (u32)(p - c->buffer);
  c->remaining = head_only ? 0 : bytes;
  c->offset = 0;
}

/* Returns the segment length, or 0 when it is not servable. */
static u32 segment(const char *s, const char *end, int *dotted) {
  u32 n = 0;
  *dotted = 0;
  for (; s + n < end && s[n] != '/'; ++n) {
    char c = s[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.'))
      return 0;
    *dotted |= c == '.';
  }
  return n && s[0] != '.' ? n : 0;
}
static const char *content_type(const char *name, u32 bytes) {
  static const struct {
    const char *suffix, *type;
  } types[] = {{".html", "text/html; charset=utf-8"},
               {".css", "text/css; charset=utf-8"},
               {".js", "text/javascript; charset=utf-8"},
               {".txt", "text/plain; charset=utf-8"}};
  for (u32 i = 0; i < sizeof(types) / sizeof(*types); ++i) {
    u32 n = (u32)length(types[i].suffix);
    if (bytes > n && prefix(name + bytes - n, types[i].suffix))
      return types[i].type;
  }
  return "application/octet-stream";
}
/* Writes the file path, relative to DATA, for a URL path; 0 if none. */
static int map_path(const char *path, const char *end, char *out) {
  if (end - path == 1) {
    memcpy(out, "index.html", 11);
    return 1;
  }
  int dotted;
  u32 first = segment(++path, end, &dotted);
  if (first && path + first == end && dotted) {
    memcpy(out, path, first);
    out[first] = 0;
    return 1;
  }
  if (first != 1 || path[0] != 'h' || path[1] != '/')
    return 0;
  const char *id = path + 2;
  u32 id_bytes = segment(id, end, &dotted);
  const char *file = id + id_bytes + 1;
  u32 file_bytes = id_bytes && file < end ? segment(file, end, &dotted) : 0;
  if (!file_bytes || file + file_bytes != end || !dotted)
    return 0;
  memcpy(out, path, (unsigned long)(end - path));
  out[end - path] = 0;
  return 1;
}
static long open_http_file(char *name) {
  long flags = RDONLY | NONBLOCK | NOFOLLOW;
  if (name[0] != 'h' || name[1] != '/')
    return openat(data_fd, name, flags, 0);

  long root = openat(data_fd, "h", RDONLY | DIRECTORY | NOFOLLOW, 0);
  if (root < 0)
    return root;
  char *leaf = name + 2;
  while (*leaf != '/')
    ++leaf;
  *leaf = 0;
  long dir = openat(root, name + 2, RDONLY | DIRECTORY | NOFOLLOW, 0);
  *leaf = '/';
  call4(N_close, root, 0, 0, 0);
  if (dir < 0)
    return dir;
  long fd = openat(dir, leaf + 1, flags, 0);
  call4(N_close, dir, 0, 0, 0);
  return fd;
}
static void route(struct client *c, const char *end) {
  const char *s = c->buffer;
  int head_only = prefix(s, "HEAD ");
  if (!head_only && !prefix(s, "GET ")) {
    respond(c, "405 Method Not Allowed", 0, 0, 1);
    return;
  }
  const char *path = s + (head_only ? 5 : 4), *stop = path;
  while (stop < end && *stop != ' ' && *stop != '?' && *stop != '\r')
    ++stop;
  char name[CLIENT_BUFFER];
  long fd = -1;
  i64 size = -1;
  if (*path == '/' && map_path(path, stop, name)) {
    fd = open_http_file(name);
    if (fd >= 0 && regular_file(fd, 0) >= 0)
      size = seek_file(fd, 0, 2);
  }
  if (size < 0 || size > 0xffffffffLL) {
    if (fd >= 0)
      call4(N_close, fd, 0, 0, 0);
    respond(c, "404 Not Found", 0, 0, 1);
    return;
  }
  c->file = fd;
  respond(c, "200 OK", content_type(name, (u32)length(name)), (u32)size, head_only);
}

static void read_client(struct client *c) {
  long n = call4(N_read, c->fd, (long)(c->buffer + c->have),
                 CLIENT_BUFFER - 1 - c->have, 0);
  if (n == -4 || n == -11)
    return;
  if (n <= 0) {
    close_client(c);
    return;
  }
  c->have += (u32)n;
  c->buffer[c->have] = 0;
  for (u32 i = 3; i < c->have; ++i)
    if (c->buffer[i] == '\n' && c->buffer[i - 1] == '\r' &&
        c->buffer[i - 2] == '\n' && c->buffer[i - 3] == '\r') {
      route(c, c->buffer + i);
      return;
    }
  if (c->have == CLIENT_BUFFER - 1)
    respond(c, "431 Request Header Fields Too Large", 0, 0, 1);
}
static void write_client(struct client *c) {
  for (;;) {
    const u8 *data;
    u64 bytes;
    if (c->head_sent < c->head_bytes) {
      data = (const u8 *)c->buffer + c->head_sent;
      bytes = c->head_bytes - c->head_sent;
    } else if (c->remaining) {
      bytes = c->remaining < sizeof(chunk) ? c->remaining : sizeof(chunk);
      long got = file_io(N_pread, c->file, chunk, (unsigned long)bytes, c->offset);
      if (got <= 0) {
        close_client(c);
        return;
      }
      bytes = (u64)got;
      data = chunk;
    } else {
      close_client(c);
      return;
    }
    long n = call6(N_send, c->fd, (long)data, (long)bytes, SEND_FLAGS, 0, 0);
    if (n == -4)
      continue;
    if (n == -11)
      return;
    if (n <= 0) {
      close_client(c);
      return;
    }
    if (c->head_sent < c->head_bytes) {
      c->head_sent += (u32)n;
    } else {
      c->offset += (u64)n;
      c->remaining -= (u64)n;
    }
  }
}
