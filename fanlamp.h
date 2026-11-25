// fanlamp.h - FanLamp Pro (v1 / v2 / v3) BLE advertising packet encoder + decoder
//
// Ported from NicoIIT's esphome-components (ble_adv_handler/fanlamp_pro.cpp and
// ble_adv_handler.cpp), MIT License, Copyright (c) 2023-2025 NicoIIT.
// https://github.com/NicoIIT/esphome-components
//
// Builds the complete raw advertising payload (31 bytes for every known variant) the
// FanLamp Pro app or a physical FanLamp remote broadcasts. No connection or pairing
// handshake exists: the fan listens for these broadcasts and obeys any "remote" ID it
// was paired with.
//
// Frame:  [02 01 <adFlag>]  <len> <adType> <header (2..5 bytes)> <24-byte body>
//         (the flags structure is optional; NicoIIT's "remote v1" has none)
//
// v2/v3 body (header F0 08) - see "v2/v3 payload" below.
// v1 body: 8-byte constant prefix AA 98 43 AF 0B 46 46 46, then a 14-byte struct
//   cmd, group_index (LE: id & 0xF0FF | index << 8), args[3], tx, param1, src, r2,
//   seed (BE), crc16 (BE, crc16be(struct[0..11], ~seed)), then crc2 (2 bytes:
//   crc16be(struct[0..13], crc16be(prefix[1..5], 0xFFFF)) BE, or a forced constant LE).
//   The whole 24-byte body is bit-reversed per byte, then whitened with an LFSR (seed 0x6F).
//   src = seed8 ^ (id >> 16), r2 = seed8 (or both seed8 ^ 1 with "xor1").
//   Variants seen: our TYX remote (header F0 FF in manufacturer data, flags AD 0x00,
//   seed always 0, crc16 left 0, crc2 computed), NicoIIT "remote v1" (header
//   56 55 18 87 52, no flags AD, xor1, forced crc2 0x7293), "other v1a" (77 F8, xor1).
//
// v2/v3 payload, 31-byte frame:
//   [0..2]  02 01 <adFlag>           AD: flags
//   [3..4]  1B <adType>              AD: 27 bytes, 0x03 (app) or 0x16 (physical remote)
//   [5..6]  F0 08                    FanLamp header
//   [7..30] p[0..23]                 payload:
//     p[0..2]  prefix                (app v3 20 80 00, app v2 10 80 00, remote 10 00 56)
//     p[3]     tx count
//     p[4..5]  device type (LE)      0x0400 FanLamp Pro, 0x0100 also seen
//     p[6..9]  remote id (LE)
//     p[10]    group index
//     p[11..12] cmd (LE, high byte 0)
//     p[13]    param0 (0)
//     p[14]    param1, p[15] arg0, p[16] arg1
//     p[17..18] AES sign of p[1..16] (LE), or 0 when the format is unsigned
//     p[19]    spare (0)
//     p[20..21] seed (LE)
//     p[22..23] crc16be(p[0..21], ~seed) (LE)
//   p[2..19] are whitened with the seed's low byte (after signing, before the CRC).

#pragma once
#include <stdint.h>
#include <string.h>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include "mbedtls/aes.h"
#else
#include <openssl/evp.h>  // host-side unit tests only
#endif

