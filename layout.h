/* Shared v1 history geometry. */
static const u32 spans[TIERS] = {3600, 86400, 604800, 2592000, 7776000};

static inline u32 publication_spacing(u32 interval, u32 publish) {
  return (publish + interval - 1) / interval * interval;
}

static inline u32 history_width(u32 points, u32 interval, u32 tier) {
  u32 width = (spans[tier] + points - 2) / (points - 1);
  u32 sampling = (interval + 999) / 1000;
  return tier == 0 && width < sampling ? sampling : width;
}

static inline u32 bucket_bytes(u32 metrics, u32 values, u32 lows, u32 offsets) {
  return (32 + metrics + 2 * values + lows + metrics * offsets + 7) / 8 + 4;
}
