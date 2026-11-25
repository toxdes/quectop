/* quectop-hub: receives agent streams and keeps DATA a static site: the
 * dashboard, hosts.txt and each host's replica files. Any static server can
 * serve DATA; --http adds a built-in one. Single-threaded, nonblocking. */
#include "../linux.h"
#define PROGRAM_NAME "quectop-hub"
#include "../protocol.h"
#include "../.build/assets.h"

static char input[4096]; /* data directory path scratch */

#include "../runtime.h"
#include "../net.h"
#include "../datadir.h"
#include "../lines.h"
#include "../auth.h"
#include "../noise.h"
#include "hosts.h"
#include "keys.h"
#include "intake.h"
#include "http.h"

extern const u8 asset_style[], asset_style_end[], asset_app[], asset_app_end[],
    asset_index[], asset_index_end[];

/* Writes the embedded dashboard when DATA holds another revision or lacks a
 * file. HTML goes last, then the revision marker. */
static void install_assets(void) {
  static const char revision[] = QUECTOP_ASSETS_REVISION;
  static const struct {
    const char *temporary, *name;
    const u8 *start, *end;
  } assets[] = {{".style.css.tmp", "style.css", asset_style, asset_style_end},
                {".app.js.tmp", "app.js", asset_app, asset_app_end},
                {".index.html.tmp", "index.html", asset_index, asset_index_end}};
  long fd = openat(data_fd, ".assets.revision", RDONLY | NOFOLLOW, 0);
  long n = fd < 0 ? 0 : call4(N_read, fd, (long)input, sizeof(revision), 0);
  if (fd >= 0)
    call4(N_close, fd, 0, 0, 0);
  int current = n == sizeof(revision) - 1;
  for (u32 i = 0; current && i < sizeof(revision) - 1; ++i)
    current = input[i] == revision[i];
  for (u32 i = 0; current && i < 3; ++i) {
    fd = openat(data_fd, assets[i].name, RDONLY | NOFOLLOW, 0);
    current = fd >= 0;
    if (fd >= 0)
      call4(N_close, fd, 0, 0, 0);
  }
  if (current)
    return;
  for (u32 i = 0; i < 3; ++i)
    replace_file(data_fd, assets[i].temporary, assets[i].name, assets[i].start,
                 (u32)(assets[i].end - assets[i].start));
  replace_file(data_fd, ".assets.revision.tmp", ".assets.revision", revision,
               sizeof(revision) - 1);
}

/* Parses "a.b.c.d:port". */
static int parse_endpoint(const char *s, struct endpoint *e) {
  memset(e, 0, sizeof(*e));
  e->family = 2;
  return parse_ipv4(&s, e->address) && *s++ == ':' && parse_port(s, e);
}
static long listen_on(const char *spec, const char *what, struct endpoint *at) {
  struct endpoint e;
  if (!parse_endpoint(spec, &e))
    fail(what);
  if (at)
    *at = e;
  long fd = call4(N_socket, 2, SOCKET_STREAM, 0, 0);
  check(fd, "cannot create socket");
  int one = 1; /* SOL_SOCKET, SO_REUSEADDR: restart without TIME_WAIT delay */
  call6(N_setsockopt, fd, 1, 2, (long)&one, sizeof(one), 0);
  check(call4(N_bind, fd, (long)&e, sizeof(e), 0), "cannot bind address");
  check(call4(N_listen, fd, 64, 0, 0), "cannot listen");
  return fd;
}

static void help(void) {
  static const char s[] =
      "quectop-hub " QUECTOP_VERSION
      ": receives quectop agents, no libc\n"
      "Usage: quectop-hub [options]\n"
      "  --agents IPV4:PORT  agent stream address (default 127.0.0.1:7070)\n"
      "  --http IPV4:PORT    also serve the data directory (default off)\n"
      "  --cors ORIGIN       let ORIGIN (or *) read --http responses\n"
      "  --keys FILE         device keys (default "
      "$HOME/.config/quectop-hub/keys);\n"
      "                      required when --agents is not on 127.0.0.1\n"
      "  --add-device NAME   add a key for NAME to the keys file, print it, "
      "exit\n"
      "  --data-dir DIR      created if missing (default "
      "$HOME/.local/state/quectop-hub)\n"
      "  --version, -v       show version\n"
      "  --help              show help\n"
      "Streams are encrypted; device keys authenticate both ends.\n"
      "The data directory is a static site: serve it with --http or any web "
      "server.\n";
  write_all(1, s, sizeof(s) - 1);
}