namespace fanlamp {

enum class Variant : uint8_t { V2 = 2, V3 = 3 };

// Command bytes (default_translator_fanlamp_common / flv2)
static constexpr uint8_t CMD_PAIR      = 0x28;
static constexpr uint8_t CMD_UNPAIR    = 0x45;
static constexpr uint8_t CMD_ALL_OFF   = 0x6F;
static constexpr uint8_t CMD_LIGHT_ON  = 0x10;
static constexpr uint8_t CMD_LIGHT_OFF = 0x11;
static constexpr uint8_t CMD_LIGHT_TOGGLE = 0x09;
static constexpr uint8_t CMD_FAN_DIR   = 0x15;  // param1: 0 = forward, 1 = reverse
static constexpr uint8_t CMD_FAN_SPEED = 0x31;  // param1: 0x00 (3 speeds) / 0x20 (6 speeds), arg0: speed, 0 = off

static constexpr uint8_t CMD_V1_FAN_SPEED6 = 0x32;  // v1, 6-speed fans: arg0 = speed (seen from a real remote)

// Extra buttons sniffed from the user's TYX (v1) remotes, 2026-11-22. Sent as-is; not used for
// app-format (v2/v3) fans.
static constexpr uint8_t CMD_V1_BRIGHT_UP   = 0x04;  // arg2: 0x80 pressed, then 0x81 released
static constexpr uint8_t CMD_V1_BRIGHT_DOWN = 0x05;
static constexpr uint8_t CMD_V1_TEMP_UP     = 0x06;  // the remote's "temperature +"
static constexpr uint8_t CMD_V1_TEMP_DOWN   = 0x07;
static constexpr uint8_t CMD_V1_CCT         = 0x21;  // arg0 = cold level, arg1 = warm level (K button)
static constexpr uint8_t CMD_V1_NIGHT       = 0x23;
static constexpr uint8_t CMD_V1_BREEZE      = 0x33;  // wavy-line button, arg0 2
static constexpr uint8_t V1_PRESS = 0x80, V1_RELEASE = 0x81;

static constexpr size_t PACKET_LEN = 31;
static constexpr size_t BODY_LEN = 24;

struct Command {
  uint8_t cmd = 0;
  uint8_t param1 = 0;
  uint8_t arg0 = 0;
  uint8_t arg1 = 0;
  uint8_t arg2 = 0;  // v1 only (third arg byte)
};

inline Command pair()            { return {CMD_PAIR, 0, 0, 0}; }
inline Command unpair()          { return {CMD_UNPAIR, 0, 0, 0}; }
inline Command lightOn()         { return {CMD_LIGHT_ON, 0, 0, 0}; }
inline Command lightOff()        { return {CMD_LIGHT_OFF, 0, 0, 0}; }
inline Command allOff()          { return {CMD_ALL_OFF, 0, 0, 0}; }
// v1 extras (see CMD_V1_*). Press-style ones are built with arg2 = V1_PRESS; the sender switches to
// V1_RELEASE partway through the burst, like the remote.
inline Command v1Press(uint8_t cmd)              { return {cmd, 0, 0, 0, V1_PRESS}; }
inline Command v1Cct(uint8_t cold, uint8_t warm) { return {CMD_V1_CCT, 0, cold, warm, 0}; }
inline Command v1Night()                         { return {CMD_V1_NIGHT, 0, 0, 0, 0}; }
inline Command v1Breeze()                        { return {CMD_V1_BREEZE, 0, 2, 0, 0}; }
// v2/v3 forms (the FanLamp Pro app). For a learned identity use the *For(fmt, ...) helpers below.
inline Command fanDirection(bool reverse) { return {CMD_FAN_DIR, (uint8_t)(reverse ? 1 : 0), 0, 0}; }
// speed: 0 = off, 1..speedCount. speedCount must be 3 or 6.
inline Command fanSpeed(uint8_t speed, uint8_t speedCount) {
  return {CMD_FAN_SPEED, (uint8_t)(speedCount == 6 ? 0x20 : 0x00), speed, 0};
}

// v1 option bits (Format::v1)
static constexpr uint8_t V1_XOR1 = 0x01;                   // src = r2 = seed8 ^ 1
static constexpr uint8_t V1_SEED_ZERO = 0x02;              // seed is always 0
static constexpr uint8_t V1_CRC1_ZERO = 0x04;              // the struct crc16 is left 0
static constexpr uint8_t V1_PAIR_ARG_ONLY_ON_PAIR = 0x08;  // args[2] = pairArg3 only for pair
// High nibble: TYX light-on crc2 quirk value n (see crc2V1). Per remote: 2 for the user's
// "both fans" remote 0x0094, 0 for remote 0x3068. Only learnable from a light-on press.
static constexpr uint8_t V1_LIGHT_CRC_SHIFT = 4;
static constexpr uint8_t V1_LIGHT_CRC_MASK = 0xF0;
inline uint8_t v1LightCrc(uint8_t v1) { return (v1 & V1_LIGHT_CRC_MASK) >> V1_LIGHT_CRC_SHIFT; }

// How a sender frames its packets.
struct Format {
  uint8_t proto;        // 2: v2/v3 body, 1: v1 body
  bool hasFlags;        // send a flags AD structure (02 01 adFlag) first
  uint8_t adFlag;
  uint8_t adType;       // AD type of the FanLamp structure: 0x03, 0x16 or 0xFF
  uint8_t headerLen;    // 2..5
  uint8_t header[5];    // F0 08 (v2/v3); F0 FF, 77 F8, 56 55 18 87 52 ... (v1)
  // v2/v3 only
  uint8_t prefix[3];    // p[0..2]
  uint16_t devType;     // p[4..5]
  bool sign;            // AES sign in p[17..18], else 0
  // v1 only
  uint8_t v1;           // V1_* bits
  uint8_t pairArg3;     // args[2] of the pair command
  uint16_t forcedCrc2;  // 0 = computed
};

static constexpr Format APP_V3 = {2, true, 0x19, 0x03, 2, {0xF0, 0x08}, {0x20, 0x80, 0x00}, 0x0400, true, 0, 0, 0};
static constexpr Format APP_V2 = {2, true, 0x19, 0x03, 2, {0xF0, 0x08}, {0x10, 0x80, 0x00}, 0x0400, false, 0, 0, 0};

// Fan speed / direction in the form a given sender uses.
// Sniffed from the TYX v1 remote (6-speed fan): speed n = 0x32 arg0 n, off = 0x31 arg0 0,
// direction = 0x15 arg0 0/1. v1 3-speed (0x31, arg0 = speed) is NicoIIT's flv1 table, unverified.
inline Command fanSpeedFor(const Format &f, uint8_t speed, uint8_t speedCount) {
  if (f.proto == 1) {
    if (speed == 0) return {CMD_FAN_SPEED, 0, 0, 0};
    return {speedCount == 6 ? CMD_V1_FAN_SPEED6 : CMD_FAN_SPEED, 0, speed, 0};
  }
  return fanSpeed(speed, speedCount);
}
inline Command fanDirectionFor(const Format &f, bool reverse) {
  if (f.proto == 1) return {CMD_FAN_DIR, 0, (uint8_t)(reverse ? 1 : 0), 0};
  return fanDirection(reverse);
}

// Who we send as: a format plus the remote ID and group index the fan is paired with.
struct Identity {
  Format fmt;
  uint32_t id;
  uint8_t index;
};

// CRC-16/CCITT, poly 0x1021, MSB first, no reflection, no final xor
// (same as esphome::crc16be with default arguments)
inline uint16_t crc16be(const uint8_t *data, size_t len, uint16_t crc) {
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// XOR whitening; applying it twice with the same seed restores the input.
inline void whiten(uint8_t *buf, uint8_t size, uint8_t seed) {
  static const uint8_t XBOXES[32] = {
      0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
      0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75};
  for (uint8_t i = 0; i < size; ++i) {
    buf[i] ^= XBOXES[(seed + i + 9) & 0x1F];
    buf[i] ^= seed;
  }
}

inline void aes128_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
#else
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int n = 0;
  EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr);
  EVP_CIPHER_CTX_set_padding(ctx, 0);
  EVP_EncryptUpdate(ctx, out, &n, in, 16);
  EVP_CIPHER_CTX_free(ctx);
#endif
}

