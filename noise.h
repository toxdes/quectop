/* Noise_NNpsk0_25519_ChaChaPoly_SHA512 (noiseprotocol.org, revision 34) over
 * vendored Monocypher. The device key is the PSK, so both sides prove they hold
 * it; each connection's ephemeral X25519 keys add forward secrecy. Checked
 * against the cacophony vectors and the noiseprotocol package (test/crypto.py). */
#include "vendor/monocypher/monocypher.h"
#include "vendor/monocypher/monocypher-ed25519.h"

#define NOISE_KEY 32
#define NOISE_HASH 64
#define NOISE_TAG 16
#define NOISE_MESSAGE_MAX 65535
#define NOISE_HANDSHAKE_MIN (NOISE_KEY + NOISE_TAG) /* e, then a sealed payload */

struct noise_cipher {
  u8 key[NOISE_KEY];
  u64 n;
};
struct noise {
  u8 ck[NOISE_HASH], h[NOISE_HASH], e[NOISE_KEY], re[NOISE_KEY];
  u8 has_e, has_re;
  struct noise_cipher c;
};

static void noise_hash(u8 *out, const u8 *a, u32 a_bytes, const u8 *b, u32 b_bytes) {
  crypto_sha512_ctx x;
  crypto_sha512_init(&x);
  crypto_sha512_update(&x, a, a_bytes);
  crypto_sha512_update(&x, b, b_bytes);
  crypto_sha512_final(&x, out);
}
static void noise_mix_hash(struct noise *s, const u8 *data, u32 bytes) {
  noise_hash(s->h, s->h, NOISE_HASH, data, bytes);
}
/* HKDF(ck, ikm) into ck and the cipher key; with psk, the middle output is hashed. */
static void noise_mix_key(struct noise *s, const u8 *ikm, u32 bytes, int psk) {
  u8 out[3 * NOISE_HASH];
  u32 outputs = psk ? 3 : 2;
  crypto_sha512_hkdf(out, outputs * NOISE_HASH, ikm, bytes, s->ck, NOISE_HASH, 0, 0);
  memcpy(s->ck, out, NOISE_HASH);
  if (psk)
    noise_mix_hash(s, out + NOISE_HASH, NOISE_HASH);
  memcpy(s->c.key, out + (outputs - 1) * NOISE_HASH, NOISE_KEY);
  s->c.n = 0;
  crypto_wipe(out, sizeof(out));
}
static void noise_nonce(u8 nonce[12], u64 n) {
  for (u32 i = 0; i < 4; ++i)
    nonce[i] = 0;
  for (u32 i = 0; i < 8; ++i)
    nonce[4 + i] = (u8)(n >> 8 * i);
}
/* Encrypts text in place and appends the tag. */
static void noise_seal(struct noise_cipher *c, const u8 *ad, u32 ad_bytes,
                       u8 *text, u32 bytes) {
  u8 nonce[12];
  crypto_aead_ctx x;
  noise_nonce(nonce, c->n++);
  crypto_aead_init_ietf(&x, c->key, nonce);
  crypto_aead_write(&x, text, text + bytes, ad, ad_bytes, text, bytes);
  crypto_wipe(&x, sizeof(x));
}
/* Decrypts bytes (tag included) in place; 0 when forged. */
static int noise_open(struct noise_cipher *c, const u8 *ad, u32 ad_bytes,
                      u8 *text, u32 bytes) {
  if (bytes < NOISE_TAG)
    return 0;
  u8 nonce[12];
  crypto_aead_ctx x;
  noise_nonce(nonce, c->n);
  crypto_aead_init_ietf(&x, c->key, nonce);
  int forged = crypto_aead_read(&x, text, text + bytes - NOISE_TAG, ad, ad_bytes,
                                text, bytes - NOISE_TAG);
  crypto_wipe(&x, sizeof(x));
  c->n += !forged;
  return !forged;
}

