/* Whole-service mapped memory. No full /proc scan or full smaps fallback. */
#define SERVICE_PROCESS_LIMIT 256
#define SERVICE_DEPTH_LIMIT 16

static long service_process_directory(u32 pid) {
  char digits[11];
  char *p = digits + sizeof(digits) - 1;
  *p = 0;
  do {
    *--p = (char)('0' + pid % 10);
    pid /= 10;
  } while (pid);
  return openat(service_proc, p, RDONLY | DIRECTORY | NOFOLLOW, 0);
}

static int service_process_start(long fd, u64 *start) {
  const char *s = service_read(fd, "stat"), *last = 0;
  if (!s)
    return 0;
  for (const char *p = s; *p; ++p)
    if (*p == ')')
      last = p;
  if (!last || last[1] != ' ')
    return 0;
  s = last + 2;
  for (int field = 3; field < 22; ++field) {
    while (*s && *s != ' ')
      ++s;
    if (!*s)
      return 0;
    ++s;
  }
  return number(&s, start) && (!*s || *s == ' ' || *s == '\n');
}

static void observe_process_uptime(u32 pid, struct service_observation *row) {
  if (!service_clock_ticks)
    return;
  long fd = service_process_directory(pid);
  u64 start, now = clock_ns(7) / 1000000000;
  if (fd >= 0) {
    if (service_process_start(fd, &start) &&
        start / service_clock_ticks <= now) {
      row->uptime = now - start / service_clock_ticks;
      row->flags |= SERVICE_HAS_UPTIME | SERVICE_PROCESS_AGE;
    }
    call4(N_close, fd, 0, 0, 0);
  }
}

static u8 service_parse_rollup(char *data, u64 values[SERVICE_MAPPED_COUNT]) {
  static const char *const keys[] = {
      "Rss:",      "Pss:",      "Private_Clean:", "Private_Dirty:",
      "Pss_Anon:", "Pss_File:", "Pss_Shmem:"};
  u64 fields[7] = {0};
  u8 found = 0;
  for (const char *line = data; *line;) {
    for (u32 i = 0; i < 7; ++i) {
      if (!prefix(line, keys[i]))
        continue;
      const char *p = line + length(keys[i]);
      u64 value;
      if (!number(&p, &value))
        return 0;
      while (*p == ' ' || *p == '\t')
        ++p;
      if (!prefix(p, "kB") || (p[2] && p[2] != '\n') || (found & (1 << i)))
        return 0;
      fields[i] = value;
      found |= (u8)(1 << i);
    }
    while (*line && *line != '\n')
      ++line;
    if (*line)
      ++line;
  }
  values[0] = fields[0];
  values[1] = fields[1];
  values[2] = fields[2] + fields[3];
  values[3] = fields[4];
  values[4] = fields[5];
  values[5] = fields[6];
  if (values[2] < fields[2])
    return 0;
  return (found & 3) | ((found & 12) == 12 ? 4 : 0) | ((found >> 1) & 56);
}

/* The directory fd pins this process: reads fail with ESRCH after it exits,
 * even if its PID is reused. */
static u8 observe_process_memory(u32 pid, u64 values[SERVICE_MAPPED_COUNT]) {
  long fd = service_process_directory(pid);
  if (fd < 0)
    return 0;
  u8 valid = 0;
  char *data = service_read(fd, "smaps_rollup");
  if (data)
    valid = service_parse_rollup(data, values);
  if (!(valid & 1)) {
    data = service_read(fd, "status");
    for (const char *p = data; p && *p;) {
      if (prefix(p, "VmRSS:")) {
        p += 6;
        if (number(&p, &values[0]))
          valid |= 1;
        break;
      }
      while (*p && *p != '\n')
        ++p;
      if (*p)
        ++p;
    }
  }
  call4(N_close, fd, 0, 0, 0);
  return valid;
}

/* getdents64 shares input with proc parsing. Resume at the selected entry's
 * cookie before the next refill so a child read cannot lose buffered entries.
 */