inline uint16_t sign(const uint8_t *buf16, uint8_t txCount, uint16_t seed) {
  uint8_t key[16] = {0, 0, 0, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16};
  key[0] = seed & 0xFF;
  key[1] = (seed >> 8) & 0xFF;
  key[2] = txCount;
  uint8_t out[16];
  aes128_ecb(key, buf16, out);
  uint16_t s = (uint16_t)out[0] | ((uint16_t)out[1] << 8);
  return s == 0 ? 0xFFFF : s;
}

inline void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
inline uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

// ---- v1 helpers (BleAdvEncoder::whiten / reverse_byte in NicoIIT's ble_adv_handler.cpp) ----
static constexpr uint8_t V1_PREFIX[8] = {0xAA, 0x98, 0x43, 0xAF, 0x0B, 0x46, 0x46, 0x46};

inline void whitenV1(uint8_t *buf, size_t len, uint8_t seed) {
  uint8_t r = seed;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = 0;
    for (uint8_t j = 0; j < 8; j++) {
      r <<= 1;
      if (r & 0x80) {
        r ^= 0x11;
        b |= 1 << j;
      }
      r &= 0x7F;
    }
    buf[i] ^= b;
  }
}

inline uint8_t reverseByte(uint8_t x) {
  x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
  x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
  return ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
}