void entry(long *stack) __attribute__((noreturn));
void entry(long *stack) {
  int argc = (int)stack[0];
  char **argv = (char **)(stack + 1);
  const char *agents_spec = "127.0.0.1:7070", *http_spec = 0, *directory = 0,
             *keys = 0, *new_device = 0;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (equal(arg, "--help")) {
      help();
      finish(0);
    }
    if (equal(arg, "--version") || equal(arg, "-v")) {
      static const char version[] = "quectop-hub " QUECTOP_VERSION "\n";
      write_all(1, version, sizeof(version) - 1);
      finish(0);
    }
    if (++i == argc)
      fail("option requires a value; see --help");
    if (equal(arg, "--agents"))
      agents_spec = argv[i];
    else if (equal(arg, "--http"))
      http_spec = argv[i];
    else if (equal(arg, "--cors"))
      cors_origin = argv[i];
    else if (equal(arg, "--keys"))
      keys = argv[i];
    else if (equal(arg, "--add-device"))
      new_device = argv[i];
    else if (equal(arg, "--data-dir"))
      directory = argv[i];
    else
      fail("unknown option; see --help");
  }
  if (cors_origin && (!http_spec || !valid_origin(cors_origin)))
    fail("--cors needs --http and one origin, such as https://example.com");
  set_keys_path(keys, argv + argc + 1);
  if (new_device) {
    add_device(new_device);
    finish(0);
  }
  data_fd = prepare_data_directory(&directory, argv + argc + 1);
  check(call4(N_flock, data_fd, 6, 0, 0),
        "another quectop-hub owns this data directory");
  install_assets();
  load_hosts();
  struct endpoint agents_at;
  long agent_listener = listen_on(agents_spec, "--agents needs IPV4:PORT", &agents_at);
  keys_required = agents_at.address[0] != 127;
  long keys_fd = openat(AT_CWD, keys_path, RDONLY, 0);
  if (keys_fd < 0 && keys_required)
    fail("--agents beyond 127.0.0.1 needs device keys; add one with "
         "--add-device NAME");
  if (keys_fd >= 0 && !private_file(keys_fd))
    fail("keys file is readable by other users; run chmod 600 on it");
  if (keys_fd >= 0)
    call4(N_close, keys_fd, 0, 0, 0);
  long http_listener =
      http_spec ? listen_on(http_spec, "--http needs IPV4:PORT", 0) : -1;
  for (u32 i = 0; i < MAX_AGENTS; ++i)
    agents[i].fd = -1;
  for (u32 i = 0; i < MAX_CLIENTS; ++i) {
    clients[i].fd = clients[i].file = -1;
    clients[i].buffer = client_buffers[i];
  }

  struct poll_entry entries[2 + MAX_AGENTS + MAX_CLIENTS];
  void *owners[2 + MAX_AGENTS + MAX_CLIENTS];
  for (;;) {
    u64 now = clock_ns(1), deadline = now + 3600000000000ULL;
    u32 count = 2;
    entries[0] = (struct poll_entry){(int)agent_listener, POLL_IN, 0};
    entries[1] = (struct poll_entry){(int)http_listener, POLL_IN, 0}; /* -1: ignored */
    for (u32 i = 0; i < MAX_AGENTS; ++i) {
      struct agent *a = &agents[i];
      if (a->fd < 0)
        continue;
      u64 expires = a->last_ns + (u64)a->timeout_ms * 1000000;
      if (now >= expires) {
        close_agent(a);
        continue;
      }
      if (expires < deadline)
        deadline = expires;
      owners[count] = a;
      entries[count++] = (struct poll_entry){(int)a->fd, POLL_IN, 0};
    }
    for (u32 i = 0; i < MAX_CLIENTS; ++i) {
      struct client *c = &clients[i];
      if (c->fd < 0)
        continue;
      if (now >= c->deadline) {
        close_client(c);
        continue;
      }
      if (c->deadline < deadline)
        deadline = c->deadline;
      owners[count] = c;
      entries[count++] =
          (struct poll_entry){(int)c->fd, c->head_bytes ? POLL_OUT : POLL_IN, 0};
    }
    if (wait_events(entries, count, deadline) <= 0)
      continue;
    if (entries[0].revents)
      accept_agent(agent_listener);
    if (http_listener >= 0 && entries[1].revents)
      accept_client(http_listener);
    for (u32 i = 2; i < count; ++i) {
      short revents = entries[i].revents;
      if (!revents)
        continue;
      if (owners[i] >= (void *)agents && owners[i] < (void *)(agents + MAX_AGENTS)) {
        struct agent *a = owners[i];
        /* An earlier stream in this pass may have replaced this one. */
        if (a->fd == entries[i].fd)
          read_agent(a);
      } else {
        struct client *c = owners[i];
        if (revents & POLL_FAILED)
          close_client(c);
        else if (c->head_bytes)
          write_client(c);
        else
          read_client(c);
      }
    }
  }
}
