/* Command-line help. */
static void help(void) {
  const char s[] =
      "quectop " QUECTOP_VERSION ": resource monitor probe for Linux ("
#if defined(__arm__)
      "ARMv6"
#else
      "ARM64/x86-64"
#endif
      ")\n"
      "Usage: quectop [options]\n"
      "\n"
      "Options:\n"
      "  --data-dir DIR       Data directory, created if missing\n"
      "                       (default: $HOME/.local/state/quectop)\n"
      "  -o, --output DIR     Same as --data-dir\n"
      "  --live-dir DIR       Directory for the live and capacity snapshots. A\n"
      "                       RAM-backed directory reduces storage writes.\n"
      "                       (default: the data directory)\n"
      "  --interval-ms N      Sampling period, 100-3600000 (default: 5000)\n"
      "  --publish-ms N       Live snapshot period, 100-3600000 (default: 30000)\n"
      "  --points N           Windows per history range, 16-10000 (default: 1000)\n"
      "  --disk-refresh-ms N  Filesystem capacity period, 1000-3600000\n"
      "                       (default: 600000)\n"
      "  --disk PATH          Report the capacity of this mount point only\n"
      "  --thermal PATH       Temperature sensor in millidegrees Celsius\n"
      "                       (default: /sys/class/thermal/thermal_zone0/temp)\n"
      "  --interface IF       Track only interface IF\n"
      "                       (default: device-backed interfaces)\n"
      "  --services LIST      Comma-separated system services to report\n"
      "  --hub HOST:PORT      Stream data to quectop-hub\n"
      "  --key-file FILE      Device key file from quectop-hub --add-device\n"
      "  -v, --version        Print the version and exit\n"
      "  --help               Print this help and exit\n"
      "\n"
      "Files: " LIVE_FILE ", " DISKS_FILE ", and 1h/1d/7d/30d/90d" HISTORY_SUFFIX ".\n"
      "The probe does not call fsync. On power loss, recent data can be lost.\n"
      "Changing --interval-ms or --points requires a new data directory.\n";
  write_all(1, s, sizeof(s) - 1);
}
