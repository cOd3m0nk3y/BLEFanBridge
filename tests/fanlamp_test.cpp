// Host-side test for fanlamp.h (the Arduino build ignores this folder).
//   g++ -std=c++17 -Wall -I.. fanlamp_test.cpp -lcrypto -o fanlamp_test && ./fanlamp_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../fanlamp.h"

using namespace fanlamp;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                                      \
    }                                                                  \
  } while (0)

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  int hi = -1;
  for (; *s; s++) {
    int c = *s, n;
    if (c >= '0' && c <= '9') n = c - '0';
    else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
    else continue;  // spaces
    if (hi < 0) hi = n; else { v.push_back((uint8_t)(hi << 4 | n)); hi = -1; }
  }
  return v;
}

static std::string toHex(const uint8_t *p, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%02X", p[i]); s += b; }
  return s;
}

static bool sameBytes(const char *what, const uint8_t *got, const std::vector<uint8_t> &want) {
  bool ok = want.size() == PACKET_LEN && std::memcmp(got, want.data(), PACKET_LEN) == 0;
  if (!ok) {
    std::printf("  FAIL %s\n    got  %s\n    want %s\n", what, toHex(got, PACKET_LEN).c_str(),
                toHex(want.data(), want.size()).c_str());
    failures++;
  }
  return ok;
}

// ---- 1. App vectors (CLAUDE.md): id 0xD2135C22, index 2, seed 0x2B53 -------------------
static void testAppVectors() {
  std::printf("app v3 vectors\n");
  struct V { const char *name; uint8_t tx; Command c; const char *hexAfterFlags; } vs[] = {
      {"pair tx117", 117, pair(), "1B03F0082080B801E122C6F2D3A76744A49F67F6B678458B532BB91B"},
      {"speed 2/6 tx127", 127, fanSpeed(2, 6), "1B03F0082080B80BE122C6F2D3A7675DA49F47F4B65B338B532B6390"},
      {"off tx128", 128, fanSpeed(0, 6), "1B03F0082080B8F4E122C6F2D3A7675DA49F47F6B6D9138B532BE608"},
  };
  for (auto &v : vs) {
    std::vector<uint8_t> want = hex("020119");
    auto rest = hex(v.hexAfterFlags);
    want.insert(want.end(), rest.begin(), rest.end());

    uint8_t out[PACKET_LEN];
    encode(out, Variant::V3, 0xD2135C22, 2, v.tx, 0x2B53, v.c);  // legacy API
    sameBytes(v.name, out, want);

    Identity who = {APP_V3, 0xD2135C22, 2};
    encode(out, who, v.tx, 0x2B53, v.c);  // Identity API
    sameBytes(v.name, out, want);

    Decoded d{};
    CHECK(decode(want.data(), want.size(), d) == DecodeResult::Ok);
    CHECK(d.who.id == 0xD2135C22 && d.who.index == 2 && d.tx == v.tx && d.seed == 0x2B53);
    CHECK(d.who.fmt.sign && d.who.fmt.adFlag == 0x19 && d.who.fmt.adType == 0x03);
    CHECK(d.who.fmt.prefix[0] == 0x20 && d.who.fmt.prefix[1] == 0x80 && d.who.fmt.prefix[2] == 0x00);
    CHECK(d.cmd.cmd == v.c.cmd && d.cmd.param1 == v.c.param1 && d.cmd.arg0 == v.c.arg0);
  }

  // v2 (unsigned) round-trips through decode with sign = false
  uint8_t out[PACKET_LEN];
  encode(out, Variant::V2, 0x12345678, 0, 42, 0x1111, fanSpeed(3, 6));
  Decoded d{};
  CHECK(decode(out, PACKET_LEN, d) == DecodeResult::Ok);
  CHECK(!d.who.fmt.sign && d.who.fmt.prefix[0] == 0x10 && d.who.fmt.prefix[1] == 0x80);
  CHECK(d.who.id == 0x12345678 && d.tx == 42 && d.cmd.arg0 == 3);
}

// ---- 2. Physical remote captures --------------------------------------------------------
struct RemoteVec {
  const char *name, *pkt;
  uint32_t id; uint8_t tx; uint16_t seed; uint8_t cmd, p1, a0, a1;
};

