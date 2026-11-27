/* Sample layout and packed metric values. */
struct values {
  u16 *small;
  u64 *net;
};
struct sample {
  struct values value;
  u32 sec;
  u8 *valid;
};
static struct sample latest;
static u8 *id_wire;
static u32 core_count, metrics, small_count, id_bits, id_bytes, latest_bytes,
    header_bytes, metadata_bytes, value_bits, frequency_bits;
/* Metrics from memory_index to small_count also keep bucket minimums. */
static u32 overall_index, user_index, system_index, iowait_index,
    pressure_index, memory_index, swap_index, temp_index, frequency_index,
    low_count, low_bits;

static u64 value_get(const struct values *v, int k) {
  return k < (int)small_count ? v->small[k] : v->net[k - small_count];
}
static void value_set(struct values *v, int k, u64 n) {
  if (k < (int)small_count)
    v->small[k] = (u16)n;
  else
    v->net[k - small_count] = n;
}
static u16 percentage(u64 used, u64 total) {
  if (used > total || !total)
    fail("invalid percentage counters");
  return (u16)((used * 1000 + total / 2) / total);
}
/* Internal network unit: 0.001 B/s. Wire: mantissa(10), decimals(2), SI
 * unit(2). */
#define NETWORK_MILLI_MAX 999000000000000ULL
static u16 network_pack(u64 milli) {
  u64 factor = 1000;
  u32 unit = 0;
  while (unit < 3 && milli / factor >= 1000) {
    factor *= 1000;
    ++unit;
  }
  for (;;) {
    u64 integer = milli / factor;
    u32 decimals = integer >= 100 ? 0 : integer >= 10 ? 1 : 2;
    u32 scale = decimals == 2 ? 100 : decimals == 1 ? 10 : 1;
    u64 divisor = factor / scale;
    u64 mantissa = milli / divisor + (milli % divisor >= (divisor + 1) / 2);
    if (mantissa <= 999)
      return (u16)(mantissa | (decimals << 10) | (unit << 12));
    if (decimals) {
      --decimals;
      scale = decimals ? 10 : 1;
      divisor = factor / scale;
      mantissa = milli / divisor + (milli % divisor >= (divisor + 1) / 2);
      if (mantissa <= 999)
        return (u16)(mantissa | (decimals << 10) | (unit << 12));
    }
    if (unit == 3)
      return 999 | (3 << 12);
    factor *= 1000;
    ++unit;
  }
}
static u32 value_width(u32 k) {
  return k < temp_index    ? 10
         : k == temp_index ? 12
         : k < small_count ? frequency_bits
                           : 14;
}
static int is_valid(u32 k) { return (latest.valid[k >> 3] >> (k & 7)) & 1; }
static void set_valid(u32 k) { latest.valid[k >> 3] |= (u8)(1U << (k & 7)); }
static void pack_values(u8 *dest, u32 *position, const struct values *v) {
  for (u32 k = 0; k < metrics; ++k)
    put(dest, position,
        k < small_count ? value_get(v, k) : network_pack(value_get(v, k)),
        value_width(k));
}