// crc2 over a plain (un-whitened) v1 body b: crc16be(struct, crc16be(b[1..5], 0xFFFF)).
// TYX quirk (crc1 left 0): for light on (0x10) some remotes compute crc2 as if the group-index
// nibble (low nibble of struct[2]) were XORed with n = v1LightCrc(v1), while sending it unchanged.
// Remote 0x0094 (controls both fans) uses n = 2; remote 0x3068 uses 0 (plain). Found from captured
// light-on packets 2026-11-22; every other captured command uses the plain crc2.
inline uint16_t crc2V1(const uint8_t b[BODY_LEN], uint8_t v1) {
  uint8_t s[14];
  memcpy(s, b + sizeof(V1_PREFIX), 14);
  if ((v1 & V1_CRC1_ZERO) && s[0] == CMD_LIGHT_ON) s[2] ^= v1LightCrc(v1);
  return crc16be(s, 14, crc16be(b + 1, 5, 0xFFFF));
}

inline void encodeBodyV1(uint8_t *out, const Identity &who, uint8_t txCount, uint16_t seed, const Command &c) {
  const Format &f = who.fmt;
  uint8_t b[BODY_LEN] = {0};
  memcpy(b, V1_PREFIX, sizeof(V1_PREFIX));
  uint8_t *d = b + sizeof(V1_PREFIX);  // 14-byte struct

  uint8_t args[3] = {c.arg0, c.arg1, c.arg2};
  if (c.cmd == CMD_PAIR) {
    args[0] = who.id & 0xFF;
    args[1] = (who.id >> 8) & 0xF0;
    args[2] = f.pairArg3;
  } else if (!(f.v1 & V1_PAIR_ARG_ONLY_ON_PAIR)) {
    args[2] = f.pairArg3;
  }
  if (f.v1 & V1_SEED_ZERO) seed = 0;
  uint8_t seed8 = seed & 0xFF;
  uint16_t gi = (uint16_t)((who.id & 0xF0FF) + ((uint16_t)(who.index & 0x0F) << 8));
  bool xor1 = f.v1 & V1_XOR1;

  d[0] = c.cmd;
  d[1] = gi & 0xFF; d[2] = gi >> 8;
  d[3] = args[0]; d[4] = args[1]; d[5] = args[2];
  d[6] = txCount;
  d[7] = c.param1;
  d[8] = xor1 ? seed8 ^ 1 : seed8 ^ ((who.id >> 16) & 0xFF);  // src
  d[9] = xor1 ? seed8 ^ 1 : seed8;                            // r2
  d[10] = seed >> 8; d[11] = seed & 0xFF;
  uint16_t crc1 = (f.v1 & V1_CRC1_ZERO) ? 0 : crc16be(d, 12, (uint16_t)~seed);
  d[12] = crc1 >> 8; d[13] = crc1 & 0xFF;
  if (f.forcedCrc2) {
    b[22] = f.forcedCrc2 & 0xFF; b[23] = f.forcedCrc2 >> 8;
  } else {
    uint16_t crc2 = crc2V1(b, f.v1);
    b[22] = crc2 >> 8; b[23] = crc2 & 0xFF;
  }
  for (size_t i = 0; i < BODY_LEN; i++) b[i] = reverseByte(b[i]);
  whitenV1(b, BODY_LEN, 0x6F);
  memcpy(out, b, BODY_LEN);
}

