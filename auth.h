/* Device key lines: "ID HEX", HEX being KEY_BYTES of key, as in the hub's
 * --keys file and an agent's --key-file. The key is the Noise PSK (noise.h). */
#define KEY_BYTES 32

/* Whether only the owner may read fd's file, as ssh demands of keys. */
static int private_file(long fd) {
  u8 info[256]; /* struct statx; stx_mode is the u16 at 28 */
  return call6(N_statx, fd, (long)"", 0x1000 /* AT_EMPTY_PATH */, 2 /* MODE */,
               (long)info, 0) == 0 &&
         !((info[28] | info[29] << 8) & 077);
}

static int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0'
         : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10
                                                   : -1;
}
/* "ID HEX" with nothing else but a '#' comment; 0 when malformed. */
static int parse_key_line(const char *line, u32 bytes, const char **id,
                          u32 *id_bytes, u8 *key) {
  u32 at = 0;
  const char *hex;
  *id_bytes = word(line, bytes, &at, id);
  if (!valid_id(*id, *id_bytes) || word(line, bytes, &at, &hex) != 2 * KEY_BYTES)
    return 0;
  for (u32 i = 0; i < KEY_BYTES; ++i) {
    int high = hex_digit(hex[2 * i]), low = hex_digit(hex[2 * i + 1]);
    if (high < 0 || low < 0)
      return 0;
    key[i] = (u8)(high << 4 | low);
  }
  const char *rest;
  return !word(line, bytes, &at, &rest);
}