static long service_next_directory(long fd) {
  while (clock_ns(1) < service_deadline) {
    long bytes = call4(N_getdents, fd, (long)input, sizeof(input), 0);
    if (bytes == -4)
      continue;
    if (bytes <= 0)
      return bytes ? -2 : -1;
    for (u32 at = 0; at < (u32)bytes;) {
      const u8 *entry = (u8 *)input + at;
      if ((u32)bytes - at < 20)
        return -2;
      u32 size = entry[16] | (u32)entry[17] << 8;
      if (size < 20 || size > (u32)bytes - at)
        return -2;
      const char *name = (const char *)entry + 19;
      u32 name_bytes = 0;
      while (name_bytes < size - 19 && name[name_bytes])
        ++name_bytes;
      if (name_bytes == size - 19)
        return -2;
      if ((entry[18] == 4 || entry[18] == 0) && !equal(name, ".") &&
          !equal(name, "..")) {
        u64 offset = load_le32(entry + 8) | (u64)load_le32(entry + 12) << 32;
        long child = openat(fd, name, RDONLY | DIRECTORY | NOFOLLOW, 0);
        if (child >= 0) {
          if (seek_file(fd, offset, 0) < 0) {
            call4(N_close, child, 0, 0, 0);
            return -2;
          }
          return child;
        }
        if (child != -20) /* ENOTDIR is possible with DT_UNKNOWN. */
          return -2;
      }
      at += size;
    }
  }
  return -2;
}

static int service_group_processes(long group, struct service_observation *row,
                                   u32 seen[SERVICE_PROCESS_LIMIT],
                                   u32 *count) {
  long fd = openat(group, "cgroup.procs", RDONLY | NOFOLLOW, 0);
  if (fd < 0)
    return 0;
  char buffer[128];
  u64 pid = 0;
  int digits = 0, complete = 0;
  while (clock_ns(1) < service_deadline) {
    long bytes = call4(N_read, fd, (long)buffer, sizeof(buffer), 0);
    if (bytes == -4)
      continue;
    if (bytes <= 0) {
      complete = !bytes && !digits;
      break;
    }
    for (long i = 0; i < bytes; ++i) {
      char c = buffer[i];
      if (c >= '0' && c <= '9') {
        pid = pid * 10 + (u32)(c - '0');
        if (pid > 0xffffffffU)
          goto done;
        digits = 1;
        continue;
      }
      if (c != '\n' || !digits || !pid)
        goto done;
      u32 k = 0;
      while (k < *count && seen[k] != pid)
        ++k;
      if (k == *count) {
        if (*count == SERVICE_PROCESS_LIMIT || clock_ns(1) >= service_deadline)
          goto done;
        seen[(*count)++] = (u32)pid;
        u64 values[SERVICE_MAPPED_COUNT] = {0};
        row->mapped_valid &= observe_process_memory((u32)pid, values);
        for (u32 j = 0; j < SERVICE_MAPPED_COUNT; ++j) {
          if (values[j] > ~0ULL - row->mapped[j])
            row->mapped_valid &= (u8) ~(1 << j);
          else
            row->mapped[j] += values[j];
        }
      }
      pid = 0;
      digits = 0;
    }
  }
done:
  call4(N_close, fd, 0, 0, 0);
  return complete;
}

static void observe_service_memory(long group,
                                   struct service_observation *row) {
  long stack[SERVICE_DEPTH_LIMIT];
  u32 seen[SERVICE_PROCESS_LIMIT], count = 0, depth = 0, groups = 1;
  row->mapped_valid = SERVICE_MAPPED_ALL;
  if (!service_group_processes(group, row, seen, &count))
    row->mapped_valid = 0;
  stack[0] = group;
  for (;;) {
    long child = service_next_directory(stack[depth]);
    if (child >= 0) {
      if (depth + 1 == SERVICE_DEPTH_LIMIT || groups++ == 256 ||
          !service_group_processes(child, row, seen, &count)) {
        call4(N_close, child, 0, 0, 0);
        row->mapped_valid = 0;
        break;
      }
      stack[++depth] = child;
    } else if (child == -1) {
      if (!depth)
        break;
      call4(N_close, stack[depth--], 0, 0, 0);
    } else {
      row->mapped_valid = 0;
      break;
    }
  }
  while (depth)
    call4(N_close, stack[depth--], 0, 0, 0);
}