inline void encodeBodyV2(uint8_t *p, const Identity &who, uint8_t txCount, uint16_t seed, const Command &c) {
  p[0] = who.fmt.prefix[0];
  p[1] = who.fmt.prefix[1];
  p[2] = who.fmt.prefix[2];
  p[3] = txCount;
  put16(p + 4, who.fmt.devType);
  p[6] = who.id & 0xFF; p[7] = (who.id >> 8) & 0xFF; p[8] = (who.id >> 16) & 0xFF; p[9] = (who.id >> 24) & 0xFF;
  p[10] = who.index;
  put16(p + 11, c.cmd);
  p[13] = 0;                                              // param0 (unused)
  p[14] = c.param1;
  p[15] = c.arg0;
  p[16] = c.arg1;
  put16(p + 17, who.fmt.sign ? sign(p + 1, txCount, seed) : 0);
  p[19] = 0;                                              // spare
  put16(p + 20, seed);

  whiten(p + 2, 18, (uint8_t)(seed & 0xFF));
  uint16_t crc = crc16be(p, 22, (uint16_t)~seed);
  p[22] = crc & 0xFF; p[23] = crc >> 8;
}

// Builds the full raw advertising payload into out (zero-padded to PACKET_LEN).
// Returns its length (31 for every known variant), or 0 if the format doesn't fit.
//   who     : format + remote id + group index
//   txCount : must change on every new command
//   seed    : random per command, 1..0xFFF4 (ignored by v1 formats with V1_SEED_ZERO)
inline size_t encode(uint8_t out[PACKET_LEN], const Identity &who, uint8_t txCount, uint16_t seed,
                     const Command &c) {
  const Format &f = who.fmt;
  memset(out, 0, PACKET_LEN);
  size_t n = 0;
  if (f.hasFlags) { out[n++] = 0x02; out[n++] = 0x01; out[n++] = f.adFlag; }  // AD: flags
  uint8_t hl = f.headerLen;
  if (hl > sizeof(f.header) || n + 2 + hl + BODY_LEN > PACKET_LEN) return 0;
  out[n++] = (uint8_t)(1 + hl + BODY_LEN);                                      // AD: FanLamp structure
  out[n++] = f.adType;
  memcpy(out + n, f.header, hl);
  n += hl;
  if (f.proto == 1) encodeBodyV1(out + n, who, txCount, seed, c);
  else encodeBodyV2(out + n, who, txCount, seed, c);
  return n + BODY_LEN;
}

// FanLamp Pro app packet (v3 signed / v2 unsigned). Same bytes as before the Identity API.
inline size_t encode(uint8_t out[PACKET_LEN], Variant variant, uint32_t id, uint8_t index,
                     uint8_t txCount, uint16_t seed, const Command &c) {
  Identity who = {variant == Variant::V3 ? APP_V3 : APP_V2, id, index};
  return encode(out, who, txCount, seed, c);
}

// ---------------------------------------------------------------------
//  Decoder (used by "Learn from remote")
// ---------------------------------------------------------------------
enum class DecodeResult : uint8_t { Ok, BadCrc, NotFanLamp, Unsupported };

struct Decoded {
  Identity who;
  uint8_t tx;
  uint16_t seed;
  Command cmd;
};

