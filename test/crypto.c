/* Freestanding driver for test/crypto.py: one "op hex..." line in, one line out;
 * "-" is an empty field. Built with the product's flags and runtime. */
#define PROGRAM_NAME "crypto"
#include "../linux.h"
#include "../protocol.h"
#include "../runtime.h"
#include "../noise.h"

#define FIELDS 12
static u8 input[16 << 20], output[16 << 20], field[FIELDS][1 << 16],
    work[1 << 17];
static u32 field_bytes[FIELDS], out_at;

static void emit(const char *s) {
  while (*s)
    output[out_at++] = (u8)*s++;
}
static void emit_hex(const u8 *p, u32 n) {
  for (u32 i = 0; i < n; ++i) {
    output[out_at++] = (u8)"0123456789abcdef"[p[i] >> 4];
    output[out_at++] = (u8)"0123456789abcdef"[p[i] & 15];
  }
}
static int nibble(u8 c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

/* n PSK PROLOGUE INIT_E RESP_E PAYLOAD...: a full session, both roles here.
 * Prints each message, then the handshake hash; "fail" if a side rejects. */
static void session(u32 payloads) {
  struct noise initiator, responder;
  struct noise_cipher i_send, i_receive, r_send, r_receive;
  noise_start(&initiator, field[1], field_bytes[1], field[0]);
  noise_start(&responder, field[1], field_bytes[1], field[0]);
  for (u32 m = 0; m < payloads; ++m) {
    const u8 *payload = field[4 + m];
    u32 bytes = field_bytes[4 + m], sent;
    int from_initiator = m % 2 == 0;
    long got;
    if (m < 2) {
      struct noise *writer = from_initiator ? &initiator : &responder,
                   *reader = from_initiator ? &responder : &initiator;
      sent = noise_write(writer, field[from_initiator ? 2 : 3], payload, bytes, work);
      emit_hex(work, sent);
      got = sent ? noise_read(reader, work, sent) : -1;
      if (got >= 0 && m == 1) {
        noise_split(&initiator, 1, &i_send, &i_receive);
        noise_split(&responder, 0, &r_send, &r_receive);
        if (crypto_verify64(initiator.h, responder.h))
          got = -1;
      }
    } else {
      memcpy(work, payload, bytes);
      noise_seal(from_initiator ? &i_send : &r_send, 0, 0, work, bytes);
      sent = bytes + NOISE_TAG;
      emit_hex(work, sent);
      got = noise_open(from_initiator ? &r_receive : &i_receive, 0, 0, work, sent)
                ? (long)bytes
                : -1;
    }
    if (got != (long)bytes) {
      emit(" fail");
      return;
    }
    const u8 *plain = m < 2 ? work + NOISE_KEY : work;
    for (u32 i = 0; i < bytes; ++i)
      if (plain[i] != payload[i]) {
        emit(" fail");
        return;
      }
    emit(" ");
  }
  emit_hex(initiator.h, NOISE_HASH);
}

void entry(long *stack) __attribute__((noreturn));
void entry(long *stack) {
  (void)stack;
  u32 have = 0;
  long n;
  while ((n = call4(N_read, 0, (long)(input + have), sizeof(input) - have, 0)) > 0)
    have += (u32)n;
  for (u32 at = 0; at < have;) {
    u32 end = at, p = at, count = 0;
    while (end < have && input[end] != '\n')
      ++end;
    u8 op = input[at];
    while (p < end && input[p] != ' ')
      ++p;
    while (p < end && count < FIELDS) {
      u32 *size = &field_bytes[count];
      *size = 0;
      ++p;
      if (input[p] == '-')
        ++p;
      else
        for (; p < end && input[p] != ' '; p += 2)
          field[count][(*size)++] = (u8)(nibble(input[p]) << 4 | nibble(input[p + 1]));
      ++count;
    }
    if (op == 's') { /* seal KEY NONCE AAD MSG: ChaCha20-Poly1305, IETF nonce */
      crypto_aead_ctx x;
      crypto_aead_init_ietf(&x, field[0], field[1]);
      crypto_aead_write(&x, work, work + field_bytes[3], field[2], field_bytes[2],
                        field[3], field_bytes[3]);
      emit_hex(work, field_bytes[3] + NOISE_TAG);
    } else if (op == 'o') { /* open KEY NONCE AAD CT TAG */
      crypto_aead_ctx x;
      crypto_aead_init_ietf(&x, field[0], field[1]);
      if (field_bytes[4] != NOISE_TAG ||
          crypto_aead_read(&x, work, field[4], field[2], field_bytes[2], field[3],
                           field_bytes[3]))
        emit("fail");
      else
        emit_hex(work, field_bytes[3]);
    } else if (op == 'x') { /* X25519 SCALAR POINT */
      crypto_x25519(work, field[0], field[1]);
      emit_hex(work, 32);
    } else if (op == 'h') { /* HMAC-SHA512 KEY MSG */
      crypto_sha512_hmac(work, field[0], field_bytes[0], field[1], field_bytes[1]);
      emit_hex(work, 64);
    } else if (op == 'k') { /* HKDF-SHA512 IKM SALT INFO SIZE(u16 BE) */
      u32 size = (u32)field[3][0] << 8 | field[3][1];
      crypto_sha512_hkdf(work, size, field[0], field_bytes[0], field[1],
                         field_bytes[1], field[2], field_bytes[2]);
      emit_hex(work, size);
    } else if (op == 'r') { /* responder reads message 1: PSK PROLOGUE MESSAGE */
      struct noise s;
      noise_start(&s, field[1], field_bytes[1], field[0]);
      long got = noise_read(&s, field[2], field_bytes[2]);
      if (got < 0)
        emit("fail");
      else
        emit_hex(field[2] + NOISE_KEY, (u32)got);
    } else if (op == 'n') {
      session(count - 4);
    }
    emit("\n");
    at = end + 1;
  }
  for (u32 w = 0; w < out_at;) {
    long m = call4(N_write, 1, (long)(output + w), out_at - w, 0);
    if (m <= 0)
      finish(1);
    w += (u32)m;
  }
  finish(0);
}