/* Both roles start alike: protocol name, prologue, then the psk token. */
static void noise_start(struct noise *s, const u8 *prologue, u32 bytes,
                        const u8 psk[NOISE_KEY]) {
  static const char name[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA512";
  memset(s, 0, sizeof(*s));
  memcpy(s->h, name, sizeof(name) - 1); /* zero-padded to HASHLEN */
  memcpy(s->ck, s->h, NOISE_HASH);
  noise_mix_hash(s, prologue, bytes);
  noise_mix_key(s, psk, NOISE_KEY, 1);
}
/* The ee token; an all-zero result means a low-order point. */
static int noise_ee(struct noise *s) {
  static const u8 zero[NOISE_KEY];
  u8 shared[NOISE_KEY];
  crypto_x25519(shared, s->e, s->re);
  int low_order = !crypto_verify32(shared, zero);
  noise_mix_key(s, shared, NOISE_KEY, 0);
  crypto_wipe(shared, sizeof(shared));
  return !low_order;
}
/* Writes "e, [ee,] payload" to out: the initiator's first message or the
 * responder's reply. Returns its length, 0 on a low-order peer key. */
static u32 noise_write(struct noise *s, const u8 secret[NOISE_KEY],
                       const u8 *payload, u32 bytes, u8 *out) {
  memcpy(s->e, secret, NOISE_KEY);
  s->has_e = 1;
  crypto_x25519_public_key(out, s->e);
  noise_mix_hash(s, out, NOISE_KEY);
  noise_mix_key(s, out, NOISE_KEY, 0); /* psk modes mix e into the key */
  if (s->has_re && !noise_ee(s))
    return 0;
  u8 *sealed = out + NOISE_KEY;
  memcpy(sealed, payload, bytes);
  noise_seal(&s->c, s->h, NOISE_HASH, sealed, bytes);
  noise_mix_hash(s, sealed, bytes + NOISE_TAG);
  return NOISE_KEY + bytes + NOISE_TAG;
}
/* Reads the peer's "e, [ee,] payload", decrypting the payload in place at
 * message + NOISE_KEY. Returns the payload length, -1 when forged. */
static long noise_read(struct noise *s, u8 *message, u32 bytes) {
  if (bytes < NOISE_HANDSHAKE_MIN)
    return -1;
  memcpy(s->re, message, NOISE_KEY);
  s->has_re = 1;
  noise_mix_hash(s, s->re, NOISE_KEY);
  noise_mix_key(s, s->re, NOISE_KEY, 0);
  if (s->has_e && !noise_ee(s))
    return -1;
  u8 *sealed = message + NOISE_KEY, next[NOISE_HASH];
  u32 sealed_bytes = bytes - NOISE_KEY;
  noise_hash(next, s->h, NOISE_HASH, sealed, sealed_bytes); /* before decrypting in place */
  if (!noise_open(&s->c, s->h, NOISE_HASH, sealed, sealed_bytes))
    return -1;
  memcpy(s->h, next, NOISE_HASH);
  return sealed_bytes - NOISE_TAG;
}
/* Transport keys; s keeps only the handshake hash. */
static void noise_split(struct noise *s, int initiator, struct noise_cipher *send,
                        struct noise_cipher *receive) {
  u8 out[2 * NOISE_HASH];
  crypto_sha512_hkdf(out, sizeof(out), 0, 0, s->ck, NOISE_HASH, 0, 0);
  memcpy(send->key, out + (initiator ? 0 : NOISE_HASH), NOISE_KEY);
  memcpy(receive->key, out + (initiator ? NOISE_HASH : 0), NOISE_KEY);
  send->n = receive->n = 0;
  crypto_wipe(out, sizeof(out));
  crypto_wipe(s->ck, NOISE_HASH);
  crypto_wipe(s->e, NOISE_KEY);
  crypto_wipe(&s->c, sizeof(s->c));
}
