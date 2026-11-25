/* Text files read line by line through input[]; overlong lines are skipped. */
struct lines {
  long fd;
  u32 at, end;
  int skipping;
};
static int next_line(struct lines *r, const char **line, u32 *bytes) {
  for (;;) {
    u32 i = r->at;
    while (i < r->end && input[i] != '\n')
      ++i;
    if (i < r->end) {
      u32 start = r->at;
      r->at = i + 1;
      if (r->skipping) {
        r->skipping = 0;
        continue;
      }
      *line = input + start;
      *bytes = i - start;
      return 1;
    }
    u32 left = r->end - r->at;
    if (left == sizeof(input)) {
      r->skipping = 1;
      left = 0;
    }
    memcpy(input, input + r->at, left); /* forward copy; dst < src */
    r->at = 0;
    r->end = left;
    long n = r->fd < 0 ? 0 : call4(N_read, r->fd, (long)(input + left),
                                   sizeof(input) - left, 0);
    if (n == -4)
      continue;
    if (n <= 0) { /* a last line without a newline still counts */
      *line = input;
      *bytes = r->skipping ? 0 : left;
      r->at = r->end = 0;
      return left && !r->skipping;
    }
    r->end += (u32)n;
  }
}
static struct lines open_lines(const char *path) {
  return (struct lines){openat(AT_CWD, path, RDONLY, 0), 0, 0, 0};
}
static void close_lines(struct lines *r) {
  if (r->fd >= 0)
    call4(N_close, r->fd, 0, 0, 0);
}
/* Next whitespace-separated word before any '#'; 0 at the end. */
static u32 word(const char *line, u32 bytes, u32 *at, const char **start) {
  while (*at < bytes && (line[*at] == ' ' || line[*at] == '\t'))
    ++*at;
  *start = line + *at;
  u32 n = 0;
  while (*at < bytes && line[*at] != ' ' && line[*at] != '\t' &&
         line[*at] != '#')
    ++n, ++*at;
  return n;
}
