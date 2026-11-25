/* Command-line help. */
static void help(void) {
  const char s[] =
      "quectop " QUECTOP_VERSION
      ": Linux "
#if defined(__arm__)
      "ARMv6"
#else
      "ARM64/x86-64"
#endif
      " agent, no libc\nUsage: quectop "
      "[options]\n"
      "  --live-dir DIR   live file directory; prefer RAM storage (default "
      "output)\n"
      "  --data-dir DIR   created if missing (default "
      "$HOME/.local/state/quectop)\n"
      "  --output, -o DIR aliases for --data-dir\n"
      "  --interval-ms N  sample period, 100..3600000 (default 5000)\n"
      "  --publish-ms N   live publication, 100..3600000 (default 30000)\n"
      "  --points N       buckets per range, 16..10000 (default 1000)\n"
      "  --disk-refresh-ms N  capacity check period, 1000..3600000 (default "
      "600000)\n"
      "  --disk PATH      restrict capacities to this mounted filesystem\n"
      "  --thermal PATH   millidegree sensor (default thermal_zone0/temp)\n"
      "  --interface IF   track only IF (default device-backed interfaces)\n"
      "  --hub HOST:PORT  also stream to quectop-hub, which serves the dashboard\n"
      "  --key-file FILE  device key line from quectop-hub --add-device\n"
      "  --services LIST comma-separated system services (default none)\n"
      "  --version, -v    show version\n"
      "  --help           show help\n"
      "Writes " LIVE_FILE ", " DISKS_FILE " and 1h/1d/7d/30d/90d" HISTORY_SUFFIX
      ".\n"
      "No fsync; active summaries and recent writes can be lost on "
      "interruption.\n"
      "A changed interval or point count requires a new output directory.\n";
  write_all(1, s, sizeof(s) - 1);
}
