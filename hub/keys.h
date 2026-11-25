/* Device keys (--keys FILE): one "ID HEX" line per device, read again on every
 * connection so added and removed lines apply to the next one. When the file
 * exists, only listed devices may stream; agents beyond loopback require it. */
#define KEYS_PATH_MAX 1024

static char keys_path[KEYS_PATH_MAX];
static int keys_required;

/* Copies $HOME/suffix or path into keys_path. */
static void set_keys_path(const char *path, char **env) {
  const char *suffix = "";
  if (!path) {
    for (; *env && !path; ++env)
      if (prefix(*env, "HOME="))
        path = *env + 5;
    if (!path || path[0] != '/')
      fail("HOME must be an absolute path; use --keys FILE");
    suffix = "/.config/" PROGRAM_NAME "/keys";
  }
  u32 a = (u32)length(path), b = (u32)length(suffix);
  if (!a || a + b >= KEYS_PATH_MAX)
    fail("--keys path is empty or too long");
  memcpy(keys_path, path, a);
  memcpy(keys_path + a, suffix, b + 1);
}

static int same_id(const char *a, u32 a_bytes, const char *b, u32 b_bytes) {
  u32 i = 0;
  while (i < a_bytes && i < b_bytes && a[i] == b[i])
    ++i;
  return i == a_bytes && i == b_bytes;
}
/* Copies id's key; zeros when no keys file is required. 0 when id is unlisted. */
static int device_key(const char *id, u32 id_bytes, u8 key[KEY_BYTES]) {
  struct lines r = open_lines(keys_path);
  memset(key, 0, KEY_BYTES);
  if (r.fd == -2 && !keys_required) /* ENOENT */
    return 1;
  if (r.fd >= 0 && !private_file(r.fd)) {
    static const char exposed[] =
        PROGRAM_NAME ": keys file is readable by other users; run chmod 600 on it\n";
    write_all(2, exposed, sizeof(exposed) - 1);
    close_lines(&r);
    return 0;
  }
  const char *line, *listed;
  u32 n, listed_bytes;
  int found = 0;
  while (!found && next_line(&r, &line, &n))
    found = parse_key_line(line, n, &listed, &listed_bytes, key) &&
            same_id(listed, listed_bytes, id, id_bytes);
  close_lines(&r);
  if (!found)
    crypto_wipe(key, KEY_BYTES);
  return found;
}

/* --add-device: appends a fresh key line for id and prints it. */
static void add_device(const char *id) {
  u32 id_bytes = (u32)length(id);
  if (!valid_id(id, id_bytes))
    fail("--add-device needs a name of [A-Za-z0-9_.-], not starting with '.'");
  char *slash = keys_path + length(keys_path);
  while (slash > keys_path && *slash != '/')
    --slash;
  if (slash > keys_path) {
    *slash = 0;
    make_directories(keys_path, "cannot create keys directory");
    *slash = '/';
  }
  long fd = openat(AT_CWD, keys_path, RDWR | CREATE | APPEND | NOFOLLOW, 0600);
  check(fd, "cannot open keys file");
  check(call4(N_flock, fd, 2, 0, 0), "cannot lock keys file");
  struct lines r = open_lines(keys_path);
  const char *line, *listed;
  u32 n, listed_bytes;
  u8 key[KEY_BYTES];
  while (next_line(&r, &line, &n))
    if (parse_key_line(line, n, &listed, &listed_bytes, key) &&
        same_id(listed, listed_bytes, id, id_bytes))
      fail("this device already has a key; delete its line to replace it");
  close_lines(&r);
  if (call4(N_getrandom, (long)key, KEY_BYTES, 0, 0) != KEY_BYTES)
    fail("cannot read random bytes");
  char out[1 + HOST_ID_MAX + 1 + 2 * KEY_BYTES + 1], *p = out;
  i64 size = seek_file(fd, 0, 2);
  u8 last = '\n';
  if (size > 0)
    file_io(N_pread, fd, &last, 1, (u64)size - 1);
  if (last != '\n') /* keep a hand-edited last line separate */
    *p++ = '\n';
  char *start = p;
  memcpy(p, id, id_bytes);
  p += id_bytes;
  *p++ = ' ';
  for (u32 i = 0; i < KEY_BYTES; ++i) {
    *p++ = "0123456789abcdef"[key[i] >> 4];
    *p++ = "0123456789abcdef"[key[i] & 15];
  }
  *p++ = '\n';
  write_all(fd, out, (unsigned long)(p - out));
  check(call4(N_close, fd, 0, 0, 0), "keys file close failed");
  write_all(1, start, (unsigned long)(p - start));
  static const char hint[] =
      "Added to the keys file. Save the line above in a file on the device, "
      "then run:\n  quectop --hub HOST:PORT --key-file FILE\n";
  write_all(2, hint, sizeof(hint) - 1);
}
