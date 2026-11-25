/* Creates path and its missing parents; path is restored before returning. */
static void make_directories(char *path, const char *what) {
  for (char *p = path + (*path == '/');; ++p) {
    if (*p && *p != '/')
      continue;
    char saved = *p;
    *p = 0;
    long result = call4(N_mkdir, AT_CWD, (long)path, 0755, 0);
    *p = saved;
    if (result < 0 && result != -17) /* EEXIST is checked by the caller. */
      fail(what);
    if (!saved)
      return;
  }
}
/* Data directory selection; the path is formatted in the input scratch. */
static long prepare_data_directory(const char **directory, char **env) {
  static const char label[] = PROGRAM_NAME ": data directory: ";
  const char *path = *directory, *suffix = "";
  if (!path) {
    for (; *env; ++env)
      if (prefix(*env, "HOME=")) {
        path = *env + 5;
        break;
      }
    if (!path || path[0] != '/')
      fail("HOME must be an absolute path; use --data-dir DIR");
    suffix = "/.local/state/" PROGRAM_NAME;
  }
  if (!*path)
    fail("data directory must not be empty");
  memcpy(input, label, sizeof(label) - 1);
  char *start = input + sizeof(label) - 1, *end = start;
  for (int part = 0; part < 2; ++part) {
    const char *s = part ? suffix : path;
    while (*s) {
      if (end == input + sizeof(input) - 1)
        fail("data directory path too long");
      *end++ = *s++;
    }
  }
  *end = 0;
  long fd = openat(AT_CWD, start, RDONLY | DIRECTORY, 0);
  if (fd == -2) { /* ENOENT */
    make_directories(start, "cannot create data directory");
    fd = openat(AT_CWD, start, RDONLY | DIRECTORY, 0);
  }
  check(fd, "cannot open output directory");
  *directory = start;
  *end = '\n';
  write_all(1, input, (unsigned long)(end - input) + 1);
  *end = 0;
  return fd;
}