static void testRemote(const RemoteVec &v) {
  std::printf("remote %s\n", v.name);
  auto pkt = hex(v.pkt);
  CHECK(pkt.size() == PACKET_LEN);
  Decoded d{};
  DecodeResult r = decode(pkt.data(), pkt.size(), d);
  CHECK(r == DecodeResult::Ok);
  if (r != DecodeResult::Ok) return;

  std::printf("  id 0x%08X idx %u tx %u seed 0x%04X cmd 0x%02X p1 0x%02X args %u %u sign %d dev 0x%04X "
              "prefix %02X %02X %02X\n",
              d.who.id, d.who.index, d.tx, d.seed, d.cmd.cmd, d.cmd.param1, d.cmd.arg0, d.cmd.arg1,
              d.who.fmt.sign, d.who.fmt.devType, d.who.fmt.prefix[0], d.who.fmt.prefix[1], d.who.fmt.prefix[2]);
  CHECK(d.who.id == v.id);
  CHECK(d.who.index == 0);
  CHECK(d.tx == v.tx);
  CHECK(d.seed == v.seed);
  CHECK(d.cmd.cmd == v.cmd);
  CHECK(d.cmd.param1 == v.p1);
  CHECK(d.cmd.arg0 == v.a0 && d.cmd.arg1 == v.a1);
  CHECK(d.who.fmt.sign);
  CHECK(d.who.fmt.devType == 0x0400);
  CHECK(d.who.fmt.proto == 2 && d.who.fmt.hasFlags);
  CHECK(d.who.fmt.adFlag == 0x02 && d.who.fmt.adType == 0x16);
  CHECK(d.who.fmt.prefix[0] == 0x10 && d.who.fmt.prefix[1] == 0x00 && d.who.fmt.prefix[2] == 0x56);

  uint8_t out[PACKET_LEN];
  encode(out, d.who, d.tx, d.seed, d.cmd);
  sameBytes("re-encode", out, pkt);
}

static void testRemotes() {
  RemoteVec vs[] = {
      {"#1", "02 01 02 1B 16 F0 08 10 00 64 91 72 52 33 F1 42 82 5B F4 46 D9 50 51 C3 9B CF 64 43 B7 A9 A6",
       0x02227574, 10, 0xB743, 0x21, 0x14, 0, 0},
      {"#2", "02 01 02 1B 16 F0 08 10 00 4E 00 41 82 1D 99 50 D6 C5 ED 04 3F DF 56 16 BB 55 2B F3 19 EF 66",
       0x03309759, 212, 0x19F3, 0x21, 0x18, 0, 0},
      {"#3", "02 01 02 1B 16 F0 08 10 00 0C 49 56 CF AA 39 0C 25 EB 5F B9 7B 71 A0 15 07 1F 3B CC 7C F4 B6",
       0x02227574, 128, 0x7CCC, 0x21, 0x40, 255, 255},
  };
  for (auto &v : vs) testRemote(v);
}

// ---- 3. v1 frames -----------------------------------------------------------------------
struct V1Vec {
  const char *name, *pkt;
  bool hasFlags; uint8_t adFlag, adType, headerLen; const char *header;
  uint8_t v1; uint16_t forcedCrc2; uint8_t pairArg3;
  uint32_t id; uint8_t index, tx; uint16_t seed; uint8_t cmd, p1, a0, a1, a2;
};

static void testV1(const V1Vec &v) {
  std::printf("v1 %s\n", v.name);
  auto pkt = hex(v.pkt);
  CHECK(pkt.size() == PACKET_LEN);
  Decoded d{};
  DecodeResult r = decode(pkt.data(), pkt.size(), d);
  CHECK(r == DecodeResult::Ok);
  if (r != DecodeResult::Ok) return;
  const Format &f = d.who.fmt;
  std::printf("  id 0x%06X idx %u tx %u seed 0x%04X cmd 0x%02X p1 0x%02X args %02X %02X %02X v1 0x%02X crc2 0x%04X "
              "pairArg3 0x%02X\n",
              d.who.id, d.who.index, d.tx, d.seed, d.cmd.cmd, d.cmd.param1, d.cmd.arg0, d.cmd.arg1, d.cmd.arg2, f.v1,
              f.forcedCrc2, f.pairArg3);
  auto hdr = hex(v.header);
  CHECK(f.proto == 1);
  CHECK(f.hasFlags == v.hasFlags && f.adFlag == v.adFlag && f.adType == v.adType);
  CHECK(f.headerLen == v.headerLen && hdr.size() == v.headerLen && std::memcmp(f.header, hdr.data(), v.headerLen) == 0);
  CHECK(f.v1 == v.v1);
  CHECK(f.forcedCrc2 == v.forcedCrc2);
  CHECK(f.pairArg3 == v.pairArg3);
  CHECK(d.who.id == v.id && d.who.index == v.index);
  CHECK(d.tx == v.tx && d.seed == v.seed);
  CHECK(d.cmd.cmd == v.cmd && d.cmd.param1 == v.p1);
  CHECK(d.cmd.arg0 == v.a0 && d.cmd.arg1 == v.a1 && d.cmd.arg2 == v.a2);

  uint8_t out[PACKET_LEN];
  CHECK(encode(out, d.who, d.tx, d.seed, d.cmd) == PACKET_LEN);
  sameBytes("re-encode", out, pkt);
}

