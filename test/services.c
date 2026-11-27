#ifdef QUECTOP_TEST_FREESTANDING
#define entry probe_entry
#endif
#define QUECTOP_VERSION "test"
#include "../main.c"
#ifdef QUECTOP_TEST_FREESTANDING
#undef entry
#endif

/* Read actual fixture files through the production syscall readers. */
int main(int argc, char **argv) {
  if (argc != 2 && argc != 4)
    return 2;
  long root = openat(AT_CWD, argv[1], RDONLY | DIRECTORY, 0);
  service_openrc = openat(root, "openrc", RDONLY | DIRECTORY, 0);
  service_cgroups = openat(root, "cgroup", RDONLY | DIRECTORY, 0);
  service_proc = openat(root, "proc", RDONLY | DIRECTORY, 0);
  service_clock_ticks = 100;
  service_backend = SERVICE_OPENRC;
  service_deadline = clock_ns(1) + 5000000000ULL;
  struct service_observation row = {0};
  if (argc == 4) {
    service_bus = option_number(argv[2], 0, 0x7fffffffU);
    observe_systemd("worker.service", &row);
    if (equal(argv[3], "valid")) {
      if ((row.flags & 15) != 9 || row.memory != 987654 ||
          row.mapped_valid != 63 || row.mapped[0] != 1200)
        fail("systemd ControlGroup and mapped memory");
    } else if (row.mapped_valid)
      fail("truncated systemd ControlGroup must be unavailable");
    return 0;
  }
  observe_openrc("worker", &row);
  if ((row.flags & 7) != 2 || !(row.flags & 8) || row.memory != 987654 ||
      !(row.flags & 16) || !(row.flags & 128))
    fail("OpenRC state, cgroup memory and process uptime");
  if (row.mapped_valid != 63 || row.mapped[0] != 1200 || row.mapped[1] != 900 ||
      row.mapped[2] != 600 || row.mapped[3] != 450 || row.mapped[4] != 300 ||
      row.mapped[5] != 150)
    fail("service process aggregation and deduplication");
  u8 encoded[512] = {0};
  u32 encoded_bytes = encode_service(encoded, "worker", &row);
  row = (struct service_observation){0};
  observe_openrc("missing", &row);
  if (row.flags != 64 || row.mapped_valid)
    fail("missing OpenRC service must be unavailable");
  row = (struct service_observation){0};
  observe_openrc("denied", &row);
  if (row.mapped_valid != 1 || row.mapped[0] != 123)
    fail("RSS fallback without PSS permissions");
  row = (struct service_observation){0};
  observe_openrc("reused", &row);
  if (row.mapped_valid || row.mapped[0])
    fail("reused PID contributed memory from a different cgroup");
  const char *group_layouts[] = {"hybrid", "legacy"};
  for (u32 i = 0; i < 2; ++i) {
    row = (struct service_observation){0};
    observe_openrc(group_layouts[i], &row);
    if (row.mapped_valid != 63 || row.mapped[0] != 400)
      fail("OpenRC cgroup membership must use the matching hierarchy");
  }
  row = (struct service_observation){0};
  observe_openrc("partial", &row);
  if (row.mapped_valid)
    fail("partial aggregates must be unavailable");
  const char *incomplete[] = {"depth", "malformed", "hugepid", "truncated"};
  for (u32 i = 0; i < 4; ++i) {
    row = (struct service_observation){0};
    observe_openrc(incomplete[i], &row);
    if (row.mapped_valid)
      fail("bounded or malformed process collection must be unavailable");
  }
  row = (struct service_observation){0};
  observe_openrc("empty", &row);
  if (row.mapped_valid != 63 || row.mapped[0] || row.mapped[1])
    fail("empty cgroup must have valid zero totals");
  service_deadline = 0;
  row = (struct service_observation){0};
  observe_openrc("worker", &row);
  if (row.mapped_valid || row.flags != 64)
    fail("expired service collection budget");
  service_backend = SERVICE_NONE;
  char *names[] = {"missing"};
  service_names = names;
  service_count = 1;
  u8 unavailable[128] = {0};
  if (encode_services(unavailable) != 9 || unavailable[1])
    fail("missing service manager must be unavailable");
  service_proc = openat(AT_CWD, "/proc", RDONLY | DIRECTORY, 0);
  service_deadline = clock_ns(1) + 5000000000ULL;
  const char *stat = service_read(service_proc, "self/stat");
  u64 pid, actual[6] = {0};
  if (!stat || !number(&stat, &pid) || pid > 0xffffffffU)
    fail("real process ID");
  long proc = service_process_directory((u32)pid);
  if (proc < 0 || (read_process_memory(proc, actual) & 3) != 3 || !actual[0] || !actual[1])
    fail("real process RSS and PSS");
  call4(N_close, proc, 0, 0, 0);
  write_all(1, encoded, encoded_bytes);
  return 0;
}

#ifdef QUECTOP_TEST_FREESTANDING
void entry(long *stack) { finish(main((int)stack[0], (char **)(stack + 1))); }
#endif