// Un-whitens/un-reverses a 24-byte v1 body into b; true if it carries the v1 prefix.
inline bool unpackV1(const uint8_t *body, uint8_t b[BODY_LEN]) {
  memcpy(b, body, BODY_LEN);
  whitenV1(b, BODY_LEN, 0x6F);
  for (size_t i = 0; i < BODY_LEN; i++) b[i] = reverseByte(b[i]);
  return memcmp(b, V1_PREFIX, sizeof(V1_PREFIX)) == 0;
}

// v1 body -> d (everything except the framing, which the caller fills in).
inline DecodeResult decodeV1(const uint8_t b[BODY_LEN], Decoded &d) {
  const uint8_t *s = b + sizeof(V1_PREFIX);  // 14-byte struct
  uint16_t gi = (uint16_t)s[1] | ((uint16_t)s[2] << 8);
  uint16_t seed = ((uint16_t)s[10] << 8) | s[11];
  uint8_t seed8 = seed & 0xFF;
  uint8_t src = s[8], r2 = s[9];
  Format &f = d.who.fmt;
  f.v1 = 0;
  f.forcedCrc2 = 0;

  if (r2 == (uint8_t)(seed8 ^ 1)) f.v1 |= V1_XOR1;
  else if (r2 != seed8) return DecodeResult::Unsupported;

  uint16_t crc1 = ((uint16_t)s[12] << 8) | s[13];
  if (crc1 != crc16be(s, 12, (uint16_t)~seed)) {
    if (crc1 != 0) return DecodeResult::BadCrc;
    f.v1 |= V1_CRC1_ZERO;
  }
  uint16_t crc2 = ((uint16_t)b[22] << 8) | b[23];
  if (crc2 != crc2V1(b, f.v1) && (f.v1 & V1_CRC1_ZERO) && s[0] == CMD_LIGHT_ON) {
    for (uint8_t n = 1; n < 16; n++) {  // light-on quirk: find this remote's n
      uint8_t v = (uint8_t)(f.v1 | (n << V1_LIGHT_CRC_SHIFT));
      if (crc2 == crc2V1(b, v)) { f.v1 = v; break; }
    }
  }
  if (crc2 != crc2V1(b, f.v1)) {
    // A constant crc2 is only believable when crc1 vouches for the packet.
    if (f.v1 & V1_CRC1_ZERO) return DecodeResult::BadCrc;
    f.forcedCrc2 = (uint16_t)b[22] | ((uint16_t)b[23] << 8);
    if (!f.forcedCrc2) return DecodeResult::BadCrc;
  }
  if (seed == 0) f.v1 |= V1_SEED_ZERO;

  d.cmd.cmd = s[0];
  d.cmd.arg0 = s[3];
  d.cmd.arg1 = s[4];
  d.cmd.arg2 = s[5];
  d.cmd.param1 = s[7];
  // args[2] is a real argument for most remotes (the TYX remote sends 0x80/0x81 there with cmds
  // 0x04..0x07). Only the xor1 variants (NicoIIT "remote v1") repeat pairArg3 in every command.
  if (s[0] == CMD_PAIR) {
    f.pairArg3 = s[5];
    f.v1 |= V1_PAIR_ARG_ONLY_ON_PAIR;
  } else if (s[5] != 0 && (f.v1 & V1_XOR1)) {
    f.pairArg3 = s[5];
  } else {
    f.pairArg3 = 0x83;  // unknown until we see a pair packet; NicoIIT's fanlamp/remote v1 value
    f.v1 |= V1_PAIR_ARG_ONLY_ON_PAIR;
  }
  uint8_t remId = (f.v1 & V1_XOR1) ? 1 : (uint8_t)(src ^ seed8);
  d.who.id = (uint32_t)(gi & 0xF0FF) | ((uint32_t)remId << 16);
  d.who.index = (gi >> 8) & 0x0F;
  d.tx = s[6];
  d.seed = seed;
  f.proto = 1;
  f.prefix[0] = f.prefix[1] = f.prefix[2] = 0;
  f.devType = 0;
  f.sign = false;
  return DecodeResult::Ok;
}