static void testV1Frames() {
  const uint8_t TYX = V1_SEED_ZERO | V1_CRC1_ZERO | V1_PAIR_ARG_ONLY_ON_PAIR;
  V1Vec vs[] = {
      // Our TYX remote, speed 2 pressed four times (captured by Learn diagnostics)
      {"TYX #1 tx1", "02 01 00 1B FF F0 FF B6 5F 2B 5E 00 FC 31 51 94 93 98 48 24 CB BB FC 71 A3 F4 55 68 CF FC C1",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x0094, 0, 1, 0, 0x32, 0, 2, 0, 0},
      {"TYX #2 tx4", "02 01 00 1B FF F0 FF B6 5F 2B 5E 00 FC 31 51 94 93 98 48 24 CB 1B FC 71 A3 F4 55 68 CF EE BC",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x0094, 0, 4, 0, 0x32, 0, 2, 0, 0},
      {"TYX #3 tx2", "02 01 00 1B FF F0 FF B6 5F 2B 5E 00 FC 31 51 94 93 98 48 24 CB 7B FC 71 A3 F4 55 68 CF EF 6F",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x0094, 0, 2, 0, 0x32, 0, 2, 0, 0},
      {"TYX #4 tx3", "02 01 00 1B FF F0 FF B6 5F 2B 5E 00 FC 31 51 94 93 98 48 24 CB FB FC 71 A3 F4 55 68 CF 0D A4",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x0094, 0, 3, 0, 0x32, 0, 2, 0, 0},
      // The same remote's light-on press (sniff diagnostics 2026-11-22). crc2 uses the light-on quirk.
      {"TYX light on tx2", "0201001BFFF0FFB65F2B5E00FC3151D093980824CB7BFC71A3F45568CF5C45",
       true, 0x00, 0xFF, 2, "F0 FF", TYX | 0x20, 0, 0x83, 0x0094, 0, 2, 0, 0x10, 0, 0, 0, 0},
      {"TYX light on tx3", "0201001BFFF0FFB65F2B5E00FC3151D093980824CBFBFC71A3F45568CFBE8E",
       true, 0x00, 0xFF, 2, "F0 FF", TYX | 0x20, 0, 0x83, 0x0094, 0, 3, 0, 0x10, 0, 0, 0, 0},
      {"TYX light on tx4", "0201001BFFF0FFB65F2B5E00FC3151D093980824CB1BFC71A3F45568CF5D96",
       true, 0x00, 0xFF, 2, "F0 FF", TYX | 0x20, 0, 0x83, 0x0094, 0, 4, 0, 0x10, 0, 0, 0, 0},
      {"TYX light on tx5", "0201001BFFF0FFB65F2B5E00FC3151D093980824CB9BFC71A3F45568CFBF5D",
       true, 0x00, 0xFF, 2, "F0 FF", TYX | 0x20, 0, 0x83, 0x0094, 0, 5, 0, 0x10, 0, 0, 0, 0},
      // Remote A (0x3068, one fan): light on uses the plain crc2 (quirk value 0).
      {"TYX A light on tx1", "0201001BFFF0FFB65F2B5E00FC3151D0AC940824CBBBFC71A3F45568CFEE69",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x3068, 0, 1, 0, 0x10, 0, 0, 0, 0},
      {"TYX A light on tx2", "0201001BFFF0FFB65F2B5E00FC3151D0AC940824CB7BFC71A3F45568CFFDC7",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x3068, 0, 2, 0, 0x10, 0, 0, 0, 0},
      {"TYX A light on tx3", "0201001BFFF0FFB65F2B5E00FC3151D0AC940824CBFBFC71A3F45568CF1F0C",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x3068, 0, 3, 0, 0x10, 0, 0, 0, 0},
      {"TYX A light on tx5", "0201001BFFF0FFB65F2B5E00FC3151D0AC940824CB9BFC71A3F45568CF1EDF",
       true, 0x00, 0xFF, 2, "F0 FF", TYX, 0, 0x83, 0x3068, 0, 5, 0, 0x10, 0, 0, 0, 0},
      // NicoIIT codec.py examples: "remote - v1" (no flags AD, xor1, forced crc2 0x7293) and "other - v1a"
      {"NicoIIT remote-v1", "1E FF 56 55 18 87 52 B6 5F 2B 5E 00 FC 31 51 50 50 9A 08 24 0A EC FC A9 7B 8E 0D 4A 67 60 57",
       false, 0x00, 0xFF, 5, "56 55 18 87 52", V1_XOR1, 0x7293, 0x83, 0x014057, 0, 235, 0x5E1A, 0x11, 0, 0, 0, 0x83},
      {"NicoIIT other-v1a", "02 01 02 1B 03 77 F8 B6 5F 2B 5E 00 FC 31 51 50 CB 92 08 24 CB BB FC 14 C6 9E B0 E9 EA 73 A4",
       true, 0x02, 0x03, 2, "77 F8", V1_XOR1 | V1_PAIR_ARG_ONLY_ON_PAIR, 0, 0x83, 0x01508E, 0, 1, 0x56A7, 0x11, 0, 0, 0, 0},
  };
  for (auto &v : vs) testV1(v);

  // A new command as the TYX remote: speed 3, next tx. Must decode back to the same fields.
  std::printf("v1 TYX new command\n");
  auto cap = hex(vs[0].pkt);
  Decoded d{};
  CHECK(decode(cap.data(), cap.size(), d) == DecodeResult::Ok);
  Command c = fanSpeedFor(d.who.fmt, 3, 6);
  CHECK(c.cmd == 0x32 && c.arg0 == 3);
  Command off = fanSpeedFor(d.who.fmt, 0, 6);  // sniffed: fan off = 0x31 arg 0
  CHECK(off.cmd == 0x31 && off.arg0 == 0);
  Command rev = fanDirectionFor(d.who.fmt, true);  // sniffed: 0x15 arg0 1
  CHECK(rev.cmd == 0x15 && rev.arg0 == 1 && rev.param1 == 0);
  uint8_t out[PACKET_LEN];
  CHECK(encode(out, d.who, 5, 0x1234, c) == PACKET_LEN);  // seed is ignored (V1_SEED_ZERO)
  Decoded d2{};
  CHECK(decode(out, PACKET_LEN, d2) == DecodeResult::Ok);
  CHECK(d2.who.id == 0x0094 && d2.tx == 5 && d2.seed == 0 && d2.cmd.cmd == 0x32 && d2.cmd.arg0 == 3);
  CHECK(d2.who.fmt.v1 == d.who.fmt.v1 && d2.who.fmt.adFlag == 0x00 && d2.who.fmt.adType == 0xFF);
  std::printf("  speed 3 tx 5: %s\n", toHex(out, PACKET_LEN).c_str());

  // TYX remote buttons with a non-zero third arg (sniffed: cmd 0x04..0x07, args 00 00 80/81).
  // Decoding one must keep args[2] as a plain argument, not mistake it for pairArg3.
  Command light = {0x04, 0, 0, 0, 0x81};
  CHECK(encode(out, d.who, 2, 0, light) == PACKET_LEN);
  Decoded d3{};
  CHECK(decode(out, PACKET_LEN, d3) == DecodeResult::Ok);
  CHECK(d3.cmd.cmd == 0x04 && d3.cmd.arg2 == 0x81);
  CHECK(d3.who.fmt.v1 == (V1_SEED_ZERO | V1_CRC1_ZERO | V1_PAIR_ARG_ONLY_ON_PAIR) && d3.who.fmt.pairArg3 == 0x83);
  uint8_t out2[PACKET_LEN];
  encode(out2, d3.who, 7, 0, fanSpeedFor(d3.who.fmt, 2, 6));  // a later command learned from that press
  Decoded d4{};
  CHECK(decode(out2, PACKET_LEN, d4) == DecodeResult::Ok && d4.cmd.arg2 == 0);
}

