/* Data file names and the probe-to-hub stream, shared by quectop and quectop-hub.
 * Frames: u8 type, u24 little-endian payload length, payload. */
#define TIERS 5
#define HEADER_BYTES 64
#define LIVE_HEADER_BYTES 144
#define PENDING_BYTES 1024
#define QUECTOP_SCHEMA_VERSION 1
#define QUECTOP_DISK_SCHEMA_VERSION 1
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
#define HISTORY_SUFFIX ".v" STRINGIFY(QUECTOP_SCHEMA_VERSION) ".bin"
#define LIVE_FILE "live" HISTORY_SUFFIX
#define DISKS_FILE "disks.v" STRINGIFY(QUECTOP_DISK_SCHEMA_VERSION) ".bin"

/* Wire: every message is a u16 little-endian length, then that many bytes.
 * The probe opens with OPEN in the clear, which is also the Noise prologue, and
 * Noise message 1, whose payload is u32 publish ms, u8 hostname length,
 * hostname. The hub answers with Noise message 2, payload empty (noise.h).
 * Every later message is one frame sealed with the transport keys. */
#define PROTOCOL_VERSION 3
#define MESSAGE_HEADER 2
#define FRAME_HEADER 4
#define FRAME_MAX (65535 - 16 - FRAME_HEADER) /* a sealed frame fills a message */
#define HOST_ID_MAX 64
#define OPEN_MAX (6 + HOST_ID_MAX) /* "QTOP", u8 version, u8 id length, id */
#define RESUME_BYTES (FRAME_HEADER + 4 * TIERS)
#define RESUME_TIMEOUT_MS 60000u
#define OPTION_MAX_MS 3600000u
#define MAX_PUBLICATION_MS (2 * OPTION_MAX_MS)

static inline u32 transport_timeout_ms(u32 publication_ms) {
  return 3 * (publication_ms < MAX_PUBLICATION_MS ? publication_ms : MAX_PUBLICATION_MS) + 60000;
}
enum {
  /* Probe to hub: u8 tier, the ring file's header and metadata. */
  FRAME_RING = 2,
  /* Hub to probe, after all rings: u32 newest bucket start held per tier. */
  FRAME_RESUME = 3,
  /* Probe to hub: u8 tier, one encoded bucket. */
  FRAME_RECORD = 4,
  /* Probe to hub: the complete live or disk snapshot file. */
  FRAME_LIVE = 5,
  FRAME_DISKS = 6
};

/* Host ids name hub directories: [A-Za-z0-9_.-], no leading dot. */
static int valid_id(const char *id, u32 bytes) {
  if (!bytes || bytes > HOST_ID_MAX || id[0] == '.')
    return 0;
  for (u32 i = 0; i < bytes; ++i) {
    char c = id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.'))
      return 0;
  }
  return 1;
}
static const char *const filenames[TIERS] = {
    "1h" HISTORY_SUFFIX, "1d" HISTORY_SUFFIX, "7d" HISTORY_SUFFIX,
    "30d" HISTORY_SUFFIX, "90d" HISTORY_SUFFIX};

static void frame_header(u8 *dest, u8 type, u32 bytes) {
  dest[0] = type;
  dest[1] = (u8)bytes;
  dest[2] = (u8)(bytes >> 8);
  dest[3] = (u8)(bytes >> 16);
}