// Parses a raw advertising payload (AD structures). On Ok, `d` holds everything needed to
// re-create the packet: encode(out, d.who, d.tx, d.seed, d.cmd).
// No heap use, safe to call from the Bluetooth callback.
inline DecodeResult decode(const uint8_t *adv, size_t len, Decoded &d) {
  bool hasFlags = false;
  uint8_t flags = 0;
  const uint8_t *fl = nullptr;      // v2/v3 body
  uint8_t flType = 0;
  uint8_t v1b[BODY_LEN];            // v1 body, already unpacked
  bool v1 = false;
  uint8_t v1Type = 0, v1HeaderLen = 0;
  const uint8_t *v1Header = nullptr;

  for (size_t i = 0; i < len;) {
    uint8_t l = adv[i];
    if (l == 0 || i + 1 + l > len) break;  // end of data / truncated structure
    uint8_t type = adv[i + 1];
    const uint8_t *data = adv + i + 2;
    size_t dlen = l - 1;
    if (type == 0x01 && dlen >= 1) {
      hasFlags = true;
      flags = data[0];
    } else if ((type == 0x03 || type == 0x16) && dlen >= 26 && data[0] == 0xF0 && data[1] == 0x08 && !fl) {
      fl = data + 2;
      flType = type;
    }
    // v1: the body is the last 24 bytes of the structure, after a 2..5-byte header
    if (!v1 && type != 0x01 && dlen >= BODY_LEN + 2 && dlen <= BODY_LEN + 5 && unpackV1(data + dlen - BODY_LEN, v1b)) {
      v1 = true;
      v1Type = type;
      v1Header = data;
      v1HeaderLen = (uint8_t)(dlen - BODY_LEN);
    }
    i += 1 + l;
  }

  d = Decoded();
  d.who.fmt.hasFlags = hasFlags;
  d.who.fmt.adFlag = flags;

  if (v1) {
    d.who.fmt.adType = v1Type;
    d.who.fmt.headerLen = v1HeaderLen;
    memcpy(d.who.fmt.header, v1Header, v1HeaderLen);
    return decodeV1(v1b, d);
  }
  if (!fl) return DecodeResult::NotFanLamp;

  uint8_t p[24];
  memcpy(p, fl, sizeof(p));
  uint16_t seed = get16(p + 20);
  if (crc16be(p, 22, (uint16_t)~seed) != (uint16_t)(((uint16_t)p[22]) | ((uint16_t)p[23] << 8)))
    return DecodeResult::BadCrc;
  whiten(p + 2, 18, (uint8_t)(seed & 0xFF));  // XOR: un-whitens

  uint16_t devType = get16(p + 4);
  if (devType != 0x0400 && devType != 0x0100) return DecodeResult::Unsupported;

  uint16_t s = get16(p + 17);
  bool signedPkt;
  if (s == 0) signedPkt = false;
  else if (s == sign(p + 1, p[3], seed)) signedPkt = true;
  else return DecodeResult::Unsupported;

  d.who.fmt.proto = 2;
  d.who.fmt.adType = flType;
  d.who.fmt.headerLen = 2;
  d.who.fmt.header[0] = 0xF0;
  d.who.fmt.header[1] = 0x08;
  d.who.fmt.prefix[0] = p[0];
  d.who.fmt.prefix[1] = p[1];
  d.who.fmt.prefix[2] = p[2];
  d.who.fmt.devType = devType;
  d.who.fmt.sign = signedPkt;
  d.who.id = (uint32_t)p[6] | ((uint32_t)p[7] << 8) | ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
  d.who.index = p[10];
  d.tx = p[3];
  d.seed = seed;
  d.cmd.cmd = p[11];
  d.cmd.param1 = p[14];
  d.cmd.arg0 = p[15];
  d.cmd.arg1 = p[16];
  return DecodeResult::Ok;
}

}  // namespace fanlamp
