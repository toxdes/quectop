/* Freestanding ARMv6 integer helpers. No code is emitted for 64-bit targets. */
#if defined(__arm__)
typedef unsigned int u32;
typedef unsigned long long u64;

u32 quectop_udiv32(u32 dividend, u32 divisor, u32 *remainder) {
  if (!divisor)
    __builtin_trap();
  if (dividend < divisor) {
    *remainder = dividend;
    return 0;
  }
  u32 shift = (u32)(__builtin_clz(divisor) - __builtin_clz(dividend));
  u32 quotient = 0, bit = 1U << shift;
  divisor <<= shift;
  do {
    if (dividend >= divisor) {
      dividend -= divisor;
      quotient |= bit;
    }
    divisor >>= 1;
    bit >>= 1;
  } while (bit);
  *remainder = dividend;
  return quotient;
}

u64 quectop_udiv64(u64 dividend, u64 divisor, u64 *remainder) {
  if (!divisor)
    __builtin_trap();
  if (!(dividend >> 32) && !(divisor >> 32)) {
    u32 rest;
    u32 quotient = quectop_udiv32((u32)dividend, (u32)divisor, &rest);
    *remainder = rest;
    return quotient;
  }
  if (dividend < divisor) {
    *remainder = dividend;
    return 0;
  }
  u32 shift = (u32)(__builtin_clzll(divisor) - __builtin_clzll(dividend));
  u64 quotient = 0, bit = 1ULL << shift;
  divisor <<= shift;
  do {
    if (dividend >= divisor) {
      dividend -= divisor;
      quotient |= bit;
    }
    divisor >>= 1;
    bit >>= 1;
  } while (bit);
  *remainder = dividend;
  return quotient;
}
#endif
