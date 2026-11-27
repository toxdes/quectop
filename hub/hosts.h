/* Host table. DATA/hosts.txt is its persistent form and the dashboard's host
 * index: one line per host, "id TAB connected(0/1) TAB hostname". Each host's
 * files live in DATA/h/<id>/. */
#define MAX_HOSTS 64
#define HOST_NAME_MAX 64
#define HOSTS_TEXT_MAX (MAX_HOSTS * (HOST_ID_MAX + 3 + HOST_NAME_MAX + 1))
#define STORAGE_RETRY_NS 5000000000ULL

struct probe;
struct host {
  long dir;
  struct probe *probe; /* connected stream, if any */
  u64 retry_at;
  char id[HOST_ID_MAX + 1];
  char name[HOST_NAME_MAX];
  u8 id_bytes, name_bytes;
};
static struct host hosts[MAX_HOSTS];
static u32 host_count;
static long data_fd, hosts_fd;
static u64 hosts_retry_at;
static char hosts_text[HOSTS_TEXT_MAX];

static int valid_name(const char *name, u32 bytes) {
  if (bytes > HOST_NAME_MAX)
    return 0;
  for (u32 i = 0; i < bytes; ++i)
    if ((u8)name[i] < 0x20 || name[i] == 0x7f)
      return 0;
  return 1;
}
static struct host *find_host(const char *id, u32 bytes) {
  for (u32 i = 0; i < host_count; ++i)
    if (hosts[i].id_bytes == bytes) {
      u32 j = 0;
      while (j < bytes && hosts[i].id[j] == id[j])
        ++j;
      if (j == bytes)
        return &hosts[i];
    }
  return 0;
}
/* Opens or creates DATA/h/<id>; 0 when the table is full. */
static struct host *add_host(const char *id, u32 bytes) {
  if (host_count == MAX_HOSTS)
    return 0;
  struct host *h = &hosts[host_count];
  memcpy(h->id, id, bytes);
  h->id[bytes] = 0;
  h->id_bytes = (u8)bytes;
  h->name_bytes = 0;
  long result = call4(N_mkdir, hosts_fd, (long)h->id, 0755, 0);
  if (result < 0 && result != -17) /* EEXIST */
    return 0;
  h->dir = openat(hosts_fd, h->id, RDONLY | DIRECTORY | NOFOLLOW, 0);
  if (h->dir < 0)
    return 0;
  ++host_count;
  return h;
}
static int publish_output(long dir, const char *temporary, const char *name,
                          long fd, int complete) {
  if (call4(N_close, fd, 0, 0, 0) == 0 && complete &&
      call4(N_rename, dir, (long)temporary, dir, (long)name) == 0)
    return 1;
  call4(N_unlink, dir, (long)temporary, 0, 0);
  return 0;
}

/* Atomically replaces dir/name with data. */
static int replace_file(long dir, const char *temporary, const char *name,
                        const void *data, u32 bytes) {
  long fd = temporary_output(dir, temporary);
  if (fd < 0)
    return 0;
  int complete = try_write_all(fd, data, bytes);
  return publish_output(dir, temporary, name, fd, complete);
}

static void publish_hosts(void) {
  char *p = hosts_text;
  for (u32 i = 0; i < host_count; ++i) {
    const struct host *h = &hosts[i];
    memcpy(p, h->id, h->id_bytes);
    p += h->id_bytes;
    *p++ = '\t';
    *p++ = h->probe ? '1' : '0';
    *p++ = '\t';
    memcpy(p, h->name, h->name_bytes);
    p += h->name_bytes;
    *p++ = '\n';
  }
  if (replace_file(data_fd, ".hosts.txt.tmp", "hosts.txt", hosts_text,
                   (u32)(p - hosts_text))) {
    hosts_retry_at = 0;
  } else {
    if (!hosts_retry_at) {
      static const char message[] = PROGRAM_NAME ": cannot publish hosts.txt. Will retry.\n";
      call4(N_write, 2, (long)message, sizeof(message) - 1, 0);
    }
    hosts_retry_at = clock_ns(1) + STORAGE_RETRY_NS;
  }
}

/* A pending retry will publish the latest host table. */
static void write_hosts(void) {
  if (!hosts_retry_at)
    publish_hosts();
}
/* Restores hosts from an earlier run, all offline; skips damaged lines. */
static void load_hosts(void) {
  long result = call4(N_mkdir, data_fd, (long)"h", 0755, 0);
  if (result < 0 && result != -17)
    fail("cannot create host directory");
  hosts_fd = openat(data_fd, "h", RDONLY | DIRECTORY | NOFOLLOW, 0);
  check(hosts_fd, "cannot open host directory");
  long fd = openat(data_fd, "hosts.txt", RDONLY | NOFOLLOW, 0);
  long n = fd < 0 ? 0 : call4(N_read, fd, (long)hosts_text, HOSTS_TEXT_MAX, 0);
  if (fd >= 0)
    call4(N_close, fd, 0, 0, 0);
  for (long at = 0; n > 0 && at < n;) {
    long end = at;
    while (end < n && hosts_text[end] != '\n')
      ++end;
    const char *line = hosts_text + at;
    u32 id_bytes = 0, line_bytes = (u32)(end - at);
    while (id_bytes < line_bytes && line[id_bytes] != '\t')
      ++id_bytes;
    const char *name = line + id_bytes + 3;
    u32 name_bytes = line_bytes >= id_bytes + 3 ? line_bytes - id_bytes - 3 : 0;
    struct host *h;
    if (end < n && line_bytes >= id_bytes + 3 && line[id_bytes + 2] == '\t' &&
        valid_id(line, id_bytes) && valid_name(name, name_bytes) &&
        !find_host(line, id_bytes) && (h = add_host(line, id_bytes))) {
      memcpy(h->name, name, name_bytes);
      h->name_bytes = (u8)name_bytes;
    }
    at = end + 1;
  }
  write_hosts();
}