// ---- 4. Rejections -----------------------------------------------------------------------
static void testRejections() {
  std::printf("rejections\n");
  Decoded d{};
  auto v1bad = hex("02 01 00 1B FF F0 FF B6 5F 2B 5E 00 FC 31 51 94 93 98 48 24 CB BB FC 71 A3 F4 55 68 CF FC C1");
  v1bad[20] ^= 0x10;  // corrupt a struct byte (not the prefix): crc2 must catch it
  CHECK(decode(v1bad.data(), v1bad.size(), d) == DecodeResult::BadCrc);

  auto bad = hex("02 01 02 1B 16 F0 08 10 00 64 91 72 52 33 F1 42 82 5B F4 46 D9 50 51 C3 9B CF 64 43 B7 A9 A6");
  bad[15] ^= 0x01;  // corrupt one payload byte
  CHECK(decode(bad.data(), bad.size(), d) == DecodeResult::BadCrc);

  auto other = hex("02 01 06 03 03 AA FE 0B 09 53 6F 6D 65 44 65 76 69 63 65");  // unrelated beacon
  CHECK(decode(other.data(), other.size(), d) == DecodeResult::NotFanLamp);

  CHECK(decode(other.data(), 0, d) == DecodeResult::NotFanLamp);
  auto trunc = hex("02 01 02 1B 16 F0 08 10 00");  // structure claims more bytes than present
  CHECK(decode(trunc.data(), trunc.size(), d) == DecodeResult::NotFanLamp);
}

int main() {
  testAppVectors();
  testRemotes();
  testV1Frames();
  testRejections();
  if (failures) { std::printf("\n%d check(s) FAILED\n", failures); return 1; }
  std::printf("\nall checks passed\n");
  return 0;
}
