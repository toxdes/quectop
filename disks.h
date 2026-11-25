/* Mounted-filesystem discovery and atomic capacity snapshots. */
/* Queried by path: an open fd would make umount fail with EBUSY. */
struct disk {
  u64 identity;
  char *source, *mount;
  u8 valid;
};
static struct disk *disks;
static void *disk_mapping;
static unsigned long disk_capacity, disk_mapping_bytes;
static u32 disk_count, disk_slots;
static const char *disk_path;
static u64 disks_generated_ms, last_disk;
static u32 disk_refresh_ms = 600000;

static char *field_token(char **line) {
  char *p = *line;
  while (*p == ' ' || *p == '\t')
    ++p;
  if (!*p)
    return 0;
  char *start = p;
  while (*p && *p != ' ' && *p != '\t')
    ++p;
  if (*p)
    *p++ = 0;
  *line = p;
  return start;
}
static void unescape_mount(char *s) {
  char *dest = s;
  while (*s) {
    if (*s == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' &&
        s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
      *dest++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + s[3] - '0');
      s += 4;
    } else
      *dest++ = *s++;
  }
  *dest = 0;
}
static int local_mount(char *line, char **source, char **mount) {
  *source = field_token(&line);
  *mount = field_token(&line);
  char *type = field_token(&line);
  if (!*source || !*mount || !type ||
      (!prefix(*source, "/dev/") && !equal(type, "zfs")))
    return 0;
  unescape_mount(*source);
  unescape_mount(*mount);
  return !disk_path || equal(disk_path, *mount);
}
static unsigned long measure_disks(u32 *slots) {
  *slots = 0;
  unsigned long bytes = 0;
  stream_rewind(mounts_fd);
  const char *line;
  while ((line = stream_line())) {
    char *source, *mount;
    if (local_mount((char *)line, &source, &mount)) {
      bytes += sizeof(struct disk) + length(source) + length(mount) + 2;
      if (++*slots > 65535)
        fail("too many mounted filesystems");
    }
  }
  return bytes;
}
static int mounts_retry;
static void discover_disks(void) {
  u32 slots;
  mounts_truncated = 0;
  unsigned long wanted = measure_disks(&slots);
  disk_count = 0;
  if (wanted > disk_capacity) {
    long mapped = call6(N_mmap, 0, (long)wanted, 3, 0x22, -1, 0);
    check_mapping(mapped, "cannot resize mounted-filesystem storage");
    if (disk_mapping)
      call4(N_unmap, (long)disk_mapping, (long)disk_mapping_bytes, 0, 0);
    disks = disk_mapping = (void *)mapped;
    disk_capacity = disk_mapping_bytes = wanted;
  }
  disk_slots = slots;
  char *names = (char *)(disks + slots), *end = (char *)disks + disk_capacity;
  stream_rewind(mounts_fd);
  const char *line;
  mounts_retry = 0;
  while ((line = stream_line())) {
    char *source, *mount;
    if (!local_mount((char *)line, &source, &mount))
      continue;
    unsigned long source_bytes = length(source) + 1,
                  mount_bytes = length(mount) + 1;
    if (disk_count == slots ||
        (unsigned long)(end - names) < source_bytes + mount_bytes) {
      mounts_retry = 1;
      continue;
    }
    struct fsinfo fs;
    int ok = filesystem_info(mount, &fs) >= 0;
    u64 identity = ok ? (u64)(u32)fs.fsid[0] | ((u64)(u32)fs.fsid[1] << 32) : 0;
    int duplicate = 0;
    for (u32 i = 0; i < disk_count; ++i)
      if ((identity && disks[i].identity == identity) ||
          equal(disks[i].source, source))
        duplicate = 1;
    if (duplicate)
      continue;
    struct disk *d = &disks[disk_count++];
    memset(d, 0, sizeof(*d));
    d->identity = identity;
    d->source = names;
    memcpy(names, source, source_bytes);
    names += source_bytes;
    d->mount = names;
    memcpy(names, mount, mount_bytes);
    names += mount_bytes;
  }
}
static int mounts_changed(void) {
  struct {
    int fd;
    short events, revents;
  } descriptor = {(int)mounts_fd, 2, 0};
  struct timespec zero = {0, 0};
  long result = call6(N_ppoll, (long)&descriptor, 1, (long)&zero, 0, 8, 0);
  return mounts_retry || result < 0 || (descriptor.revents & 10);
}
static void publish_disks(void) {
  long fd = temporary_output(live_directory_fd, "." DISKS_FILE ".tmp");
  check(fd, "cannot create filesystem snapshot");
  disks_generated_ms = clock_ns(0) / 1000000;
  u8 h[28] = {0};
  memcpy(h, "QUECTOPD", 8);
  store_le32(h + 8, QUECTOP_DISK_SCHEMA_VERSION);
  store_le64(h + 12, disks_generated_ms);
  store_le32(h + 20, disk_count);
  store_le32(h + 24, (u32)mounts_retry | ((u32)mounts_truncated << 1));
  write_all(fd, h, sizeof(h));
  u32 checksum = hash(h, sizeof(h));
  for (u32 i = 0; i < disk_count; ++i) {
    struct disk *d = &disks[i];
    struct fsinfo fs;
    d->valid = filesystem_info(d->mount, &fs) >= 0 &&
               fs.blocks && fs.bfree <= fs.blocks && fs.bavail <= fs.bfree &&
               fs.bsize > 0 && fs.blocks <= ~0ULL / (u64)fs.bsize;
    u8 record[29];
    memset(record, 0, sizeof(record));
    u32 position = 0;
    put(record, &position, length(d->source), 16);
    put(record, &position, length(d->mount), 16);
    u64 values[3] = {0, 0, 0};
    if (d->valid) {
      values[0] = fs.blocks * (u64)fs.bsize / 1024;
      values[1] = (fs.blocks - fs.bfree) * (u64)fs.bsize / 1024;
      values[2] = fs.bavail * (u64)fs.bsize / 1024;
    }
    u32 bits = bits_needed(values[0]);
    put(record, &position, d->valid | ((bits - 1) << 1), 8);
    for (u32 k = 0; k < 3; ++k)
      put(record, &position, values[k], bits);
    u32 record_bytes = (position + 7) / 8;
    write_all(fd, record, record_bytes);
    write_all(fd, d->source, length(d->source));
    write_all(fd, d->mount, length(d->mount));
    checksum = hash_more(hash_more(hash_more(checksum, record, record_bytes),
                                   d->source, length(d->source)),
                         d->mount, length(d->mount));
  }
  u8 footer[4];
  store_le32(footer, checksum);
  write_all(fd, footer, sizeof(footer));
  check(call4(N_close, fd, 0, 0, 0), "filesystem snapshot close failed");
  check(call4(N_rename, live_directory_fd, (long)"." DISKS_FILE ".tmp",
              live_directory_fd, (long)DISKS_FILE),
        "filesystem snapshot rename failed");
  uplink_disks();
}

/* Capacities refresh on a timer; mount changes also rediscover filesystems. */
static void refresh_disks(u64 mono) {
  if (!last_disk || mono - last_disk >= (u64)disk_refresh_ms * 1000000) {
    publish_disks();
    last_disk = mono;
  }
}
static void rediscover_disks(void) {
  if (mounts_changed()) {
    discover_disks();
    publish_disks();
    last_disk = clock_ns(7);
  }
}
