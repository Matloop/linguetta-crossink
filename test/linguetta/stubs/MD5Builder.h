#pragma once
// Host stand-in for Arduino's MD5Builder (RFC 1321), enough for KOReaderDocumentId.

#include <cstdint>
#include <cstring>
#include <string>

class MD5Builder {
 public:
  void begin() {
    a0 = 0x67452301;
    b0 = 0xefcdab89;
    c0 = 0x98badcfe;
    d0 = 0x10325476;
    len = 0;
    bufLen = 0;
  }
  void add(const char* s) { add(reinterpret_cast<const uint8_t*>(s), strlen(s)); }
  void add(const uint8_t* data, size_t n) {
    len += n;
    while (n > 0) {
      const size_t take = std::min(n, static_cast<size_t>(64 - bufLen));
      memcpy(buf + bufLen, data, take);
      bufLen += take;
      data += take;
      n -= take;
      if (bufLen == 64) {
        block(buf);
        bufLen = 0;
      }
    }
  }
  void calculate() {
    const uint64_t bits = len * 8;
    const uint8_t pad = 0x80;
    add(&pad, 1);
    const uint8_t zero = 0;
    while (bufLen != 56) add(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = static_cast<uint8_t>(bits >> (8 * i));
    add(lenBytes, 8);
    const uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++) digest[i * 4 + j] = static_cast<uint8_t>(words[i] >> (8 * j));
  }
  std::string toString() const {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (uint8_t b : digest) {
      s.push_back(hex[b >> 4]);
      s.push_back(hex[b & 15]);
    }
    return s;
  }

 private:
  static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
      m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
    uint32_t a = a0, b = b0, c = c0, d = d0;
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const uint32_t tmp = d;
      d = c;
      c = b;
      b = b + rotl(a + f + K[i] + m[g], R[i]);
      a = tmp;
    }
    a0 += a;
    b0 += b;
    c0 += c;
    d0 += d;
  }
  uint32_t a0 = 0, b0 = 0, c0 = 0, d0 = 0;
  uint64_t len = 0;
  uint8_t buf[64] = {};
  size_t bufLen = 0;
  uint8_t digest[16] = {};
};
