#include "slothdb/execution/expression_executor.hpp"
#include "slothdb/binder/binder.hpp"
#include "slothdb/planner/planner.hpp"
#include "slothdb/execution/physical_planner.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include "slothdb/common/types/string_type.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <regex>

namespace slothdb {

static bool LikeMatch(const std::string &str, const std::string &pattern);

// Self-contained SHA-512 (FIPS 180-4) over arbitrary-length bytes.
// 64-bit state, 80 rounds, 128-bit big-endian length pad. Output
// is 128 lowercase hex chars.
// Test vectors:
//   SHA512('')    = cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc
//                   83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f
//                   63b931bd47417a81a538327af927da3e
//   SHA512('abc') = ddaf35a193617abacc417349ae20413112e6fa4e89a97ea2
//                   0a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd
//                   454d4423643ce80e2a9ac94fa54ca49f
static std::string Sha512Hex(const std::string &input) {
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
        0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
        0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
        0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
    };
    uint64_t H[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    auto ror = [](uint64_t x, int n) -> uint64_t {
        return (x >> n) | (x << (64 - n));
    };
    // Pad: append 0x80, zero-pad to 112 mod 128, then 128-bit BE length
    // (high 64 bits = 0 for inputs < 2^64 bits, which is everything we
    // handle in a single std::string).
    uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
    std::string msg = input;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 128 != 112) msg.push_back(0);
    // High 64 bits of 128-bit length: 0.
    for (int i = 0; i < 8; i++) msg.push_back(0);
    // Low 64 bits, big-endian.
    for (int i = 7; i >= 0; i--) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 128) {
        uint64_t W[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(&msg[chunk + i * 8]);
            W[i] = 0;
            for (int b = 0; b < 8; b++) {
                W[i] = (W[i] << 8) | static_cast<uint64_t>(p[b]);
            }
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = ror(W[i-15], 1) ^ ror(W[i-15], 8) ^ (W[i-15] >> 7);
            uint64_t s1 = ror(W[i-2], 19) ^ ror(W[i-2], 61) ^ (W[i-2] >> 6);
            W[i] = W[i-16] + s0 + W[i-7] + s1;
        }
        uint64_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
        for (int i = 0; i < 80; i++) {
            uint64_t S1 = ror(e, 14) ^ ror(e, 18) ^ ror(e, 41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = h + S1 + ch + K[i] + W[i];
            uint64_t S0 = ror(a, 28) ^ ror(a, 34) ^ ror(a, 39);
            uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = S0 + mj;
            h = g; g = f; f = e;
            e = d + t1;
            d = c; c = b; b = a;
            a = t1 + t2;
        }
        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    static const char *digs = "0123456789abcdef";
    std::string out;
    out.reserve(128);
    for (int i = 0; i < 8; i++) {
        for (int byte = 7; byte >= 0; byte--) {
            unsigned char b = (H[i] >> (byte * 8)) & 0xFF;
            out.push_back(digs[b >> 4]);
            out.push_back(digs[b & 0xF]);
        }
    }
    return out;
}

// Self-contained SHA-1 (FIPS 180-4) over arbitrary-length bytes.
// Verified vectors:
//   SHA1("")    = da39a3ee5e6b4b0d3255bfef95601890afd80709
//   SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89b
//   SHA1("The quick brown fox jumps over the lazy dog")
//             = 2fd4e1c67a2d28fced849ee1bb76e7391b93eb12
static std::string Sha1Hex(const std::string &input) {
    uint32_t H[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int n) -> uint32_t {
        return (x << n) | (x >> (32 - n));
    };
    uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
    std::string msg = input;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t W[80];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(&msg[chunk + i * 4]);
            W[i] = (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                    static_cast<uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; i++) {
            W[i] = rol(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);
        }
        uint32_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + W[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e;
    }
    static const char *digs = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 5; i++) {
        for (int byte = 3; byte >= 0; byte--) {
            unsigned char b = (H[i] >> (byte * 8)) & 0xFF;
            out.push_back(digs[b >> 4]);
            out.push_back(digs[b & 0xF]);
        }
    }
    return out;
}

// Self-contained SHA-256 (FIPS 180-4) over arbitrary-length bytes.
// Verified against the FIPS-180 test vectors:
//   SHA256("")    = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
//   SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
static std::string Sha256Hex(const std::string &input) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    auto ror = [](uint32_t x, int n) -> uint32_t {
        return (x >> n) | (x << (32 - n));
    };
    // Pad input: append 0x80, zero-pad to 56 mod 64, then 64-bit
    // big-endian bit length.
    uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
    std::string msg = input;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; i--) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t W[64];
        for (int i = 0; i < 16; i++) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(&msg[chunk + i * 4]);
            W[i] = (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                    static_cast<uint32_t>(p[3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(W[i-15], 7) ^ ror(W[i-15], 18) ^ (W[i-15] >> 3);
            uint32_t s1 = ror(W[i-2], 17) ^ ror(W[i-2], 19) ^ (W[i-2] >> 10);
            W[i] = W[i-16] + s0 + W[i-7] + s1;
        }
        uint32_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + W[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            h = g; g = f; f = e;
            e = d + t1;
            d = c; c = b; b = a;
            a = t1 + t2;
        }
        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    static const char *digs = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; i++) {
        for (int byte = 3; byte >= 0; byte--) {
            unsigned char b = (H[i] >> (byte * 8)) & 0xFF;
            out.push_back(digs[b >> 4]);
            out.push_back(digs[b & 0xF]);
        }
    }
    return out;
}

// Self-contained MD5 (RFC 1321) over arbitrary-length bytes.
// Verified against RFC 1321 test suite:
//   MD5("")        = d41d8cd98f00b204e9800998ecf8427e
//   MD5("a")       = 0cc175b9c0f1b6a831c399e269772661
//   MD5("abc")     = 900150983cd24fb0d6963f7d28e17f72
//   MD5("message digest") = f96b697d7cb7938d525a2f31aaf161d0
static std::string Md5Hex(const std::string &input) {
    auto rol = [](uint32_t x, int n) -> uint32_t {
        return (x << n) | (x >> (32 - n));
    };
    static const uint32_t T[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const int S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    uint32_t a0 = 0x67452301;
    uint32_t b0 = 0xefcdab89;
    uint32_t c0 = 0x98badcfe;
    uint32_t d0 = 0x10325476;

    // Build padded message: input || 0x80 || zero pad || 64-bit little-endian
    // length in bits.
    uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
    std::string msg = input;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; i++) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t M[16];
        for (int j = 0; j < 16; j++) {
            const unsigned char *p = reinterpret_cast<const unsigned char *>(&msg[chunk + j * 4]);
            M[j] = static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7*i) % 16; }
            uint32_t tmp = D;
            D = C;
            C = B;
            B = B + rol(A + F + T[i] + M[g], S[i]);
            A = tmp;
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    static const char *digs = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    uint32_t parts[4] = {a0, b0, c0, d0};
    for (int p = 0; p < 4; p++) {
        for (int byte = 0; byte < 4; byte++) {
            unsigned char b = (parts[p] >> (byte * 8)) & 0xFF;
            out.push_back(digs[b >> 4]);
            out.push_back(digs[b & 0xF]);
        }
    }
    return out;
}

void ExpressionExecutor::Execute(const BoundExpression &expr, DataChunk &input,
                                  Vector &result, idx_t count) {
    switch (expr.GetExpressionType()) {
    case BoundExpressionType::COLUMN_REF:
        ExecuteColumnRef(static_cast<const BoundColumnRef &>(expr), input, result, count);
        break;
    case BoundExpressionType::CONSTANT:
        ExecuteConstant(static_cast<const BoundConstant &>(expr), result, count);
        break;
    case BoundExpressionType::COMPARISON:
        ExecuteComparison(static_cast<const BoundComparison &>(expr), input, result, count);
        break;
    case BoundExpressionType::CONJUNCTION:
        ExecuteConjunction(static_cast<const BoundConjunction &>(expr), input, result, count);
        break;
    case BoundExpressionType::ARITHMETIC:
        ExecuteArithmetic(static_cast<const BoundArithmetic &>(expr), input, result, count);
        break;
    case BoundExpressionType::NEGATION:
        ExecuteNegation(static_cast<const BoundNegation &>(expr), input, result, count);
        break;
    case BoundExpressionType::IS_NULL:
        ExecuteIsNull(static_cast<const BoundIsNull &>(expr), input, result, count);
        break;
    case BoundExpressionType::IS_BOOL:
        ExecuteIsBool(static_cast<const BoundIsBool &>(expr), input, result, count);
        break;
    case BoundExpressionType::UNARY_MINUS:
        ExecuteUnaryMinus(static_cast<const BoundUnaryMinus &>(expr), input, result, count);
        break;
    case BoundExpressionType::FUNCTION:
        ExecuteFunction(static_cast<const BoundFunction &>(expr), input, result, count);
        break;
    case BoundExpressionType::CAST:
        ExecuteCast(static_cast<const BoundCast &>(expr), input, result, count);
        break;
    case BoundExpressionType::SUBQUERY:
        ExecuteSubquery(static_cast<const BoundSubqueryExpression &>(expr), input, result, count);
        break;
    default:
        throw NotImplementedException("Expression executor for type");
    }
}

Value ExpressionExecutor::ExecuteScalar(const BoundExpression &expr) {
    if (expr.GetExpressionType() == BoundExpressionType::CONSTANT) {
        return static_cast<const BoundConstant &>(expr).value;
    }
    // Handle unary minus on constant (e.g., -5 in INSERT VALUES).
    if (expr.GetExpressionType() == BoundExpressionType::UNARY_MINUS) {
        auto &um = static_cast<const BoundUnaryMinus &>(expr);
        auto child_val = ExecuteScalar(*um.child);
        if (child_val.IsNull()) return child_val;
        switch (child_val.type().id()) {
        case LogicalTypeId::INTEGER:
            return Value::INTEGER(-child_val.GetValue<int32_t>());
        case LogicalTypeId::BIGINT:
            return Value::BIGINT(-child_val.GetValue<int64_t>());
        case LogicalTypeId::DOUBLE:
            return Value::DOUBLE(-child_val.GetValue<double>());
        case LogicalTypeId::FLOAT:
            return Value::FLOAT(-child_val.GetValue<float>());
        default:
            break;
        }
    }
    // Handle function on constants (e.g., CAST in INSERT).
    if (expr.GetExpressionType() == BoundExpressionType::CAST) {
        auto &cast = static_cast<const BoundCast &>(expr);
        auto child_val = ExecuteScalar(*cast.child);
        if (child_val.IsNull()) return child_val;
        auto str = child_val.ToString();
        // Strict integer parse: trim ASCII whitespace, require leading
        // optional sign + digits + trailing whitespace only. Rejects
        // '1.5', '12abc', empty / whitespace-only, and overflow.
        // Previously std::stoi accepted '1.5' (silently truncates) and
        // leaked raw std::invalid_argument / out_of_range for non-
        // numeric or out-of-range strings.
        auto parse_strict_int64 = [&](const std::string &s) -> int64_t {
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i >= s.size()) {
                throw ConversionException(
                    "Could not convert string '" + s + "' to integer");
            }
            size_t start = i;
            if (s[i] == '+' || s[i] == '-') i++;
            size_t digit_start = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            if (i == digit_start) {
                throw ConversionException(
                    "Could not convert string '" + s + "' to integer");
            }
            size_t digit_end = i;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i != s.size()) {
                throw ConversionException(
                    "Could not convert string '" + s + "' to integer");
            }
            try {
                return std::stoll(s.substr(start, digit_end - start));
            } catch (const std::out_of_range &) {
                throw ConversionException(
                    "Value '" + s + "' out of range for integer");
            } catch (const std::exception &) {
                throw ConversionException(
                    "Could not convert string '" + s + "' to integer");
            }
        };
        auto parse_strict_double = [&](const std::string &s) -> double {
            try {
                size_t pos = 0;
                double d = std::stod(s, &pos);
                while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
                if (pos != s.size()) {
                    throw ConversionException(
                        "Could not convert string '" + s + "' to floating-point");
                }
                return d;
            } catch (const std::out_of_range &) {
                throw ConversionException(
                    "Value '" + s + "' out of range for floating-point");
            } catch (const ConversionException &) {
                throw;
            } catch (const std::exception &) {
                throw ConversionException(
                    "Could not convert string '" + s + "' to floating-point");
            }
        };
        switch (cast.GetReturnType().id()) {
        case LogicalTypeId::TINYINT: {
            int64_t v = parse_strict_int64(str);
            if (v < std::numeric_limits<int8_t>::min() ||
                v > std::numeric_limits<int8_t>::max()) {
                throw ConversionException(
                    "Value '" + str + "' out of range for TINYINT");
            }
            return Value::TINYINT(static_cast<int8_t>(v));
        }
        case LogicalTypeId::SMALLINT: {
            int64_t v = parse_strict_int64(str);
            if (v < std::numeric_limits<int16_t>::min() ||
                v > std::numeric_limits<int16_t>::max()) {
                throw ConversionException(
                    "Value '" + str + "' out of range for SMALLINT");
            }
            return Value::SMALLINT(static_cast<int16_t>(v));
        }
        case LogicalTypeId::INTEGER: {
            int64_t v = parse_strict_int64(str);
            if (v < std::numeric_limits<int32_t>::min() ||
                v > std::numeric_limits<int32_t>::max()) {
                throw ConversionException(
                    "Value '" + str + "' out of range for INTEGER");
            }
            return Value::INTEGER(static_cast<int32_t>(v));
        }
        case LogicalTypeId::BIGINT:
            return Value::BIGINT(parse_strict_int64(str));
        case LogicalTypeId::UTINYINT: {
            int64_t v = parse_strict_int64(str);
            if (v < 0 || v > std::numeric_limits<uint8_t>::max()) {
                throw ConversionException(
                    "Value '" + str + "' out of range for UTINYINT");
            }
            return Value::UTINYINT(static_cast<uint8_t>(v));
        }
        case LogicalTypeId::USMALLINT: {
            int64_t v = parse_strict_int64(str);
            if (v < 0 || v > std::numeric_limits<uint16_t>::max()) {
                throw ConversionException(
                    "Value '" + str + "' out of range for USMALLINT");
            }
            return Value::USMALLINT(static_cast<uint16_t>(v));
        }
        case LogicalTypeId::UINTEGER: {
            int64_t v = parse_strict_int64(str);
            if (v < 0 || v > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                throw ConversionException(
                    "Value '" + str + "' out of range for UINTEGER");
            }
            return Value::UINTEGER(static_cast<uint32_t>(v));
        }
        case LogicalTypeId::UBIGINT: {
            int64_t v = parse_strict_int64(str);
            if (v < 0) {
                throw ConversionException(
                    "Value '" + str + "' is negative; cannot cast to UBIGINT");
            }
            return Value::UBIGINT(static_cast<uint64_t>(v));
        }
        case LogicalTypeId::FLOAT: {
            double d = parse_strict_double(str);
            if (d > static_cast<double>(std::numeric_limits<float>::max()) ||
                d < -static_cast<double>(std::numeric_limits<float>::max())) {
                throw ConversionException(
                    "Value '" + str + "' out of range for FLOAT");
            }
            return Value::FLOAT(static_cast<float>(d));
        }
        case LogicalTypeId::BOOLEAN: {
            std::string lower;
            lower.reserve(str.size());
            for (char c : str) lower += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            if (lower == "true" || lower == "t" || lower == "yes" || lower == "y" || lower == "1") {
                return Value::BOOLEAN(true);
            }
            if (lower == "false" || lower == "f" || lower == "no" || lower == "n" || lower == "0") {
                return Value::BOOLEAN(false);
            }
            throw ConversionException(
                "Could not convert string '" + str + "' to BOOLEAN");
        }
        case LogicalTypeId::DOUBLE: {
            try {
                size_t pos = 0;
                double d = std::stod(str, &pos);
                while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t')) pos++;
                if (pos != str.size()) {
                    throw ConversionException(
                        "Could not convert string '" + str + "' to DOUBLE");
                }
                return Value::DOUBLE(d);
            } catch (const std::out_of_range &) {
                throw ConversionException(
                    "Value '" + str + "' out of range for DOUBLE");
            } catch (const ConversionException &) {
                throw;
            } catch (const std::exception &) {
                throw ConversionException(
                    "Could not convert string '" + str + "' to DOUBLE");
            }
        }
        case LogicalTypeId::VARCHAR: return Value::VARCHAR(str);
        case LogicalTypeId::DATE: {
            int32_t days;
            if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                throw ConversionException(
                    "Could not convert string '" + str + "' to DATE");
            }
            return Value::DATE(days);
        }
        case LogicalTypeId::TIMESTAMP:
        case LogicalTypeId::TIMESTAMP_TZ: {
            int64_t micros;
            if (!Value::TryParseTimestampMicros(str.data(), str.size(), micros)) {
                int32_t days;
                if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                    throw ConversionException(
                        "Could not convert string '" + str + "' to TIMESTAMP");
                }
                micros = static_cast<int64_t>(days) * 86400LL * 1000000LL;
            }
            return Value::TIMESTAMP(micros);
        }
        default: return Value::VARCHAR(str);
        }
    }
    // Fallback: evaluate any expression that doesn't depend on input
    // columns by running the full Execute() pipeline against an empty
    // input chunk into a single-row result vector. Lets INSERT VALUES
    // accept arithmetic / functions / CASE / COALESCE / IF / etc.
    // instead of throwing "ExecuteScalar only supports constants and
    // unary minus". An expression that references a column will hit
    // ExecuteColumnRef and throw inside Execute(); that's the
    // appropriate error for INSERT VALUES context anyway.
    DataChunk empty_input;
    Vector out(expr.GetReturnType(), 1);
    Execute(expr, empty_input, out, 1);
    return out.GetValue(0);
}

void ExpressionExecutor::ExecuteColumnRef(const BoundColumnRef &expr, DataChunk &input,
                                           Vector &result, idx_t count) {
    auto &src = input.GetVector(expr.column_index);
    auto physical = src.GetType().GetInternalType();
    idx_t type_size = GetTypeIdSize(physical);

    if (physical == PhysicalType::VARCHAR) {
        // Vectorized memcpy of string_t entries (16 bytes each) plus aux
        // ptr share so the destination vector keeps the source's string
        // heap alive. Falls back to per-row SetValue only when the source
        // has no auxiliary buffer (constructed-by-Value path). Drops the
        // per-row Value-boxing cost that dominated wall time on
        // filtered SELECT VARCHAR ORDER BY — 75 s -> sub-second on a
        // 13 M-row scan.
        std::memcpy(result.GetData(), src.GetData(), count * sizeof(string_t));
        if (auto aux = src.GetAuxiliaryPtr()) {
            result.SetAuxiliaryPtr(std::move(aux));
        }
        if (!src.GetValidity().AllValid()) {
            for (idx_t i = 0; i < count; i++) {
                if (!src.GetValidity().RowIsValid(i)) {
                    result.GetValidity().SetInvalid(i);
                }
            }
        }
    } else if (type_size > 0) {
        std::memcpy(result.GetData(), src.GetData(), count * type_size);
        if (!src.GetValidity().AllValid()) {
            for (idx_t i = 0; i < count; i++) {
                if (!src.GetValidity().RowIsValid(i)) {
                    result.GetValidity().SetInvalid(i);
                }
            }
        }
    }
}

void ExpressionExecutor::ExecuteConstant(const BoundConstant &expr, Vector &result,
                                          idx_t count) {
    for (idx_t i = 0; i < count; i++) {
        result.SetValue(i, expr.value);
    }
}

// Compare dispatch.
//
// Two important micro-optimizations:
//
//   1. The op string ("=", ">", etc.) is invariant for the whole call but
//      the loop used to compare it per row, costing 6+ string ops per
//      element. Hoist op selection outside via an enum + switch over
//      function-pointer, so each row is one branch + one typed compare.
//
//   2. When both inputs are all-valid (the common case for column data
//      with no nulls), skip the per-row validity check entirely. The
//      compiler then auto-vectorises the comparison loop.
//
// On the 10M-row Parquet `WHERE quantity > 50` benchmark this dropped
// CompareTyped<int64_t> from being the dominant cost (~600 ms) to
// effectively negligible.
template <typename T>
void ExpressionExecutor::CompareTyped(const std::string &op, Vector &left, Vector &right,
                                       Vector &result, idx_t count) {
    auto *ldata = left.GetData<T>();
    auto *rdata = right.GetData<T>();
    auto *out = result.GetData<bool>();

    enum class CmpOp : uint8_t { EQ, NE, LT, GT, LE, GE, UNKNOWN };
    CmpOp cop = CmpOp::UNKNOWN;
    if      (op == "=")  cop = CmpOp::EQ;
    else if (op == "!=" || op == "<>") cop = CmpOp::NE;
    else if (op == "<")  cop = CmpOp::LT;
    else if (op == ">")  cop = CmpOp::GT;
    else if (op == "<=") cop = CmpOp::LE;
    else if (op == ">=") cop = CmpOp::GE;

    auto &lvalid = left.GetValidity();
    auto &rvalid = right.GetValidity();
    auto &ovalid = result.GetValidity();
    bool both_all_valid = lvalid.AllValid() && rvalid.AllValid();

    auto run = [&](auto cmp) {
        if (both_all_valid) {
            for (idx_t i = 0; i < count; i++) out[i] = cmp(ldata[i], rdata[i]);
        } else {
            for (idx_t i = 0; i < count; i++) {
                if (!lvalid.RowIsValid(i) || !rvalid.RowIsValid(i)) {
                    ovalid.SetInvalid(i);
                    out[i] = false;
                } else {
                    out[i] = cmp(ldata[i], rdata[i]);
                }
            }
        }
    };

    switch (cop) {
    case CmpOp::EQ: run([](T a, T b) { return a == b; }); break;
    case CmpOp::NE: run([](T a, T b) { return a != b; }); break;
    case CmpOp::LT: run([](T a, T b) { return a <  b; }); break;
    case CmpOp::GT: run([](T a, T b) { return a >  b; }); break;
    case CmpOp::LE: run([](T a, T b) { return a <= b; }); break;
    case CmpOp::GE: run([](T a, T b) { return a >= b; }); break;
    default:
        for (idx_t i = 0; i < count; i++) out[i] = false;
        break;
    }
}

void ExpressionExecutor::ExecuteComparison(const BoundComparison &expr, DataChunk &input,
                                            Vector &result, idx_t count) {
    Vector left(expr.left->GetReturnType(), count);
    Vector right(expr.right->GetReturnType(), count);
    Execute(*expr.left, input, left, count);
    Execute(*expr.right, input, right, count);

    // SQLNULL-typed operand short-circuit. Any normal comparison op with
    // a literal NULL on either side is NULL per SQL three-valued logic.
    // Without this branch, ExecuteComparison fell through to the typed
    // dispatch switch and threw "NotImplemented: Comparison for type NULL"
    // because PhysicalType::INVALID isn't in the switch.
    //
    // Order matters: this runs BEFORE the IS DISTINCT FROM branch below
    // would be wrong (IS DISTINCT FROM has null-safe semantics — handled
    // there). LIKE / ILIKE with NULL pattern is handled in its own block.
    bool left_is_sqlnull = (expr.left->GetReturnType().id() == LogicalTypeId::SQLNULL);
    bool right_is_sqlnull = (expr.right->GetReturnType().id() == LogicalTypeId::SQLNULL);
    if ((left_is_sqlnull || right_is_sqlnull) &&
        expr.op != "IS DISTINCT FROM" && expr.op != "IS NOT DISTINCT FROM") {
        auto *out = result.GetData<bool>();
        auto &ovalid = result.GetValidity();
        for (idx_t i = 0; i < count; i++) {
            ovalid.SetInvalid(i);
            out[i] = false;
        }
        return;
    }

    // IS [NOT] DISTINCT FROM — null-safe (in)equality. Result is never NULL:
    //   NULL IS NOT DISTINCT FROM NULL -> true
    //   NULL IS DISTINCT FROM 5         -> true
    //   5 IS NOT DISTINCT FROM 5        -> true
    if (expr.op == "IS DISTINCT FROM" || expr.op == "IS NOT DISTINCT FROM") {
        bool not_distinct = (expr.op == "IS NOT DISTINCT FROM");
        auto *out = result.GetData<bool>();
        auto &lvalid = left.GetValidity();
        auto &rvalid = right.GetValidity();
        auto &ovalid = result.GetValidity();
        for (idx_t i = 0; i < count; i++) {
            ovalid.SetValid(i);
            bool l_null = !lvalid.RowIsValid(i);
            bool r_null = !rvalid.RowIsValid(i);
            bool eq;
            if (l_null && r_null) {
                eq = true;
            } else if (l_null || r_null) {
                eq = false;
            } else {
                eq = (left.GetValue(i) == right.GetValue(i));
            }
            out[i] = not_distinct ? eq : !eq;
        }
        return;
    }

    // Handle LIKE / ILIKE / NOT LIKE / NOT ILIKE specially.
    if (expr.op == "LIKE" || expr.op == "ILIKE" ||
        expr.op == "NOT LIKE" || expr.op == "NOT ILIKE") {
        bool negate = (expr.op == "NOT LIKE" || expr.op == "NOT ILIKE");
        bool case_insensitive = (expr.op == "ILIKE" || expr.op == "NOT ILIKE");
        auto *out = result.GetData<bool>();
        // Constant-pattern hoist: when the RHS is a literal pattern, the
        // existing per-row path allocates a fresh std::string for the pattern
        // on every row. Lift that allocation out of the loop. For shape
        // '%literal%' (case-sensitive, no _, no escapes) skip the backtracker
        // entirely and call std::search on string_t::GetData() directly --
        // no GetString() heap alloc on the haystack either.
        bool pat_is_constant =
            (expr.right->GetExpressionType() == BoundExpressionType::CONSTANT);
        if (pat_is_constant && count > 0 && right.GetValidity().RowIsValid(0)) {
            std::string pattern = right.GetData<string_t>()[0].GetString();
            if (case_insensitive) {
                for (auto &c : pattern) c = static_cast<char>(std::tolower(c));
            }
            bool contains_shape = false;
            std::string needle;
            if (!case_insensitive && pattern.size() >= 2 &&
                pattern.front() == '%' && pattern.back() == '%') {
                needle = pattern.substr(1, pattern.size() - 2);
                bool ok = true;
                for (char c : needle) {
                    if (c == '%' || c == '_' || c == '\\') { ok = false; break; }
                }
                contains_shape = ok;
            }
            if (contains_shape) {
                for (idx_t i = 0; i < count; i++) {
                    if (!left.GetValidity().RowIsValid(i)) {
                        result.GetValidity().SetInvalid(i);
                        out[i] = false;
                        continue;
                    }
                    const auto &s = left.GetData<string_t>()[i];
                    const char *hs = s.GetData();
                    uint32_t hl = s.GetSize();
                    bool m;
                    if (needle.empty()) {
                        m = true;
                    } else if (hl < needle.size()) {
                        m = false;
                    } else {
                        auto end = hs + hl;
                        m = (std::search(hs, end, needle.begin(), needle.end()) != end);
                    }
                    out[i] = negate ? !m : m;
                }
                return;
            }
            for (idx_t i = 0; i < count; i++) {
                if (!left.GetValidity().RowIsValid(i)) {
                    result.GetValidity().SetInvalid(i);
                    out[i] = false;
                } else {
                    auto str = left.GetData<string_t>()[i].GetString();
                    if (case_insensitive) {
                        for (auto &c : str) c = static_cast<char>(std::tolower(c));
                    }
                    bool m = LikeMatch(str, pattern);
                    out[i] = negate ? !m : m;
                }
            }
            return;
        }
        for (idx_t i = 0; i < count; i++) {
            if (!left.GetValidity().RowIsValid(i) || !right.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
            } else {
                auto str = left.GetData<string_t>()[i].GetString();
                auto pattern = right.GetData<string_t>()[i].GetString();
                if (case_insensitive) {
                    for (auto &c : str) c = static_cast<char>(std::tolower(c));
                    for (auto &c : pattern) c = static_cast<char>(std::tolower(c));
                }
                bool m = LikeMatch(str, pattern);
                out[i] = negate ? !m : m;
            }
        }
        return;
    }

    // DATE vs TIMESTAMP mixed compare: convert the DATE side to
    // micros-since-epoch (days * 86400 * 1e6) so the int64 compare
    // operates on the same scale. Previously the mixed-type branch
    // fell through to stod(ToString()) which silently truncated and
    // returned wrong answers — e.g. DATE '2024-01-01' < TIMESTAMP
    // '2024-06-15 12:00:00' returned false.
    {
        auto lid = left.GetType().id();
        auto rid = right.GetType().id();
        bool l_date = (lid == LogicalTypeId::DATE);
        bool r_date = (rid == LogicalTypeId::DATE);
        bool l_ts = (lid == LogicalTypeId::TIMESTAMP || lid == LogicalTypeId::TIMESTAMP_TZ);
        bool r_ts = (rid == LogicalTypeId::TIMESTAMP || rid == LogicalTypeId::TIMESTAMP_TZ);
        bool l_varchar = (lid == LogicalTypeId::VARCHAR);
        bool r_varchar = (rid == LogicalTypeId::VARCHAR);
        if ((l_date && r_ts) || (l_ts && r_date)) {
            // Promote the DATE side to TIMESTAMP-micros in a fresh
            // Vector so the typed int64 path below handles it.
            Vector promoted(LogicalType::TIMESTAMP(), count);
            Vector *date_vec = l_date ? &left : &right;
            auto *src = date_vec->GetData<int32_t>();
            auto *dst = promoted.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) {
                if (date_vec->GetValidity().RowIsValid(i)) {
                    dst[i] = static_cast<int64_t>(src[i]) * 86400LL * 1000000LL;
                } else {
                    promoted.GetValidity().SetInvalid(i);
                    dst[i] = 0;
                }
            }
            if (l_date) {
                left = std::move(promoted);
            } else {
                right = std::move(promoted);
            }
        }
        // DATE/TIMESTAMP vs VARCHAR: parse the VARCHAR side as DATE or
        // TIMESTAMP so the typed compare operates on matched units.
        // Previously the mixed-type branch ran stod(varchar) which
        // truncated 'YYYY-MM-DD ...' to the leading year — silently
        // wrong filtering across every WHERE clause with a date-string
        // literal.
        else if ((l_ts && r_varchar) || (r_ts && l_varchar)) {
            Vector *str_vec = l_varchar ? &left : &right;
            Vector promoted(LogicalType::TIMESTAMP(), count);
            auto *dst = promoted.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) {
                if (!str_vec->GetValidity().RowIsValid(i)) {
                    promoted.GetValidity().SetInvalid(i);
                    dst[i] = 0;
                    continue;
                }
                auto s = str_vec->GetValue(i).GetValue<std::string>();
                int64_t micros;
                int32_t days;
                if (Value::TryParseTimestampMicros(s.data(), s.size(), micros)) {
                    dst[i] = micros;
                } else if (Value::TryParseDateStringEpochDays(s.data(), s.size(), days)) {
                    dst[i] = static_cast<int64_t>(days) * 86400LL * 1000000LL;
                } else {
                    promoted.GetValidity().SetInvalid(i);
                    dst[i] = 0;
                }
            }
            if (l_varchar) left = std::move(promoted);
            else right = std::move(promoted);
        }
        else if ((l_date && r_varchar) || (r_date && l_varchar)) {
            Vector *str_vec = l_varchar ? &left : &right;
            Vector *date_vec = l_varchar ? &right : &left;
            // Promote DATE side to its int32 days unchanged via copy,
            // and parse the string into DATE days. Both go via int32.
            Vector promoted_str(LogicalType::DATE(), count);
            auto *dst = promoted_str.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) {
                if (!str_vec->GetValidity().RowIsValid(i)) {
                    promoted_str.GetValidity().SetInvalid(i);
                    dst[i] = 0;
                    continue;
                }
                auto s = str_vec->GetValue(i).GetValue<std::string>();
                int32_t days;
                if (Value::TryParseDateStringEpochDays(s.data(), s.size(), days)) {
                    dst[i] = days;
                } else {
                    promoted_str.GetValidity().SetInvalid(i);
                    dst[i] = 0;
                }
            }
            if (l_varchar) left = std::move(promoted_str);
            else right = std::move(promoted_str);
            (void)date_vec;
        }
    }

    auto left_phys = left.GetType().GetInternalType();
    auto right_phys = right.GetType().GetInternalType();

    // If types match, use typed comparison for speed.
    if (left_phys == right_phys) {
        switch (left_phys) {
        case PhysicalType::BOOL:
            CompareTyped<bool>(expr.op, left, right, result, count); break;
        case PhysicalType::INT8:
            CompareTyped<int8_t>(expr.op, left, right, result, count); break;
        case PhysicalType::INT16:
            CompareTyped<int16_t>(expr.op, left, right, result, count); break;
        case PhysicalType::INT32:
            CompareTyped<int32_t>(expr.op, left, right, result, count); break;
        case PhysicalType::INT64:
            CompareTyped<int64_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT8:
            CompareTyped<uint8_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT16:
            CompareTyped<uint16_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT32:
            CompareTyped<uint32_t>(expr.op, left, right, result, count); break;
        case PhysicalType::UINT64:
            CompareTyped<uint64_t>(expr.op, left, right, result, count); break;
        case PhysicalType::FLOAT:
            CompareTyped<float>(expr.op, left, right, result, count); break;
        case PhysicalType::DOUBLE:
            CompareTyped<double>(expr.op, left, right, result, count); break;
        case PhysicalType::VARCHAR:
            CompareTyped<string_t>(expr.op, left, right, result, count); break;
        default:
            throw NotImplementedException("Comparison for type " + left.GetType().ToString());
        }
    } else {
        // Mixed types: numeric coercion via stod. Non-numeric VARCHAR
        // operands previously leaked std::invalid_argument /
        // std::out_of_range from stod up the stack — every WHERE clause
        // mixing a VARCHAR column with an integer literal crashed when
        // any row's string wasn't numeric. Now: catch and raise
        // ConversionException with a clear message; SQL-92 says
        // unparseable values should be a typed error, not a process
        // crash.
        auto *out = result.GetData<bool>();
        for (idx_t i = 0; i < count; i++) {
            auto lv = left.GetValue(i), rv = right.GetValue(i);
            if (lv.IsNull() || rv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
                continue;
            }
            double ld = 0.0, rd = 0.0;
            try {
                ld = std::stod(lv.ToString());
                rd = std::stod(rv.ToString());
            } catch (const std::exception &) {
                throw ConversionException(
                    "Cannot compare values '" + lv.ToString() + "' and '" +
                    rv.ToString() + "' as numeric");
            }
            if (expr.op == "=") out[i] = ld == rd;
            else if (expr.op == "!=" || expr.op == "<>") out[i] = ld != rd;
            else if (expr.op == "<") out[i] = ld < rd;
            else if (expr.op == ">") out[i] = ld > rd;
            else if (expr.op == "<=") out[i] = ld <= rd;
            else if (expr.op == ">=") out[i] = ld >= rd;
            else out[i] = false;
        }
    }
}

void ExpressionExecutor::ExecuteConjunction(const BoundConjunction &expr, DataChunk &input,
                                             Vector &result, idx_t count) {
    Vector left(LogicalType::BOOLEAN(), count);
    Vector right(LogicalType::BOOLEAN(), count);
    Execute(*expr.left, input, left, count);
    Execute(*expr.right, input, right, count);

    auto *ldata = left.GetData<bool>();
    auto *rdata = right.GetData<bool>();
    auto *out = result.GetData<bool>();
    auto &lvalid = left.GetValidity();
    auto &rvalid = right.GetValidity();
    auto &ovalid = result.GetValidity();

    // SQL-92 Kleene three-valued logic:
    //   AND: FALSE wins over NULL  (FALSE AND NULL = FALSE)
    //        TRUE AND NULL = NULL, NULL AND NULL = NULL
    //   OR : TRUE wins over NULL   (TRUE OR NULL = TRUE)
    //        FALSE OR NULL = NULL, NULL OR NULL = NULL
    // The previous code read ldata/rdata without consulting validity,
    // so `NULL AND TRUE` (left invalid, right=true) computed `0 && 1`
    // and returned FALSE — a silent wrong-result bug in every WHERE
    // clause that mixed NULL-producing predicates with TRUE/FALSE.
    bool is_and = (expr.op == "AND");
    for (idx_t i = 0; i < count; i++) {
        bool l_valid = lvalid.RowIsValid(i);
        bool r_valid = rvalid.RowIsValid(i);
        bool l = l_valid && ldata[i];
        bool r = r_valid && rdata[i];

        if (is_and) {
            // FALSE on either side wins regardless of the other's validity.
            if ((l_valid && !l) || (r_valid && !r)) {
                ovalid.SetValid(i);
                out[i] = false;
            } else if (!l_valid || !r_valid) {
                ovalid.SetInvalid(i);
                out[i] = false;
            } else {
                ovalid.SetValid(i);
                out[i] = l && r;
            }
        } else {  // OR
            // TRUE on either side wins regardless of the other's validity.
            if ((l_valid && l) || (r_valid && r)) {
                ovalid.SetValid(i);
                out[i] = true;
            } else if (!l_valid || !r_valid) {
                ovalid.SetInvalid(i);
                out[i] = false;
            } else {
                ovalid.SetValid(i);
                out[i] = l || r;
            }
        }
    }
}

template <typename T>
void ExpressionExecutor::ArithmeticTyped(const std::string &op, Vector &left, Vector &right,
                                          Vector &result, idx_t count) {
    auto *ldata = left.GetData<T>();
    auto *rdata = right.GetData<T>();
    auto *out = result.GetData<T>();

    for (idx_t i = 0; i < count; i++) {
        if (!left.GetValidity().RowIsValid(i) || !right.GetValidity().RowIsValid(i)) {
            result.GetValidity().SetInvalid(i);
            out[i] = T{};
            continue;
        }
        if constexpr (std::is_integral_v<T>) {
            // Signed integer overflow is undefined behaviour in C++ and
            // silently wraps on x86 — `2147483647 + 1` produced
            // `-2147483648` even though the user got no error. SQL
            // standard says overflow is a runtime error; we return NULL
            // to match the divide-by-zero / modulo-by-zero behaviour
            // already established by the +/-/* paths.
            T a = ldata[i], b = rdata[i];
            T r{};
            bool overflow = false;
            if (op == "+") {
                if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
                    (b < 0 && a < std::numeric_limits<T>::min() - b)) {
                    overflow = true;
                } else r = a + b;
            } else if (op == "-") {
                if ((b < 0 && a > std::numeric_limits<T>::max() + b) ||
                    (b > 0 && a < std::numeric_limits<T>::min() + b)) {
                    overflow = true;
                } else r = a - b;
            } else if (op == "*") {
                if (a != 0 && b != 0) {
                    T m = a * b;
                    // Detect overflow via re-division check (portable;
                    // MSVC has no __builtin_mul_overflow).
                    if (m / a != b) overflow = true;
                    else r = m;
                } // else r = 0 (already)
            } else if (op == "/") {
                if (b == 0) {
                    overflow = true;  // -> NULL
                } else if (a == std::numeric_limits<T>::min() && b == -1) {
                    overflow = true;  // INT_MIN / -1 overflows
                } else {
                    r = a / b;
                }
            } else {
                r = T{};
            }
            if (overflow) {
                result.GetValidity().SetInvalid(i);
                out[i] = T{};
            } else {
                out[i] = r;
            }
        } else {
            // Float / double: IEEE-754 inf/nan is the standard behaviour
            // for overflow; SQL queries expect that, so don't NULL it.
            if (op == "+")       out[i] = ldata[i] + rdata[i];
            else if (op == "-")  out[i] = ldata[i] - rdata[i];
            else if (op == "*")  out[i] = ldata[i] * rdata[i];
            else if (op == "/") {
                if (rdata[i] == T{}) {
                    result.GetValidity().SetInvalid(i);
                    out[i] = T{};
                } else {
                    out[i] = ldata[i] / rdata[i];
                }
            }
            else out[i] = T{};
        }
    }
}

// Cast a numeric Vector to `target_type` if its current type doesn't match.
// Returns the original Vector by-move if no cast is needed; otherwise builds
// a new Vector of target_type with converted values. Falls back to Value
// boxing for uncommon conversions; the typed branches are inlined for the
// hot int32/int64 -> double path that hits whenever a numeric literal is
// mixed with a DOUBLE-typed aggregate (e.g. AVG(x) + 1, AVG(x)/COUNT(*)).
static Vector CoerceVector(Vector &&src, const LogicalType &target_type, idx_t count) {
    if (src.GetType().id() == target_type.id()) return std::move(src);
    Vector out(target_type, count);
    auto src_id = src.GetType().id();
    auto dst_id = target_type.id();

    auto copy_validity = [&]() {
        for (idx_t i = 0; i < count; i++) {
            if (!src.GetValidity().RowIsValid(i)) out.GetValidity().SetInvalid(i);
        }
    };

    if (dst_id == LogicalTypeId::DOUBLE) {
        auto *o = out.GetData<double>();
        if (src_id == LogicalTypeId::INTEGER) {
            auto *s = src.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::BIGINT) {
            auto *s = src.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::FLOAT) {
            auto *s = src.GetData<float>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
    } else if (dst_id == LogicalTypeId::INTEGER) {
        // ResolveArithmeticType promotes TINYINT+TINYINT and SMALLINT+
        // SMALLINT to INTEGER. Without these typed widenings, the
        // fallback per-row SetValue wrote a TINYINT-typed Value into
        // an INT32 buffer slot which read back as garbage (silent
        // wrong-result on every TINYINT/SMALLINT arithmetic).
        auto *o = out.GetData<int32_t>();
        if (src_id == LogicalTypeId::TINYINT) {
            auto *s = src.GetData<int8_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int32_t>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::SMALLINT) {
            auto *s = src.GetData<int16_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int32_t>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::UTINYINT) {
            auto *s = src.GetData<uint8_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int32_t>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::USMALLINT) {
            auto *s = src.GetData<uint16_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int32_t>(s[i]);
            copy_validity(); return out;
        }
    } else if (dst_id == LogicalTypeId::BIGINT) {
        auto *o = out.GetData<int64_t>();
        if (src_id == LogicalTypeId::INTEGER) {
            auto *s = src.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int64_t>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::TINYINT) {
            auto *s = src.GetData<int8_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int64_t>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::SMALLINT) {
            auto *s = src.GetData<int16_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<int64_t>(s[i]);
            copy_validity(); return out;
        }
    } else if (dst_id == LogicalTypeId::FLOAT) {
        auto *o = out.GetData<float>();
        if (src_id == LogicalTypeId::INTEGER) {
            auto *s = src.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<float>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::BIGINT) {
            auto *s = src.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<float>(s[i]);
            copy_validity(); return out;
        }
    } else if (dst_id == LogicalTypeId::DOUBLE) {
        auto *o = out.GetData<double>();
        if (src_id == LogicalTypeId::TINYINT) {
            auto *s = src.GetData<int8_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
        if (src_id == LogicalTypeId::SMALLINT) {
            auto *s = src.GetData<int16_t>();
            for (idx_t i = 0; i < count; i++) o[i] = static_cast<double>(s[i]);
            copy_validity(); return out;
        }
    }
    // Fallback: per-row boxed conversion.
    for (idx_t i = 0; i < count; i++) {
        if (!src.GetValidity().RowIsValid(i)) {
            out.GetValidity().SetInvalid(i); continue;
        }
        out.SetValue(i, src.GetValue(i));
    }
    return out;
}

void ExpressionExecutor::ExecuteArithmetic(const BoundArithmetic &expr, DataChunk &input,
                                            Vector &result, idx_t count) {
    Vector left(expr.left->GetReturnType(), count);
    Vector right(expr.right->GetReturnType(), count);
    Execute(*expr.left, input, left, count);
    Execute(*expr.right, input, right, count);

    // TIMESTAMP arithmetic per SQL standard / DuckDB convention:
    //   TIMESTAMP + N (seconds) -> TIMESTAMP   (commutative)
    //   TIMESTAMP - N (seconds) -> TIMESTAMP
    //   TIMESTAMP - TIMESTAMP   -> BIGINT seconds between
    // TIMESTAMP is internally INT64 micros; integer-seconds operand
    // must be scaled by 1e6 before the int64 math. TIMESTAMP-TIMESTAMP
    // produces a raw micros diff that we then truncate to seconds.
    auto lt_id = expr.left->GetReturnType().id();
    auto rt_id = expr.right->GetReturnType().id();
    bool left_ts  = (lt_id == LogicalTypeId::TIMESTAMP || lt_id == LogicalTypeId::TIMESTAMP_TZ);
    bool right_ts = (rt_id == LogicalTypeId::TIMESTAMP || rt_id == LogicalTypeId::TIMESTAMP_TZ);
    auto is_int_id = [](LogicalTypeId t) {
        return t == LogicalTypeId::TINYINT  || t == LogicalTypeId::SMALLINT ||
               t == LogicalTypeId::INTEGER  || t == LogicalTypeId::BIGINT;
    };
    if (left_ts || right_ts) {
        auto &ov = result.GetValidity();
        auto *out = result.GetData<int64_t>();
        if (expr.op == "-" && left_ts && right_ts) {
            // TIMESTAMP - TIMESTAMP -> BIGINT seconds.
            for (idx_t i = 0; i < count; i++) {
                if (!left.GetValidity().RowIsValid(i) || !right.GetValidity().RowIsValid(i)) {
                    ov.SetInvalid(i); out[i] = 0; continue;
                }
                int64_t lm = left.GetValue(i).GetValue<int64_t>();
                int64_t rm = right.GetValue(i).GetValue<int64_t>();
                out[i] = (lm - rm) / 1000000LL;
            }
            return;
        }
        // TIMESTAMP ± integer-seconds. Identify which side is the TS.
        bool int_on_right = left_ts && (is_int_id(rt_id) || rt_id == LogicalTypeId::SQLNULL);
        bool int_on_left  = right_ts && (is_int_id(lt_id) || lt_id == LogicalTypeId::SQLNULL);
        for (idx_t i = 0; i < count; i++) {
            if (!left.GetValidity().RowIsValid(i) || !right.GetValidity().RowIsValid(i)) {
                ov.SetInvalid(i); out[i] = 0; continue;
            }
            int64_t ts_micros = int_on_right
                ? left.GetValue(i).GetValue<int64_t>()
                : (int_on_left ? right.GetValue(i).GetValue<int64_t>() : 0);
            int64_t secs = 0;
            if (int_on_right) {
                auto v = right.GetValue(i);
                secs = (v.type().id() == LogicalTypeId::INTEGER)
                    ? v.GetValue<int32_t>() : v.GetValue<int64_t>();
            } else if (int_on_left) {
                auto v = left.GetValue(i);
                secs = (v.type().id() == LogicalTypeId::INTEGER)
                    ? v.GetValue<int32_t>() : v.GetValue<int64_t>();
            }
            int64_t add_micros = secs * 1000000LL;
            if (expr.op == "+") {
                out[i] = ts_micros + add_micros;
            } else {  // "-"
                // only left_ts && int-on-right reaches here in the
                // binder; defensively still subtract.
                out[i] = ts_micros - add_micros;
            }
        }
        return;
    }

    // Promote operands to result type so ArithmeticTyped<T> below reads the
    // right physical layout. Without this, mixing AVG (DOUBLE) with an
    // INTEGER literal turns into a reinterpret-cast on raw int bytes.
    //
    // Modulo used to skip this coercion. That caused
    //   SELECT null % null
    // to dereference a SQLNULL-typed buffer as int32, segfaulting on
    // operator.slt L268 and any user query with NULL % NULL. Including
    // modulo in the coercion makes the operand layout consistent with
    // +/-/*//; the existing modulo branch then reads the correct types.
    // String concatenation (||) still skips because it converts via
    // Value::ToString and doesn't need typed-buffer layout.
    if (expr.op != "||") {
        left = CoerceVector(std::move(left), result.GetType(), count);
        right = CoerceVector(std::move(right), result.GetType(), count);
    }

    // Handle % modulo for integers. NULL propagates: if either operand
    // is invalid (NULL), the result row is NULL. The pre-fix code wrote
    // garbage into out[i] for those rows; now they're explicitly invalid.
    if (expr.op == "%") {
        auto physical = result.GetType().GetInternalType();
        auto &lv = left.GetValidity();
        auto &rv = right.GetValidity();
        auto &ov = result.GetValidity();
        bool lvalid_all = lv.AllValid();
        bool rvalid_all = rv.AllValid();
        if (physical == PhysicalType::INT32) {
            auto *ld = left.GetData<int32_t>(), *rd = right.GetData<int32_t>();
            auto *out = result.GetData<int32_t>();
            for (idx_t i = 0; i < count; i++) {
                bool li = lvalid_all || lv.RowIsValid(i);
                bool ri = rvalid_all || rv.RowIsValid(i);
                if (!li || !ri || rd[i] == 0) { ov.SetInvalid(i); out[i] = 0; }
                else out[i] = ld[i] % rd[i];
            }
        } else if (physical == PhysicalType::INT64) {
            auto *ld = left.GetData<int64_t>(), *rd = right.GetData<int64_t>();
            auto *out = result.GetData<int64_t>();
            for (idx_t i = 0; i < count; i++) {
                bool li = lvalid_all || lv.RowIsValid(i);
                bool ri = rvalid_all || rv.RowIsValid(i);
                if (!li || !ri || rd[i] == 0) { ov.SetInvalid(i); out[i] = 0; }
                else out[i] = ld[i] % rd[i];
            }
        } else {
            // Float modulo via fmod.
            auto *ld = left.GetData<double>(), *rd = right.GetData<double>();
            auto *out = result.GetData<double>();
            for (idx_t i = 0; i < count; i++) {
                bool li = lvalid_all || lv.RowIsValid(i);
                bool ri = rvalid_all || rv.RowIsValid(i);
                if (!li || !ri || rd[i] == 0.0) { ov.SetInvalid(i); out[i] = 0.0; }
                else out[i] = std::fmod(ld[i], rd[i]);
            }
        }
        return;
    }

    // Handle || for string concatenation.
    if (expr.op == "||") {
        for (idx_t i = 0; i < count; i++) {
            auto lv = left.GetValue(i), rv = right.GetValue(i);
            if (lv.IsNull() || rv.IsNull()) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.SetValue(i, Value::VARCHAR(lv.ToString() + rv.ToString()));
            }
        }
        return;
    }

    auto physical = result.GetType().GetInternalType();
    switch (physical) {
    case PhysicalType::INT32:
        ArithmeticTyped<int32_t>(expr.op, left, right, result, count); break;
    case PhysicalType::INT64:
        ArithmeticTyped<int64_t>(expr.op, left, right, result, count); break;
    case PhysicalType::FLOAT:
        ArithmeticTyped<float>(expr.op, left, right, result, count); break;
    case PhysicalType::DOUBLE:
        ArithmeticTyped<double>(expr.op, left, right, result, count); break;
    default:
        throw NotImplementedException("Arithmetic for type " + result.GetType().ToString());
    }
}

void ExpressionExecutor::ExecuteNegation(const BoundNegation &expr, DataChunk &input,
                                          Vector &result, idx_t count) {
    Vector child(LogicalType::BOOLEAN(), count);
    Execute(*expr.child, input, child, count);
    auto *cdata = child.GetData<bool>();
    auto *out = result.GetData<bool>();
    auto &cv = child.GetValidity();
    auto &ov = result.GetValidity();
    for (idx_t i = 0; i < count; i++) {
        // SQL Kleene logic: NOT UNKNOWN = UNKNOWN. Propagate the
        // child's invalid bit so NOT (x IN (1, 2, NULL)) returns
        // NULL/UNKNOWN when x doesn't equal any concrete element,
        // matching SQL three-valued logic.
        if (!cv.RowIsValid(i)) {
            ov.SetInvalid(i);
            out[i] = false;
        } else {
            out[i] = !cdata[i];
        }
    }
}

void ExpressionExecutor::ExecuteIsNull(const BoundIsNull &expr, DataChunk &input,
                                        Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);
    auto *out = result.GetData<bool>();
    for (idx_t i = 0; i < count; i++) {
        bool is_null = !child.GetValidity().RowIsValid(i);
        out[i] = expr.is_not ? !is_null : is_null;
    }
}

// SQL-92 three-valued logic predicates: x IS [NOT] {TRUE | FALSE | UNKNOWN}.
// Result is always BOOLEAN, never NULL. The child may be NULL — that's
// the whole point of the predicate. UNKNOWN is the SQL spelling for
// "the boolean expression evaluated to NULL", i.e. equivalent to IS NULL
// applied to a boolean operand.
void ExpressionExecutor::ExecuteIsBool(const BoundIsBool &expr, DataChunk &input,
                                        Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);
    auto *out = result.GetData<bool>();
    auto &validity = child.GetValidity();
    bool is_null_child_type = (expr.child->GetReturnType().id() == LogicalTypeId::SQLNULL);
    auto pred = expr.pred;
    bool negate = expr.is_not;

    for (idx_t i = 0; i < count; i++) {
        bool valid = !is_null_child_type && validity.RowIsValid(i);
        bool match;
        if (pred == BoundIsBool::Predicate::UNKNOWN_) {
            match = !valid;
        } else if (!valid) {
            // NULL IS TRUE/FALSE — both false (NULL is neither TRUE nor FALSE).
            match = false;
        } else {
            bool b = child.GetData<bool>()[i];
            match = (pred == BoundIsBool::Predicate::TRUE_) ? b : !b;
        }
        out[i] = negate ? !match : match;
        // Result is BOOLEAN, never NULL — IS-predicates close the
        // three-valued logic to two-valued.
    }
}

void ExpressionExecutor::ExecuteUnaryMinus(const BoundUnaryMinus &expr, DataChunk &input,
                                            Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);

    // Propagate NULL: -NULL is NULL in SQL. Previously the per-element
    // loop unconditionally negated the underlying byte (so -CAST(NULL AS
    // DOUBLE) printed as "-0" instead of NULL). Set the result's invalid
    // bits up-front from the child's; the typed loop still runs but the
    // garbage value is masked.
    auto &cvalid = child.GetValidity();
    auto &ovalid = result.GetValidity();
    auto physical = result.GetType().GetInternalType();
    switch (physical) {
    case PhysicalType::INT16: {
        auto *src = child.GetData<int16_t>();
        auto *dst = result.GetData<int16_t>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0; }
            else dst[i] = static_cast<int16_t>(-src[i]);
        }
        break;
    }
    case PhysicalType::INT32: {
        auto *src = child.GetData<int32_t>();
        auto *dst = result.GetData<int32_t>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0; }
            else dst[i] = -src[i];
        }
        break;
    }
    case PhysicalType::INT64: {
        auto *src = child.GetData<int64_t>();
        auto *dst = result.GetData<int64_t>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0; }
            else dst[i] = -src[i];
        }
        break;
    }
    case PhysicalType::FLOAT: {
        auto *src = child.GetData<float>();
        auto *dst = result.GetData<float>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0.0f; }
            else dst[i] = -src[i];
        }
        break;
    }
    case PhysicalType::DOUBLE: {
        auto *src = child.GetData<double>();
        auto *dst = result.GetData<double>();
        for (idx_t i = 0; i < count; i++) {
            if (!cvalid.RowIsValid(i)) { ovalid.SetInvalid(i); dst[i] = 0.0; }
            else dst[i] = -src[i];
        }
        break;
    }
    default:
        throw NotImplementedException("Unary minus for type " +
                                       result.GetType().ToString());
    }
}

// ============================================================================
// Function execution (CASE, IN, scalar functions)
// ============================================================================

static bool LikeMatch(const std::string &str, const std::string &pattern) {
    // SQL LIKE pattern matching:
    //   %       any sequence of zero or more characters
    //   _       any single character
    //   \%, \_  literal % or _ (backslash escape)
    //   \\      literal backslash
    // ESCAPE clause is desugared at parse time into backslash escapes,
    // so the executor only needs to recognise the standard `\` form.
    size_t si = 0, pi = 0;
    size_t star_p = std::string::npos, star_s = 0;
    auto pattern_char_at = [&](size_t i, char &out, bool &is_meta) -> size_t {
        // Returns the number of pattern chars consumed (1 or 2). For an
        // escape sequence `\X`, returns 2 with `is_meta=false` and the
        // literal X. For a metachar (`%` or `_`), returns 1 with
        // `is_meta=true`. For a plain literal, returns 1 with
        // `is_meta=false`.
        if (i + 1 < pattern.size() && pattern[i] == '\\') {
            out = pattern[i + 1];
            is_meta = false;
            return 2;
        }
        out = pattern[i];
        is_meta = (out == '%' || out == '_');
        return 1;
    };
    while (si < str.size()) {
        if (pi < pattern.size()) {
            char pc;
            bool is_meta;
            size_t step = pattern_char_at(pi, pc, is_meta);
            if (is_meta && pc == '%') {
                star_p = pi;
                star_s = si;
                pi += step;
                continue;
            }
            if (is_meta && pc == '_') {
                si++;
                pi += step;
                continue;
            }
            // Literal (possibly via escape).
            if (pc == str[si]) {
                si++;
                pi += step;
                continue;
            }
        }
        if (star_p != std::string::npos) {
            // Backtrack: advance one in str, reset pi to the char after
            // the % (which is just star_p + 1 since `%` is always one
            // pattern char wide — backslash-escaped `%` can't be a star
            // anyway).
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }
    // Skip any trailing `%` in the pattern.
    while (pi < pattern.size()) {
        if (pattern[pi] == '%') {
            pi++;
        } else {
            break;
        }
    }
    return pi == pattern.size();
}

void ExpressionExecutor::ExecuteFunction(const BoundFunction &expr, DataChunk &input,
                                          Vector &result, idx_t count) {
    auto &name = expr.function_name;

    // CASE(when1, then1, when2, then2, ..., [else])
    // IF(c, t, f) and IIF(c, t, f) reach the executor with the same arg
    // layout (cond, then, else) so we route them through this path.
    //
    // Vectorized form: evaluate each branch's when/then over the FULL chunk
    // once, then select per-row. The earlier per-row implementation was
    // O(count^2) — it re-Executed when_vec for every row, so a 2048-row
    // chunk paid 2048× the cost of a single Execute. A CASE-heavy query timed
    // out >30s entirely in that loop. Now we Execute once per branch.
    if (name == "CASE" || name == "IF" || name == "IIF") {
        const size_t nargs = expr.arguments.size();
        const bool has_else = (nargs % 2 == 1);
        // Track which output rows are still unfilled (haven't matched a WHEN).
        std::vector<uint8_t> need(count, 1);
        idx_t remaining = count;
        for (size_t a = 0; a + 1 < nargs && remaining > 0; a += 2) {
            Vector when_vec(LogicalType::BOOLEAN(), count);
            Execute(*expr.arguments[a], input, when_vec, count);
            // Find rows that match THIS branch (when==true and not yet filled).
            std::vector<idx_t> hits;
            hits.reserve(remaining);
            const bool *wd = when_vec.GetData<bool>();
            const auto &wv = when_vec.GetValidity();
            for (idx_t i = 0; i < count; i++) {
                if (need[i] && wv.RowIsValid(i) && wd[i]) hits.push_back(i);
            }
            if (hits.empty()) continue;
            Vector then_vec(expr.GetReturnType(), count);
            Execute(*expr.arguments[a + 1], input, then_vec, count);
            for (idx_t i : hits) {
                result.SetValue(i, then_vec.GetValue(i));
                need[i] = 0;
            }
            remaining -= hits.size();
        }
        if (remaining > 0) {
            if (has_else) {
                Vector else_vec(expr.GetReturnType(), count);
                Execute(*expr.arguments.back(), input, else_vec, count);
                for (idx_t i = 0; i < count; i++) {
                    if (need[i]) result.SetValue(i, else_vec.GetValue(i));
                }
            } else {
                for (idx_t i = 0; i < count; i++) {
                    if (need[i]) result.GetValidity().SetInvalid(i);
                }
            }
        }
        return;
    }

    // BETWEEN(value, low, high)
    if (name == "BETWEEN") {
        auto *out = result.GetData<bool>();
        for (idx_t i = 0; i < count; i++) {
            Vector val_vec(expr.arguments[0]->GetReturnType(), count);
            Vector low_vec(expr.arguments[1]->GetReturnType(), count);
            Vector high_vec(expr.arguments[2]->GetReturnType(), count);
            Execute(*expr.arguments[0], input, val_vec, count);
            Execute(*expr.arguments[1], input, low_vec, count);
            Execute(*expr.arguments[2], input, high_vec, count);
            auto v = val_vec.GetValue(i);
            auto lo = low_vec.GetValue(i);
            auto hi = high_vec.GetValue(i);
            if (v.IsNull() || lo.IsNull() || hi.IsNull()) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
            } else {
                out[i] = !(v < lo) && !(hi < v);
            }
        }
        return;
    }

    // IN(value, list...)
    if (name == "IN") {
        // SQL three-valued logic for IN:
        //   x IN (a, b, ..., NULL)
        //     true   if x equals any non-null element
        //     NULL   if x is NULL, or any element is NULL and no
        //            non-null equality matched
        //     false  otherwise
        // The pre-fix code returned false for NULL-in-list cases,
        // which (when wrapped by NOT) produced wrong-result rows for
        // any query using NOT IN with a NULL value in the list.
        auto *out = result.GetData<bool>();
        // Pre-evaluate the LHS and list args once for the chunk; the
        // previous code re-ran Execute inside the per-row loop, which
        // was both slow and produced wrong validity for chained args.
        Vector val_vec(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, val_vec, count);
        std::vector<Vector> list_vecs;
        list_vecs.reserve(expr.arguments.size() - 1);
        for (size_t a = 1; a < expr.arguments.size(); a++) {
            list_vecs.emplace_back(expr.arguments[a]->GetReturnType(), count);
            Execute(*expr.arguments[a], input, list_vecs.back(), count);
        }
        for (idx_t i = 0; i < count; i++) {
            auto val = val_vec.GetValue(i);
            if (val.IsNull()) {
                // NULL IN anything is NULL.
                result.GetValidity().SetInvalid(i);
                out[i] = false;
                continue;
            }
            bool found = false;
            bool saw_null = false;
            for (size_t a = 0; a < list_vecs.size(); a++) {
                auto e = list_vecs[a].GetValue(i);
                if (e.IsNull()) {
                    saw_null = true;
                    continue;
                }
                if (val == e) {
                    found = true;
                    break;
                }
            }
            if (found) {
                out[i] = true;
            } else if (saw_null) {
                result.GetValidity().SetInvalid(i);
                out[i] = false;
            } else {
                out[i] = false;
            }
        }
        return;
    }

    // ---- Scalar string functions ----

    if (name == "LENGTH" || name == "STRLEN") {
        auto *out = result.GetData<int32_t>();
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
            } else {
                out[i] = static_cast<int32_t>(arg.GetData<string_t>()[i].GetSize());
            }
        }
        return;
    }

    // MD5(s) — RFC-1321 message digest. Returns 32 lowercase hex chars.
    // NULL input -> NULL.
    if (name == "MD5") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            result.SetValue(i, Value::VARCHAR(Md5Hex(v.GetValue<std::string>())));
        }
        return;
    }

    // SHA1 / SHA_1 — FIPS-180-4 message digest. Returns 40 lowercase
    // hex chars. NULL input -> NULL. Same NULL pattern as MD5/SHA256.
    if (name == "SHA1" || name == "SHA_1") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            result.SetValue(i, Value::VARCHAR(Sha1Hex(v.GetValue<std::string>())));
        }
        return;
    }

    // SHA256 / SHA2_256 — FIPS-180-4 message digest. Returns 64
    // lowercase hex chars. NULL input -> NULL.
    if (name == "SHA256" || name == "SHA2_256") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            result.SetValue(i, Value::VARCHAR(Sha256Hex(v.GetValue<std::string>())));
        }
        return;
    }

    // SHA512 / SHA2_512 — FIPS-180-4 message digest. Returns 128
    // lowercase hex chars. NULL input -> NULL.
    if (name == "SHA512" || name == "SHA2_512") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            result.SetValue(i, Value::VARCHAR(Sha512Hex(v.GetValue<std::string>())));
        }
        return;
    }

    // BASE64_ENCODE / TO_BASE64 — RFC 4648 standard alphabet, '=' padded.
    // NULL input -> NULL. Embedded NULs / arbitrary bytes pass through.
    if (name == "BASE64_ENCODE" || name == "TO_BASE64") {
        static const char *alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            auto s = v.GetValue<std::string>();
            std::string out;
            out.reserve(((s.size() + 2) / 3) * 4);
            size_t k = 0;
            while (k + 3 <= s.size()) {
                uint32_t b = (static_cast<unsigned char>(s[k]) << 16) |
                             (static_cast<unsigned char>(s[k+1]) << 8) |
                              static_cast<unsigned char>(s[k+2]);
                out.push_back(alphabet[(b >> 18) & 0x3F]);
                out.push_back(alphabet[(b >> 12) & 0x3F]);
                out.push_back(alphabet[(b >> 6)  & 0x3F]);
                out.push_back(alphabet[b & 0x3F]);
                k += 3;
            }
            if (k < s.size()) {
                uint32_t b = static_cast<unsigned char>(s[k]) << 16;
                if (k + 1 < s.size()) b |= static_cast<unsigned char>(s[k+1]) << 8;
                out.push_back(alphabet[(b >> 18) & 0x3F]);
                out.push_back(alphabet[(b >> 12) & 0x3F]);
                out.push_back(k + 1 < s.size() ? alphabet[(b >> 6) & 0x3F] : '=');
                out.push_back('=');
            }
            result.SetValue(i, Value::VARCHAR(out));
        }
        return;
    }

    // BASE64_DECODE / FROM_BASE64 — RFC 4648 standard alphabet.
    // Strict: length must be a multiple of 4, all chars in alphabet or
    // padding. Invalid input -> NULL (row-level), not engine error.
    if (name == "BASE64_DECODE" || name == "FROM_BASE64") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        auto decode_char = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            auto s = v.GetValue<std::string>();
            if (s.empty()) {
                result.SetValue(i, Value::VARCHAR(""));
                continue;
            }
            if (s.size() % 4 != 0) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            std::string out;
            out.reserve((s.size() / 4) * 3);
            bool ok = true;
            for (size_t k = 0; k < s.size(); k += 4) {
                int v0 = decode_char(s[k]);
                int v1 = decode_char(s[k+1]);
                int v2 = (s[k+2] == '=') ? -2 : decode_char(s[k+2]);
                int v3 = (s[k+3] == '=') ? -2 : decode_char(s[k+3]);
                if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) { ok = false; break; }
                // Padding only allowed in last group.
                if ((v2 == -2 || v3 == -2) && k + 4 != s.size()) { ok = false; break; }
                if (v2 == -2 && v3 != -2) { ok = false; break; }
                uint32_t b = (static_cast<uint32_t>(v0) << 18) |
                             (static_cast<uint32_t>(v1) << 12);
                out.push_back(static_cast<char>((b >> 16) & 0xFF));
                if (v2 != -2) {
                    b |= static_cast<uint32_t>(v2) << 6;
                    out.push_back(static_cast<char>((b >> 8) & 0xFF));
                    if (v3 != -2) {
                        b |= static_cast<uint32_t>(v3);
                        out.push_back(static_cast<char>(b & 0xFF));
                    }
                }
            }
            if (!ok) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.SetValue(i, Value::VARCHAR(out));
            }
        }
        return;
    }

    // HEX / TO_HEX / UNHEX — hex encode/decode. HEX/TO_HEX accept either
    // an integer (returns lowercase hex digits, no leading zeros for
    // positive; two's-complement at input width for negative) or a
    // VARCHAR (two lowercase hex chars per byte). UNHEX consumes a hex
    // string (case-insensitive); odd length / non-hex chars produce
    // NULL. NULL operand always returns NULL.
    if (name == "HEX" || name == "TO_HEX" || name == "UNHEX") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        auto arg_id = expr.arguments[0]->GetReturnType().id();
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            if (name == "UNHEX") {
                auto s = v.GetValue<std::string>();
                if (s.size() % 2 != 0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                std::string out;
                out.reserve(s.size() / 2);
                bool ok = true;
                auto hex_val = [](char c, int &outv) {
                    if (c >= '0' && c <= '9') { outv = c - '0'; return true; }
                    if (c >= 'a' && c <= 'f') { outv = c - 'a' + 10; return true; }
                    if (c >= 'A' && c <= 'F') { outv = c - 'A' + 10; return true; }
                    return false;
                };
                for (size_t k = 0; k < s.size(); k += 2) {
                    int hi, lo;
                    if (!hex_val(s[k], hi) || !hex_val(s[k+1], lo)) { ok = false; break; }
                    out.push_back(static_cast<char>((hi << 4) | lo));
                }
                if (!ok) {
                    result.GetValidity().SetInvalid(i);
                } else {
                    result.SetValue(i, Value::VARCHAR(out));
                }
                continue;
            }
            // HEX / TO_HEX
            if (arg_id == LogicalTypeId::VARCHAR) {
                auto s = v.GetValue<std::string>();
                static const char *digs = "0123456789abcdef";
                std::string out;
                out.reserve(s.size() * 2);
                for (unsigned char b : s) {
                    out.push_back(digs[b >> 4]);
                    out.push_back(digs[b & 0xF]);
                }
                result.SetValue(i, Value::VARCHAR(out));
            } else {
                // Numeric: format as lowercase hex. Negatives use the
                // two's-complement representation at the input's width.
                int64_t val = 0;
                int width_bytes = 8;
                switch (arg_id) {
                case LogicalTypeId::TINYINT:
                    val = v.GetValue<int8_t>();  width_bytes = 1; break;
                case LogicalTypeId::SMALLINT:
                    val = v.GetValue<int16_t>(); width_bytes = 2; break;
                case LogicalTypeId::INTEGER:
                    val = v.GetValue<int32_t>(); width_bytes = 4; break;
                case LogicalTypeId::BIGINT:
                    val = v.GetValue<int64_t>(); width_bytes = 8; break;
                default:
                    throw NotImplementedException(
                        "HEX for type " + v.type().ToString());
                }
                static const char *digs = "0123456789abcdef";
                std::string out;
                if (val == 0) {
                    out = "0";
                } else if (val > 0) {
                    while (val) {
                        out.push_back(digs[val & 0xF]);
                        val = static_cast<int64_t>(static_cast<uint64_t>(val) >> 4);
                    }
                    std::reverse(out.begin(), out.end());
                } else {
                    // Mask to width's worth of bits.
                    uint64_t mask = width_bytes >= 8 ? ~0ULL : ((1ULL << (width_bytes * 8)) - 1);
                    uint64_t uval = static_cast<uint64_t>(val) & mask;
                    for (int k = width_bytes * 2 - 1; k >= 0; k--) {
                        out.push_back(digs[(uval >> (k * 4)) & 0xF]);
                    }
                }
                result.SetValue(i, Value::VARCHAR(out));
            }
        }
        return;
    }

    // OCTET_LENGTH(s) — byte count, identical to LENGTH today (slothdb
    // is byte-oriented). BIT_LENGTH(s) returns byte_count * 8.
    if (name == "OCTET_LENGTH" || name == "BIT_LENGTH") {
        auto *out = result.GetData<int32_t>();
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        bool mult8 = (name == "BIT_LENGTH");
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            int64_t bytes = static_cast<int64_t>(arg.GetData<string_t>()[i].GetSize());
            int64_t v = mult8 ? bytes * 8 : bytes;
            if (v > std::numeric_limits<int32_t>::max()) {
                result.GetValidity().SetInvalid(i);
                out[i] = 0;
            } else {
                out[i] = static_cast<int32_t>(v);
            }
        }
        return;
    }

    // ASCII(s) — code point of first byte (1-byte semantics; multibyte
    // returns the first byte's value, matching slothdb's byte-oriented
    // LEFT/RIGHT/SUBSTRING). NULL -> NULL; empty -> 0.
    if (name == "ASCII") {
        auto *out = result.GetData<int32_t>();
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            auto s = v.GetValue<std::string>();
            out[i] = s.empty() ? 0 : static_cast<int32_t>(static_cast<unsigned char>(s[0]));
        }
        return;
    }

    // CHR(n) — character with byte value n. NULL -> NULL. Out-of-range
    // values are clamped to 0..127 (7-bit ASCII) for now — full Unicode
    // codepoint handling deferred to a multibyte-aware string layer.
    if (name == "CHR") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int32_t n = v.GetValue<int32_t>();
            if (n == 0) {
                result.SetValue(i, Value::VARCHAR(""));
            } else {
                // Clip to 7-bit ASCII byte until multibyte support lands.
                unsigned char byte = static_cast<unsigned char>(n & 0xFF);
                result.SetValue(i, Value::VARCHAR(std::string(1, static_cast<char>(byte))));
            }
        }
        return;
    }

    if (name == "UPPER" || name == "LOWER") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
            } else {
                auto s = arg.GetData<string_t>()[i].GetString();
                for (auto &c : s) {
                    c = name == "UPPER" ? static_cast<char>(std::toupper(c))
                                        : static_cast<char>(std::tolower(c));
                }
                result.SetValue(i, Value::VARCHAR(s));
            }
        }
        return;
    }

    if (name == "SUBSTRING" || name == "SUBSTR") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector start_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, start_vec, count);
        bool has_len = expr.arguments.size() > 2;
        Vector len_vec(LogicalType::INTEGER(), count);
        if (has_len) Execute(*expr.arguments[2], input, len_vec, count);

        for (idx_t i = 0; i < count; i++) {
            // NULL propagation: if string OR start OR len is NULL the
            // result is NULL. Previously a NULL start crashed with
            // "Cannot get value from NULL" from the typed GetValue.
            bool null_in = !str_vec.GetValidity().RowIsValid(i) ||
                           !start_vec.GetValidity().RowIsValid(i) ||
                           (has_len && !len_vec.GetValidity().RowIsValid(i));
            if (null_in) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto sv = str_vec.GetValue(i);
            if (sv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            int32_t start_raw = start_vec.GetValue(i).GetValue<int32_t>();
            // SQL standard: positions are 1-based; positions <1 are
            // clipped but the "missing" characters STILL count against
            // len in the 3-arg form. SUBSTRING('hello', -2, 5) covers
            // logical positions -2,-1,0,1,2 → physical "he". A negative
            // len produces empty. Out-of-range start produces empty.
            //
            // 2-arg form (no length): suffix from max(start,1) to end
            // of string. Previously len was set to s.size() and then
            // added to start, so SUBSTRING('hello', -2) clipped to
            // 'he' instead of returning 'hello'.
            int64_t s_size_i = static_cast<int64_t>(s.size());
            int64_t effective_start = std::max<int32_t>(1, start_raw);
            int64_t effective_end;
            if (has_len) {
                int32_t len_raw = len_vec.GetValue(i).GetValue<int32_t>();
                if (len_raw <= 0) {
                    result.SetValue(i, Value::VARCHAR(""));
                    continue;
                }
                effective_end =
                    static_cast<int64_t>(start_raw) + static_cast<int64_t>(len_raw);
            } else {
                effective_end = s_size_i + 1;
            }
            if (effective_end <= 1) {
                result.SetValue(i, Value::VARCHAR(""));
                continue;
            }
            int64_t out_start = effective_start - 1;
            int64_t out_len = effective_end - effective_start;
            int64_t s_size = static_cast<int64_t>(s.size());
            if (out_start >= s_size || out_len <= 0) {
                result.SetValue(i, Value::VARCHAR(""));
                continue;
            }
            out_len = std::min<int64_t>(out_len, s_size - out_start);
            result.SetValue(i, Value::VARCHAR(
                s.substr(static_cast<size_t>(out_start),
                         static_cast<size_t>(out_len))));
        }
        return;
    }

    if (name == "REPLACE") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector from_vec(expr.arguments[1]->GetReturnType(), count);
        Vector to_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, from_vec, count);
        Execute(*expr.arguments[2], input, to_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto fv = from_vec.GetValue(i);
            auto tv = to_vec.GetValue(i);
            if (sv.IsNull() || fv.IsNull() || tv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto from = fv.GetValue<std::string>();
            auto to = tv.GetValue<std::string>();
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.length(), to);
                pos += to.length();
            }
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "CONCAT") {
        for (idx_t i = 0; i < count; i++) {
            std::string concat_result;
            for (auto &arg : expr.arguments) {
                Vector v(arg->GetReturnType(), count);
                Execute(*arg, input, v, count);
                auto val = v.GetValue(i);
                if (!val.IsNull()) concat_result += val.ToString();
            }
            result.SetValue(i, Value::VARCHAR(concat_result));
        }
        return;
    }

    // CONCAT_WS(separator, val1, val2, ...) — concatenate with a
    // separator. NULL separator -> NULL result for that row (PG rule).
    // NULL values are skipped (no separator emitted for them). All-NULL
    // values with a non-NULL separator -> empty string.
    if (name == "CONCAT_WS") {
        // Need at least the separator; binder enforces arity.
        if (expr.arguments.empty()) {
            for (idx_t i = 0; i < count; i++) {
                result.SetValue(i, Value::VARCHAR(""));
            }
            return;
        }
        Vector sep_vec(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, sep_vec, count);
        // Pre-evaluate value args once.
        std::vector<Vector> val_vecs;
        val_vecs.reserve(expr.arguments.size() - 1);
        for (size_t a = 1; a < expr.arguments.size(); a++) {
            val_vecs.emplace_back(expr.arguments[a]->GetReturnType(), count);
            Execute(*expr.arguments[a], input, val_vecs.back(), count);
        }
        for (idx_t i = 0; i < count; i++) {
            auto sv = sep_vec.GetValue(i);
            if (sv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            std::string sep = sv.GetValue<std::string>();
            std::string out;
            bool first = true;
            for (size_t a = 0; a < val_vecs.size(); a++) {
                auto v = val_vecs[a].GetValue(i);
                if (v.IsNull()) continue;
                if (!first) out += sep;
                out += v.ToString();
                first = false;
            }
            result.SetValue(i, Value::VARCHAR(out));
        }
        return;
    }

    if (name == "TRIM" || name == "LTRIM" || name == "RTRIM") {
        // Two-arg form: arguments[1] is the character set to trim.
        // Previously the second argument was silently ignored, so
        // TRIM('xxhixx','x') returned 'xxhixx' unchanged — a textbook
        // silent-wrong-result bug.
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        Vector chars_vec(LogicalType::VARCHAR(), count);
        bool has_chars = expr.arguments.size() >= 2;
        if (has_chars) {
            Execute(*expr.arguments[1], input, chars_vec, count);
        }
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            std::string chars = " \t\n\r";
            if (has_chars) {
                auto cv = chars_vec.GetValue(i);
                if (cv.IsNull()) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                chars = cv.GetValue<std::string>();
            }
            auto s = v.GetValue<std::string>();
            if (!chars.empty()) {
                if (name == "TRIM" || name == "LTRIM") {
                    s.erase(0, s.find_first_not_of(chars));
                }
                if (name == "TRIM" || name == "RTRIM") {
                    size_t last = s.find_last_not_of(chars);
                    if (last == std::string::npos) s.clear();
                    else s.erase(last + 1);
                }
            }
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    // ---- Scalar math functions ----

    if (name == "ABS") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        auto physical = arg.GetType().GetInternalType();
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            // Signed-integer ABS at the minimum value overflows (UB in
            // C++; on x86 produces the same negative value back). SQL
            // standard says overflow -> NULL. Matches the LN/SQRT/POWER
            // domain-error idiom (set validity invalid). Also covers
            // INT8/INT16 which previously fell through `default` and
            // silently returned 0.
            switch (physical) {
            case PhysicalType::INT8: {
                auto v = arg.GetData<int8_t>()[i];
                if (v == std::numeric_limits<int8_t>::min()) {
                    result.GetValidity().SetInvalid(i);
                    result.GetData<int8_t>()[i] = 0;
                } else {
                    result.GetData<int8_t>()[i] = static_cast<int8_t>(v < 0 ? -v : v);
                }
                break;
            }
            case PhysicalType::INT16: {
                auto v = arg.GetData<int16_t>()[i];
                if (v == std::numeric_limits<int16_t>::min()) {
                    result.GetValidity().SetInvalid(i);
                    result.GetData<int16_t>()[i] = 0;
                } else {
                    result.GetData<int16_t>()[i] = static_cast<int16_t>(v < 0 ? -v : v);
                }
                break;
            }
            case PhysicalType::INT32: {
                auto v = arg.GetData<int32_t>()[i];
                if (v == std::numeric_limits<int32_t>::min()) {
                    result.GetValidity().SetInvalid(i);
                    result.GetData<int32_t>()[i] = 0;
                } else {
                    result.GetData<int32_t>()[i] = std::abs(v);
                }
                break;
            }
            case PhysicalType::INT64: {
                auto v = arg.GetData<int64_t>()[i];
                if (v == std::numeric_limits<int64_t>::min()) {
                    result.GetValidity().SetInvalid(i);
                    result.GetData<int64_t>()[i] = 0;
                } else {
                    result.GetData<int64_t>()[i] = std::abs(v);
                }
                break;
            }
            case PhysicalType::DOUBLE:
                result.GetData<double>()[i] = std::abs(arg.GetData<double>()[i]); break;
            case PhysicalType::FLOAT:
                result.GetData<float>()[i] = std::abs(arg.GetData<float>()[i]); break;
            default: break;
            }
        }
        return;
    }

    if (name == "CEIL" || name == "CEILING" || name == "FLOOR" || name == "ROUND") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        // ROUND can take an optional second arg `precision`: positive
        // values control fractional digits, negative round to powers
        // of 10, zero is integer-round. Previously the second arg was
        // silently dropped — ROUND(2.345, 2) returned 2 instead of 2.35.
        bool has_precision = (name == "ROUND" && expr.arguments.size() >= 2);
        Vector prec_vec(LogicalType::INTEGER(), count);
        if (has_precision) Execute(*expr.arguments[1], input, prec_vec, count);
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            if (has_precision && !prec_vec.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            double val = 0;
            auto phys = arg.GetType().GetInternalType();
            if (phys == PhysicalType::DOUBLE) val = arg.GetData<double>()[i];
            else if (phys == PhysicalType::FLOAT) val = arg.GetData<float>()[i];
            else if (phys == PhysicalType::INT32) val = arg.GetData<int32_t>()[i];
            else if (phys == PhysicalType::INT64) val = static_cast<double>(arg.GetData<int64_t>()[i]);

            if (name == "CEIL" || name == "CEILING") val = std::ceil(val);
            else if (name == "FLOOR") val = std::floor(val);
            else if (name == "ROUND") {
                if (has_precision) {
                    int32_t p = prec_vec.GetValue(i).GetValue<int32_t>();
                    double factor = std::pow(10.0, static_cast<double>(p));
                    val = std::round(val * factor) / factor;
                } else {
                    val = std::round(val);
                }
            }

            result.GetData<double>()[i] = val;
        }
        return;
    }

    if (name == "POW") {
        // PG/MySQL alias for POWER.
        const_cast<BoundFunction &>(expr).function_name = "POWER";
    }
    if (name == "SQRT" || name == "CBRT" || name == "POWER" || name == "MOD" ||
        name == "POW") {
        Vector arg1(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg1, count);
        if (name == "CBRT") {
            // Real-valued cube root: total over the reals (handles
            // negatives correctly — distinguishes it from SQRT).
            for (idx_t i = 0; i < count; i++) {
                auto val = arg1.GetValue(i);
                if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
                double d;
                auto tid = val.type().id();
                if (tid == LogicalTypeId::INTEGER) d = val.GetValue<int32_t>();
                else if (tid == LogicalTypeId::BIGINT) d = static_cast<double>(val.GetValue<int64_t>());
                else if (tid == LogicalTypeId::SMALLINT) d = val.GetValue<int16_t>();
                else if (tid == LogicalTypeId::TINYINT) d = val.GetValue<int8_t>();
                else if (tid == LogicalTypeId::FLOAT) d = val.GetValue<float>();
                else d = val.GetValue<double>();
                double r = std::cbrt(d);
                if (std::isnan(r) || std::isinf(r)) {
                    result.GetValidity().SetInvalid(i);
                } else {
                    result.GetData<double>()[i] = r;
                }
            }
            return;
        }
        if (name == "SQRT") {
            for (idx_t i = 0; i < count; i++) {
                auto val = arg1.GetValue(i);
                if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
                double d = (val.type().id() == LogicalTypeId::INTEGER)
                    ? val.GetValue<int32_t>() : val.GetValue<double>();
                // SQRT of a negative produces nan; SQL standard says
                // this is a domain error -> NULL (matches POWER's
                // negative-base-with-fractional-exponent guard).
                if (d < 0.0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                result.GetData<double>()[i] = std::sqrt(d);
            }
        } else {
            Vector arg2(expr.arguments[1]->GetReturnType(), count);
            Execute(*expr.arguments[1], input, arg2, count);
            for (idx_t i = 0; i < count; i++) {
                auto v1 = arg1.GetValue(i), v2 = arg2.GetValue(i);
                if (v1.IsNull() || v2.IsNull()) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                // GetValue<double> handles all numeric input types; the
                // prior std::stod(ToString()) round-trip burned CPU and
                // crashed on non-numeric strings.
                double d1, d2;
                try {
                    d1 = v1.type().id() == LogicalTypeId::DOUBLE
                            ? v1.GetValue<double>()
                            : std::stod(v1.ToString());
                    d2 = v2.type().id() == LogicalTypeId::DOUBLE
                            ? v2.GetValue<double>()
                            : std::stod(v2.ToString());
                } catch (...) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                if (name == "POWER") {
                    // Domain errors -> NULL per SQL standard:
                    //   POWER(0, -k)         -> +inf  (was)
                    //   POWER(negative, frac) -> nan
                    if ((d1 == 0.0 && d2 < 0.0) || (d1 < 0.0 && std::floor(d2) != d2)) {
                        result.GetValidity().SetInvalid(i);
                        continue;
                    }
                    double r = std::pow(d1, d2);
                    if (std::isnan(r) || std::isinf(r)) {
                        result.GetValidity().SetInvalid(i);
                    } else {
                        result.GetData<double>()[i] = r;
                    }
                } else {
                    // MOD(x, 0) -> NULL (was nan). Integer % already
                    // returns NULL via the ExecuteArithmetic %-branch;
                    // function-form MOD now matches.
                    if (d2 == 0.0) {
                        result.GetValidity().SetInvalid(i);
                        continue;
                    }
                    result.GetData<double>()[i] = std::fmod(d1, d2);
                }
            }
        }
        return;
    }

    // ---- Additional string functions ----

    if (name == "POSITION" || name == "STRPOS") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector sub_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, sub_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto subv = sub_vec.GetValue(i);
            if (sv.IsNull() || subv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sub = subv.GetValue<std::string>();
            auto pos = s.find(sub);
            result.SetValue(i, Value::INTEGER(pos == std::string::npos ? 0 : static_cast<int32_t>(pos + 1)));
        }
        return;
    }

    if (name == "LEFT" || name == "RIGHT") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector n_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, n_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto nv = n_vec.GetValue(i);
            if (sv.IsNull() || nv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto n = nv.GetValue<int32_t>();
            // Postgres semantics for negative n:
            //   LEFT(s, -k)  = s[: len(s)-k] (drop last k)
            //   RIGHT(s, -k) = s[k :]        (drop first k)
            // Clamps to empty when k >= len(s).
            if (n < 0) {
                int64_t drop = -static_cast<int64_t>(n);
                int64_t keep = static_cast<int64_t>(s.size()) - drop;
                if (keep <= 0) {
                    result.SetValue(i, Value::VARCHAR(""));
                } else {
                    auto k = static_cast<size_t>(keep);
                    result.SetValue(i, Value::VARCHAR(
                        name == "LEFT" ? s.substr(0, k) : s.substr(s.size() - k)));
                }
                continue;
            }
            auto un = static_cast<size_t>(n);
            result.SetValue(i, Value::VARCHAR(
                name == "LEFT" ? s.substr(0, un) : s.substr(s.size() > un ? s.size() - un : 0)));
        }
        return;
    }

    if (name == "LPAD" || name == "RPAD") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector len_vec(expr.arguments[1]->GetReturnType(), count);
        Vector pad_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, len_vec, count);
        Execute(*expr.arguments[2], input, pad_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto nv = len_vec.GetValue(i);
            auto pv = pad_vec.GetValue(i);
            if (sv.IsNull() || nv.IsNull() || pv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto target_len = static_cast<size_t>(nv.GetValue<int32_t>());
            auto pad = pv.GetValue<std::string>();
            while (s.size() < target_len && !pad.empty()) {
                if (name == "LPAD") s = pad + s; else s = s + pad;
            }
            if (s.size() > target_len) s = s.substr(name == "LPAD" ? s.size() - target_len : 0, target_len);
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "REVERSE") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = v.GetValue<std::string>();
            std::reverse(s.begin(), s.end());
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "REPEAT") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector n_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, n_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto nv = n_vec.GetValue(i);
            if (sv.IsNull() || nv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto n = nv.GetValue<int32_t>();
            if (n < 0) n = 0;
            if (n > 65536) throw InvalidInputException("REPEAT count too large (max: 65536)");
            std::string r;
            for (int j = 0; j < n; j++) r += s;
            result.SetValue(i, Value::VARCHAR(r));
        }
        return;
    }

    // INSTR(haystack, needle) — Oracle/MySQL/SQLite spelling of STRPOS.
    // 1-based position of first occurrence, 0 if not found. NULL on
    // either side propagates.
    if (name == "INSTR") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector sub_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, sub_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto subv = sub_vec.GetValue(i);
            if (sv.IsNull() || subv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sub = subv.GetValue<std::string>();
            auto pos = s.find(sub);
            result.SetValue(i, Value::INTEGER(pos == std::string::npos
                                              ? 0 : static_cast<int32_t>(pos + 1)));
        }
        return;
    }

    if (name == "STARTS_WITH" || name == "PREFIX") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pre_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pre_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto pv = pre_vec.GetValue(i);
            if (sv.IsNull() || pv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto p = pv.GetValue<std::string>();
            result.SetValue(i, Value::BOOLEAN(s.substr(0, p.size()) == p));
        }
        return;
    }

    if (name == "ENDS_WITH" || name == "SUFFIX") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector suf_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, suf_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto sufv = suf_vec.GetValue(i);
            if (sv.IsNull() || sufv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sf = sufv.GetValue<std::string>();
            bool match = s.size() >= sf.size() && s.substr(s.size() - sf.size()) == sf;
            result.SetValue(i, Value::BOOLEAN(match));
        }
        return;
    }

    if (name == "CONTAINS") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector sub_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, sub_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto subv = sub_vec.GetValue(i);
            if (sv.IsNull() || subv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto sub = subv.GetValue<std::string>();
            result.SetValue(i, Value::BOOLEAN(s.find(sub) != std::string::npos));
        }
        return;
    }

    if (name == "SPLIT_PART") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector delim_vec(expr.arguments[1]->GetReturnType(), count);
        Vector idx_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, delim_vec, count);
        Execute(*expr.arguments[2], input, idx_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto dv = delim_vec.GetValue(i);
            auto iv = idx_vec.GetValue(i);
            if (sv.IsNull() || dv.IsNull() || iv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto d = dv.GetValue<std::string>();
            auto idx = iv.GetValue<int32_t>();
            // Multi-character delimiter: split via s.find(d). The
            // previous StringUtil::Split with d[0] silently truncated
            // multi-char delimiters to one byte, so SPLIT_PART(
            // 'aXXbXXc', 'XX', 2) returned '' instead of 'b'.
            std::vector<std::string> parts;
            if (d.empty()) {
                parts.push_back(s);
            } else {
                size_t pos = 0;
                while (pos <= s.size()) {
                    auto next = s.find(d, pos);
                    if (next == std::string::npos) {
                        parts.push_back(s.substr(pos));
                        break;
                    }
                    parts.push_back(s.substr(pos, next - pos));
                    pos = next + d.size();
                }
            }
            if (idx >= 1 && idx <= static_cast<int32_t>(parts.size()))
                result.SetValue(i, Value::VARCHAR(parts[idx - 1]));
            else
                result.SetValue(i, Value::VARCHAR(""));
        }
        return;
    }

    // ---- Additional math functions ----

    if (name == "LOG" || name == "LN") {
        // 1-arg form computes the natural log (matches existing
        // behaviour). 2-arg LOG(base, value) computes log base `base`
        // of `value`, previously silently dropped the second arg.
        // Domain errors (non-positive arg, base==1) return NULL.
        bool two_arg = (name == "LOG" && expr.arguments.size() >= 2);
        Vector arg0(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg0, count);
        Vector arg1(two_arg ? expr.arguments[1]->GetReturnType()
                            : LogicalType::DOUBLE(), count);
        if (two_arg) Execute(*expr.arguments[1], input, arg1, count);
        auto val_to_dbl = [](const Value &v) -> double {
            return v.type().id() == LogicalTypeId::INTEGER
                ? v.GetValue<int32_t>() : v.GetValue<double>();
        };
        for (idx_t i = 0; i < count; i++) {
            auto v0 = arg0.GetValue(i);
            if (v0.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            if (two_arg) {
                auto v1 = arg1.GetValue(i);
                if (v1.IsNull()) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                double base = val_to_dbl(v0);
                double val = val_to_dbl(v1);
                if (base <= 0.0 || base == 1.0 || val <= 0.0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                double r = std::log(val) / std::log(base);
                if (std::isnan(r) || std::isinf(r)) {
                    result.GetValidity().SetInvalid(i);
                } else {
                    result.GetData<double>()[i] = r;
                }
            } else {
                double d = val_to_dbl(v0);
                if (d <= 0.0) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                result.GetData<double>()[i] = std::log(d);
            }
        }
        return;
    }

    if (name == "LOG2") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            if (d <= 0.0) { result.GetValidity().SetInvalid(i); continue; }
            result.GetData<double>()[i] = std::log2(d);
        }
        return;
    }

    if (name == "LOG10") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            if (d <= 0.0) { result.GetValidity().SetInvalid(i); continue; }
            result.GetData<double>()[i] = std::log10(d);
        }
        return;
    }

    if (name == "EXP") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            double r = std::exp(d);
            if (std::isnan(r) || std::isinf(r)) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.GetData<double>()[i] = r;
            }
        }
        return;
    }

    if (name == "SIGN") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        // Previously only INTEGER and DOUBLE branches existed; TINYINT,
        // SMALLINT, BIGINT, FLOAT all fell into the `v.GetValue<double>()`
        // path which threw / returned garbage. Result was silently wrong:
        //   SIGN(BIGINT -5)   -> 0   (was)  -> -1
        //   SIGN(TINYINT -5)  -> 1            -> -1
        //   SIGN(SMALLINT -k) -> 1            -> -1
        // Switch on PhysicalType for typed dispatch.
        auto physical = arg.GetType().GetInternalType();
        for (idx_t i = 0; i < count; i++) {
            if (!arg.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            int32_t sign = 0;
            switch (physical) {
            case PhysicalType::INT8: {
                int8_t v = arg.GetData<int8_t>()[i];
                sign = v > 0 ? 1 : (v < 0 ? -1 : 0);
                break;
            }
            case PhysicalType::INT16: {
                int16_t v = arg.GetData<int16_t>()[i];
                sign = v > 0 ? 1 : (v < 0 ? -1 : 0);
                break;
            }
            case PhysicalType::INT32: {
                int32_t v = arg.GetData<int32_t>()[i];
                sign = v > 0 ? 1 : (v < 0 ? -1 : 0);
                break;
            }
            case PhysicalType::INT64: {
                int64_t v = arg.GetData<int64_t>()[i];
                sign = v > 0 ? 1 : (v < 0 ? -1 : 0);
                break;
            }
            case PhysicalType::FLOAT: {
                float v = arg.GetData<float>()[i];
                if (std::isnan(v)) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                sign = v > 0.0f ? 1 : (v < 0.0f ? -1 : 0);
                break;
            }
            case PhysicalType::DOUBLE: {
                double v = arg.GetData<double>()[i];
                if (std::isnan(v)) {
                    result.GetValidity().SetInvalid(i);
                    continue;
                }
                sign = v > 0.0 ? 1 : (v < 0.0 ? -1 : 0);
                break;
            }
            default:
                throw NotImplementedException(
                    "SIGN for type " + arg.GetType().ToString());
            }
            result.SetValue(i, Value::INTEGER(sign));
        }
        return;
    }

    if (name == "PI") {
        for (idx_t i = 0; i < count; i++) {
            result.GetData<double>()[i] = 3.14159265358979323846;
        }
        return;
    }

    if (name == "RANDOM" || name == "RAND") {
        for (idx_t i = 0; i < count; i++) {
            result.GetData<double>()[i] = static_cast<double>(std::rand()) / RAND_MAX;
        }
        return;
    }

    if (name == "LEAST") {
        for (idx_t i = 0; i < count; i++) {
            Value min_val;
            bool first = true;
            for (auto &a : expr.arguments) {
                Vector v(a->GetReturnType(), count);
                Execute(*a, input, v, count);
                auto val = v.GetValue(i);
                if (val.IsNull()) continue;
                if (first || val < min_val) { min_val = val; first = false; }
            }
            if (first) result.GetValidity().SetInvalid(i);
            else result.SetValue(i, min_val);
        }
        return;
    }

    if (name == "GREATEST") {
        for (idx_t i = 0; i < count; i++) {
            Value max_val;
            bool first = true;
            for (auto &a : expr.arguments) {
                Vector v(a->GetReturnType(), count);
                Execute(*a, input, v, count);
                auto val = v.GetValue(i);
                if (val.IsNull()) continue;
                if (first || val > max_val) { max_val = val; first = false; }
            }
            if (first) result.GetValidity().SetInvalid(i);
            else result.SetValue(i, max_val);
        }
        return;
    }

    if (name == "INITCAP") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = v.GetValue<std::string>();
            bool cap_next = true;
            for (auto &c : s) {
                if (std::isalpha(static_cast<unsigned char>(c))) {
                    c = cap_next ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                 : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    cap_next = false;
                } else {
                    cap_next = true;
                }
            }
            result.SetValue(i, Value::VARCHAR(s));
        }
        return;
    }

    if (name == "SIN" || name == "COS" || name == "TAN" ||
        name == "ASIN" || name == "ACOS" || name == "ATAN" ||
        name == "SINH" || name == "COSH" || name == "TANH" ||
        name == "ASINH" || name == "ACOSH" || name == "ATANH") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() :
                       (v.type().id() == LogicalTypeId::BIGINT)  ? static_cast<double>(v.GetValue<int64_t>()) :
                       (v.type().id() == LogicalTypeId::FLOAT)   ? v.GetValue<float>() :
                                                                   v.GetValue<double>();
            // Domain guards: out-of-range inputs would produce nan/inf
            // which leak into projection results, sort keys, and hash
            // groups. Match the LN/LOG/SQRT pattern of returning NULL
            // on domain error.
            if ((name == "ASIN" || name == "ACOS") && (d < -1.0 || d > 1.0)) {
                result.GetValidity().SetInvalid(i); continue;
            }
            if (name == "ACOSH" && d < 1.0) {
                result.GetValidity().SetInvalid(i); continue;
            }
            if (name == "ATANH" && (d <= -1.0 || d >= 1.0)) {
                result.GetValidity().SetInvalid(i); continue;
            }
            double r;
            if (name == "SIN") r = std::sin(d);
            else if (name == "COS") r = std::cos(d);
            else if (name == "TAN") r = std::tan(d);
            else if (name == "ASIN") r = std::asin(d);
            else if (name == "ACOS") r = std::acos(d);
            else if (name == "ATAN") r = std::atan(d);
            else if (name == "SINH") r = std::sinh(d);
            else if (name == "COSH") r = std::cosh(d);
            else if (name == "TANH") r = std::tanh(d);
            else if (name == "ASINH") r = std::asinh(d);
            else if (name == "ACOSH") r = std::acosh(d);
            else r = std::atanh(d);  // ATANH
            if (std::isnan(r) || std::isinf(r)) {
                result.GetValidity().SetInvalid(i);
            } else {
                result.GetData<double>()[i] = r;
            }
        }
        return;
    }

    if (name == "ISNAN" || name == "ISINF" || name == "ISFINITE") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::FLOAT) ? v.GetValue<float>() : v.GetValue<double>();
            bool r;
            if (name == "ISNAN")    r = std::isnan(d);
            else if (name == "ISINF") r = std::isinf(d);
            else                      r = std::isfinite(d);
            result.GetData<bool>()[i] = r;
        }
        return;
    }

    if (name == "ATAN2") {
        Vector a1(expr.arguments[0]->GetReturnType(), count);
        Vector a2(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, a1, count);
        Execute(*expr.arguments[1], input, a2, count);
        for (idx_t i = 0; i < count; i++) {
            auto v1 = a1.GetValue(i), v2 = a2.GetValue(i);
            if (v1.IsNull() || v2.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d1 = (v1.type().id() == LogicalTypeId::INTEGER) ? v1.GetValue<int32_t>() : v1.GetValue<double>();
            double d2 = (v2.type().id() == LogicalTypeId::INTEGER) ? v2.GetValue<int32_t>() : v2.GetValue<double>();
            result.GetData<double>()[i] = std::atan2(d1, d2);
        }
        return;
    }

    if (name == "DEGREES") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            result.GetData<double>()[i] = d * 180.0 / 3.14159265358979323846;
        }
        return;
    }

    if (name == "RADIANS") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            double d = (v.type().id() == LogicalTypeId::INTEGER) ? v.GetValue<int32_t>() : v.GetValue<double>();
            result.GetData<double>()[i] = d * 3.14159265358979323846 / 180.0;
        }
        return;
    }

    if (name == "TRUNC" || name == "TRUNCATE") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        // Optional second arg controls fractional digits, same shape
        // as ROUND. Previously the second arg was silently dropped.
        bool has_precision = expr.arguments.size() >= 2;
        Vector prec_vec(LogicalType::INTEGER(), count);
        if (has_precision) Execute(*expr.arguments[1], input, prec_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = arg.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            if (has_precision && !prec_vec.GetValidity().RowIsValid(i)) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            double d = v.GetValue<double>();
            if (has_precision) {
                int32_t p = prec_vec.GetValue(i).GetValue<int32_t>();
                double factor = std::pow(10.0, static_cast<double>(p));
                d = std::trunc(d * factor) / factor;
            } else {
                d = std::trunc(d);
            }
            result.GetData<double>()[i] = d;
        }
        return;
    }

    // ---- Additional date functions ----

    // Helper used by both DATE_DIFF and DATE_ADD: convert a Value to
    // microseconds-since-epoch regardless of whether the source type is
    // DATE (int32 days), TIMESTAMP (int64 micros), or BIGINT (raw
    // seconds or micros depending on magnitude).
    auto to_micros_any = [](const Value &v) -> int64_t {
        auto tid = v.type().id();
        if (tid == LogicalTypeId::DATE) {
            return static_cast<int64_t>(v.GetValue<int32_t>()) * 86400LL * 1000000LL;
        }
        if (tid == LogicalTypeId::TIMESTAMP || tid == LogicalTypeId::TIMESTAMP_TZ) {
            return v.GetValue<int64_t>();
        }
        if (tid == LogicalTypeId::BIGINT) {
            int64_t raw = v.GetValue<int64_t>();
            int64_t abs_raw = raw < 0 ? -raw : raw;
            return (abs_raw >= 10000000000000LL) ? raw : raw * 1000000LL;
        }
        if (tid == LogicalTypeId::INTEGER) {
            return static_cast<int64_t>(v.GetValue<int32_t>()) * 1000000LL;
        }
        return 0;
    };

    // Hinnant civil-from-days helpers used by month/year arithmetic.
    // Reference: http://howardhinnant.github.io/date_algorithms.html
    auto days_to_civil = [](int64_t days_since_epoch, int &y, unsigned &m, unsigned &d) {
        int64_t z = days_since_epoch + 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = static_cast<unsigned>(z - era * 146097);
        unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        int yi = static_cast<int>(yoe) + static_cast<int>(era) * 400;
        unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
        unsigned mp = (5*doy + 2) / 153;
        d = doy - (153*mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = yi + (m <= 2 ? 1 : 0);
    };
    auto civil_to_days = [](int y, unsigned m, unsigned d) -> int64_t {
        y -= (m <= 2 ? 1 : 0);
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
        return era * 146097 + static_cast<int64_t>(doe) - 719468;
    };
    auto last_day_of_month = [](int y, unsigned m) -> unsigned {
        static const unsigned dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2) {
            bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
            return leap ? 29u : 28u;
        }
        return dim[m - 1];
    };

    // AGE(ts1, ts2) — microsecond difference (ts1 - ts2) as BIGINT.
    // Postgres returns INTERVAL; slothdb has no INTERVAL type so the
    // microsecond BIGINT is the natural substitute (callers can /1e6
    // for seconds, divide by 86400e6 for days, etc.). Accepts DATE
    // and TIMESTAMP for either arg.
    if (name == "AGE") {
        if (expr.arguments.size() != 2) {
            throw NotImplementedException("AGE requires 2 arguments");
        }
        Vector a(expr.arguments[0]->GetReturnType(), count);
        Vector b(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, a, count);
        Execute(*expr.arguments[1], input, b, count);
        auto age_to_micros = [](const Value &v) -> int64_t {
            auto tid = v.type().id();
            if (tid == LogicalTypeId::DATE) {
                return static_cast<int64_t>(v.GetValue<int32_t>()) * 86400LL * 1000000LL;
            }
            if (tid == LogicalTypeId::TIMESTAMP || tid == LogicalTypeId::TIMESTAMP_TZ) {
                return v.GetValue<int64_t>();
            }
            if (tid == LogicalTypeId::BIGINT) {
                int64_t raw = v.GetValue<int64_t>();
                int64_t abs_raw = raw < 0 ? -raw : raw;
                return (abs_raw >= 10000000000000LL) ? raw : raw * 1000000LL;
            }
            return 0;
        };
        for (idx_t i = 0; i < count; i++) {
            auto va = a.GetValue(i);
            auto vb = b.GetValue(i);
            if (va.IsNull() || vb.IsNull()) {
                result.GetValidity().SetInvalid(i); continue;
            }
            int64_t ma = age_to_micros(va);
            int64_t mb = age_to_micros(vb);
            result.SetValue(i, Value::BIGINT(ma - mb));
        }
        return;
    }

    if (name == "DATE_DIFF" || name == "DATEDIFF" || name == "TIMESTAMPDIFF") {
        auto part = StringUtil::Upper(
            ExpressionExecutor::ExecuteScalar(*expr.arguments[0]).GetValue<std::string>());
        Vector ts1(expr.arguments[1]->GetReturnType(), count);
        Vector ts2(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, ts1, count);
        Execute(*expr.arguments[2], input, ts2, count);
        for (idx_t i = 0; i < count; i++) {
            auto v1 = ts1.GetValue(i), v2 = ts2.GetValue(i);
            if (v1.IsNull() || v2.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t m1 = to_micros_any(v1);
            int64_t m2 = to_micros_any(v2);
            int64_t diff_micros = m2 - m1;
            int64_t diff_sec = diff_micros / 1000000;
            int64_t r = 0;
            if (part == "SECOND") r = diff_sec;
            else if (part == "MINUTE") r = diff_sec / 60;
            else if (part == "HOUR") r = diff_sec / 3600;
            else if (part == "DAY") r = diff_sec / 86400;
            else if (part == "WEEK" || part == "WEEKS") r = diff_sec / (86400 * 7);
            else if (part == "MILLISECOND" || part == "MILLISECONDS") r = diff_micros / 1000;
            else if (part == "MICROSECOND" || part == "MICROSECONDS") r = diff_micros;
            else if (part == "MONTH" || part == "MONTHS" ||
                     part == "QUARTER" || part == "QUARTERS" ||
                     part == "YEAR" || part == "YEARS") {
                // Calendar-aware month/quarter/year diff. Convert both
                // operands to civil (Y, M, D), then:
                //   month_diff = (Y2-Y1)*12 + (M2-M1)
                //              - (D2 < D1 ? 1 : 0)
                // matches Postgres semantics: "full months elapsed".
                int64_t days1 = m1 / (86400LL * 1000000LL);
                if (m1 < 0 && (m1 % (86400LL * 1000000LL)) != 0) days1--;
                int64_t days2 = m2 / (86400LL * 1000000LL);
                if (m2 < 0 && (m2 % (86400LL * 1000000LL)) != 0) days2--;
                int y1, y2; unsigned mo1, mo2, d1, d2;
                days_to_civil(days1, y1, mo1, d1);
                days_to_civil(days2, y2, mo2, d2);
                int64_t month_diff = (int64_t)(y2 - y1) * 12 +
                                     (int64_t)mo2 - (int64_t)mo1;
                if (month_diff > 0 && d2 < d1) month_diff--;
                if (month_diff < 0 && d2 > d1) month_diff++;
                if (part == "MONTH" || part == "MONTHS") r = month_diff;
                else if (part == "QUARTER" || part == "QUARTERS") r = month_diff / 3;
                else r = month_diff / 12;  // YEAR/YEARS
            }
            else {
                throw NotImplementedException(
                    "DATE_DIFF unit '" + part + "' not yet supported "
                    "(supported: SECOND, MINUTE, HOUR, DAY, WEEK, MONTH, "
                    "QUARTER, YEAR, MILLISECOND, MICROSECOND)");
            }
            result.SetValue(i, Value::BIGINT(r));
        }
        return;
    }

    if (name == "DATE_ADD" || name == "DATEADD") {
        auto part = StringUtil::Upper(
            ExpressionExecutor::ExecuteScalar(*expr.arguments[0]).GetValue<std::string>());
        Vector n_vec(expr.arguments[1]->GetReturnType(), count);
        Vector ts_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, n_vec, count);
        Execute(*expr.arguments[2], input, ts_vec, count);
        auto ts_type = expr.arguments[2]->GetReturnType().id();
        bool day_grain = (part == "DAY" || part == "DAYS" ||
                          part == "WEEK" || part == "WEEKS" ||
                          part == "MONTH" || part == "MONTHS" ||
                          part == "QUARTER" || part == "QUARTERS" ||
                          part == "YEAR" || part == "YEARS");
        bool preserve_date = (ts_type == LogicalTypeId::DATE) && day_grain;
        bool calendar_unit = (part == "MONTH" || part == "MONTHS" ||
                              part == "QUARTER" || part == "QUARTERS" ||
                              part == "YEAR" || part == "YEARS");
        for (idx_t i = 0; i < count; i++) {
            auto n_val = n_vec.GetValue(i);
            auto ts_val = ts_vec.GetValue(i);
            if (n_val.IsNull() || ts_val.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            int64_t n = (n_val.type().id() == LogicalTypeId::INTEGER)
                ? n_val.GetValue<int32_t>() : n_val.GetValue<int64_t>();
            int64_t ts_micros = to_micros_any(ts_val);
            int64_t result_micros = 0;
            if (calendar_unit) {
                // Decompose ts_micros into civil date + intra-day micros,
                // add to month component, clamp day to month length (so
                // 2024-01-31 + 1 month -> 2024-02-29), re-encode.
                int64_t day_micros = 86400LL * 1000000LL;
                int64_t days = ts_micros / day_micros;
                int64_t rem = ts_micros - days * day_micros;
                if (rem < 0) { days--; rem += day_micros; }
                int y; unsigned m, d;
                days_to_civil(days, y, m, d);
                int64_t months_to_add = n;
                if (part == "QUARTER" || part == "QUARTERS") months_to_add = n * 3;
                else if (part == "YEAR" || part == "YEARS") months_to_add = n * 12;
                // Add months, normalize to [1,12].
                int64_t total_months = (int64_t)(y) * 12 + (int64_t)(m - 1) + months_to_add;
                int64_t new_y_signed = total_months >= 0 ? total_months / 12
                                                          : (total_months - 11) / 12;
                int64_t new_m_zero = total_months - new_y_signed * 12;
                unsigned new_m = static_cast<unsigned>(new_m_zero + 1);
                int new_y = static_cast<int>(new_y_signed);
                unsigned max_d = last_day_of_month(new_y, new_m);
                unsigned new_d = d > max_d ? max_d : d;
                int64_t new_days = civil_to_days(new_y, new_m, new_d);
                result_micros = new_days * day_micros + rem;
            } else {
                int64_t add_micros = 0;
                if (part == "SECOND" || part == "SECONDS") add_micros = n * 1000000LL;
                else if (part == "MINUTE" || part == "MINUTES") add_micros = n * 60LL * 1000000LL;
                else if (part == "HOUR" || part == "HOURS") add_micros = n * 3600LL * 1000000LL;
                else if (part == "DAY" || part == "DAYS") add_micros = n * 86400LL * 1000000LL;
                else if (part == "WEEK" || part == "WEEKS") add_micros = n * 7LL * 86400LL * 1000000LL;
                else if (part == "MILLISECOND" || part == "MILLISECONDS") add_micros = n * 1000LL;
                else if (part == "MICROSECOND" || part == "MICROSECONDS") add_micros = n;
                else {
                    throw NotImplementedException(
                        "DATE_ADD unit '" + part + "' not yet supported "
                        "(supported: SECOND, MINUTE, HOUR, DAY, WEEK, MONTH, "
                        "QUARTER, YEAR, MILLISECOND, MICROSECOND)");
                }
                result_micros = ts_micros + add_micros;
            }
            if (preserve_date) {
                int64_t days = result_micros / (86400LL * 1000000LL);
                if (result_micros < 0 && (result_micros % (86400LL * 1000000LL)) != 0) {
                    days--;
                }
                result.SetValue(i, Value::DATE(static_cast<int32_t>(days)));
            } else {
                result.SetValue(i, Value::TIMESTAMP(result_micros));
            }
        }
        return;
    }

    if (name == "STRFTIME" || name == "FORMAT_TIMESTAMP") {
        // Simple format: return ISO-like string.
        Vector ts_vec(expr.arguments.size() > 1 ? expr.arguments[1]->GetReturnType()
                                                 : expr.arguments[0]->GetReturnType(), count);
        Execute(expr.arguments.size() > 1 ? *expr.arguments[1] : *expr.arguments[0], input, ts_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto v = ts_vec.GetValue(i);
            if (v.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = v.GetValue<int64_t>();
            auto seconds = static_cast<time_t>(micros / 1000000);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &seconds);
#else
            gmtime_r(&seconds, &tm_buf);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
            result.SetValue(i, Value::VARCHAR(std::string(buf)));
        }
        return;
    }

    // ---- Null handling functions ----

    // IFNULL(a, b) and NVL(a, b) reach this dispatcher with the same arg
    // layout as COALESCE — route them to the same loop.
    if (name == "COALESCE" || name == "IFNULL" || name == "NVL") {
        for (idx_t i = 0; i < count; i++) {
            bool found = false;
            for (auto &arg : expr.arguments) {
                Vector v(arg->GetReturnType(), count);
                Execute(*arg, input, v, count);
                auto val = v.GetValue(i);
                if (!val.IsNull()) {
                    result.SetValue(i, val);
                    found = true;
                    break;
                }
            }
            if (!found) result.GetValidity().SetInvalid(i);
        }
        return;
    }

    if (name == "NULLIF") {
        Vector arg1(expr.arguments[0]->GetReturnType(), count);
        Vector arg2(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg1, count);
        Execute(*expr.arguments[1], input, arg2, count);
        for (idx_t i = 0; i < count; i++) {
            auto v1 = arg1.GetValue(i), v2 = arg2.GetValue(i);
            if (v1 == v2) result.GetValidity().SetInvalid(i);
            else result.SetValue(i, v1);
        }
        return;
    }

    // ---- Regex functions ----

    if (name == "REGEXP_MATCHES" || name == "REGEXP_MATCH") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pat_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pat_vec, count);
        auto *out = result.GetData<bool>();
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto pv = pat_vec.GetValue(i);
            if (sv.IsNull() || pv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto p = pv.GetValue<std::string>();
            try {
                std::regex re(p);
                out[i] = std::regex_search(s, re);
            } catch (...) {
                out[i] = false;
            }
        }
        return;
    }

    if (name == "REGEXP_REPLACE") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pat_vec(expr.arguments[1]->GetReturnType(), count);
        Vector rep_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pat_vec, count);
        Execute(*expr.arguments[2], input, rep_vec, count);
        // Translate SQL-style replacement (\1..\9 backrefs, \\ literal) into
        // std::regex_replace ECMAScript form ($1..$9, $$ literal $). Bare $
        // in the input must be escaped to $$ so it isn't reinterpreted.
        auto translate_replacement = [](const std::string &r) {
            std::string out;
            out.reserve(r.size());
            for (size_t k = 0; k < r.size(); ++k) {
                char c = r[k];
                if (c == '\\' && k + 1 < r.size()) {
                    char n = r[k + 1];
                    if (n >= '0' && n <= '9') {
                        out += '$';
                        out += n;
                        ++k;
                        continue;
                    }
                    if (n == '\\') {
                        out += '\\';
                        ++k;
                        continue;
                    }
                    // Unknown escape: keep backslash + char as-is.
                    out += c;
                    out += n;
                    ++k;
                    continue;
                }
                if (c == '$') {
                    out += "$$";
                    continue;
                }
                out += c;
            }
            return out;
        };
        std::optional<std::regex> compiled_re;
        std::string cached_pattern;
        bool have_cached = false;
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto pv = pat_vec.GetValue(i);
            auto rv = rep_vec.GetValue(i);
            if (sv.IsNull() || pv.IsNull() || rv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto p = pv.GetValue<std::string>();
            auto r = rv.GetValue<std::string>();
            try {
                if (!have_cached || cached_pattern != p) {
                    have_cached = false;
                    compiled_re.emplace(p);
                    cached_pattern = p;
                    have_cached = true;
                }
                auto rep = translate_replacement(r);
                result.SetValue(i, Value::VARCHAR(std::regex_replace(s, *compiled_re, rep)));
            } catch (...) {
                result.SetValue(i, Value::VARCHAR(s));
            }
        }
        return;
    }

    if (name == "REGEXP_EXTRACT") {
        Vector str_vec(expr.arguments[0]->GetReturnType(), count);
        Vector pat_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, str_vec, count);
        Execute(*expr.arguments[1], input, pat_vec, count);
        for (idx_t i = 0; i < count; i++) {
            auto sv = str_vec.GetValue(i);
            auto pv = pat_vec.GetValue(i);
            if (sv.IsNull() || pv.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            auto s = sv.GetValue<std::string>();
            auto p = pv.GetValue<std::string>();
            try {
                std::regex re(p);
                std::smatch m;
                if (std::regex_search(s, m, re) && m.size() > 1) {
                    result.SetValue(i, Value::VARCHAR(m[1].str()));
                } else if (std::regex_search(s, m, re)) {
                    result.SetValue(i, Value::VARCHAR(m[0].str()));
                } else {
                    result.GetValidity().SetInvalid(i);
                }
            } catch (...) {
                result.GetValidity().SetInvalid(i);
            }
        }
        return;
    }

    // ---- Timestamp/Date functions ----

    if (name == "NOW" || name == "CURRENT_TIMESTAMP" ||
        name == "LOCALTIMESTAMP") {
        // Return a typed TIMESTAMP so ToString renders as
        // 'YYYY-MM-DD HH:MM:SS' and downstream operators (cast,
        // comparison, EXTRACT) recognize it. Previously stored as
        // raw BIGINT micros which leaked the 18-digit integer to
        // every user-facing output.
        auto now = std::chrono::system_clock::now();
        auto epoch = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::TIMESTAMP(epoch));
        }
        return;
    }

    if (name == "CURRENT_USER" || name == "SESSION_USER" ||
        name == "USER") {
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::VARCHAR("slothdb"));
        }
        return;
    }
    if (name == "CURRENT_SCHEMA") {
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::VARCHAR("main"));
        }
        return;
    }
    if (name == "CURRENT_DATABASE") {
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::VARCHAR("slothdb"));
        }
        return;
    }
    if (name == "VERSION") {
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::VARCHAR("slothdb"));
        }
        return;
    }
    if (name == "UUID" || name == "GEN_RANDOM_UUID") {
        // RFC 4122 variant 1 / version 4: 128 random bits with the
        // version nibble and variant bits patched in. Use the engine's
        // existing RNG (rand()) — not cryptographically strong, but
        // matches DuckDB / PG's gen_random_uuid() output shape.
        static const char hex[] = "0123456789abcdef";
        for (idx_t i = 0; i < count; i++) {
            unsigned char b[16];
            for (int k = 0; k < 16; k++) b[k] = static_cast<unsigned char>(rand() & 0xff);
            b[6] = (b[6] & 0x0f) | 0x40; // version 4
            b[8] = (b[8] & 0x3f) | 0x80; // variant 1
            char buf[37];
            char *p = buf;
            for (int k = 0; k < 16; k++) {
                *p++ = hex[(b[k] >> 4) & 0xf];
                *p++ = hex[b[k] & 0xf];
                if (k == 3 || k == 5 || k == 7 || k == 9) *p++ = '-';
            }
            *p = '\0';
            result.SetValue(i, Value::VARCHAR(std::string(buf, 36)));
        }
        return;
    }

    if (name == "CURRENT_DATE") {
        // Return a typed DATE (epoch days). Previously encoded as
        // YYYYMMDD INTEGER, which made EXTRACT(YEAR), DATE arithmetic,
        // and CAST-to-VARCHAR all misbehave. Compute epoch days
        // directly from the local tm components via the Hinnant
        // days_from_civil formula to avoid relying on platform
        // timegm/_mkgmtime.
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
#ifdef _MSC_VER
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif
        int y = tm_buf.tm_year + 1900;
        unsigned mo = static_cast<unsigned>(tm_buf.tm_mon + 1);
        unsigned d = static_cast<unsigned>(tm_buf.tm_mday);
        // Hinnant civil_from_days inverse.
        int y_adj = y - (mo <= 2 ? 1 : 0);
        int era = (y_adj >= 0 ? y_adj : y_adj - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y_adj - era * 400);
        unsigned doy = (153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
        int32_t days = static_cast<int32_t>(era * 146097 +
                                             static_cast<int>(doe) - 719468);
        for (idx_t i = 0; i < count; i++) {
            result.SetValue(i, Value::DATE(days));
        }
        return;
    }

    if (name == "EXTRACT" || name == "DATE_PART") {
        // EXTRACT(part, timestamp_expr)
        // part is a string constant. The argument's unit is inferred per row:
        //   |v| >= 1e13  -> microseconds since epoch (matches NOW/TO_TIMESTAMP)
        //   otherwise    -> seconds since epoch (common epoch-second timestamps)
        // Time-of-day parts (HOUR/MINUTE/SECOND/DOW/EPOCH) use direct integer
        // arithmetic and avoid the gmtime round-trip; calendar parts
        // (YEAR/MONTH/DAY) still go through gmtime_s/gmtime_r.
        auto part_str = ExpressionExecutor::ExecuteScalar(*expr.arguments[0])
                            .GetValue<std::string>();
        auto part = StringUtil::Upper(part_str);

        Vector ts_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, ts_vec, count);

        // Bucket parts: arithmetic (no gmtime) vs calendar (gmtime).
        // MILLISECOND / MICROSECOND need micros-of-second, derived from
        // the raw input when the value is TIMESTAMP-typed or stored as
        // microseconds (|raw| >= 1e13). Previously the EXTRACT branch
        // didn't list these, so MILLI/MICROSECOND silently returned 0.
        bool is_arith = (part == "HOUR" || part == "MINUTE" || part == "SECOND" ||
                         part == "EPOCH" || part == "DOW" || part == "DAYOFWEEK" ||
                         part == "MILLISECOND" || part == "MILLISECONDS" ||
                         part == "MICROSECOND" || part == "MICROSECONDS");
        bool is_calendar = (part == "YEAR" || part == "MONTH" || part == "DAY");
        bool is_sub_second = (part == "MILLISECOND" || part == "MILLISECONDS" ||
                               part == "MICROSECOND" || part == "MICROSECONDS");
        // Calendar-extended parts that need full civil decomposition
        // (week, quarter, day-of-year, century, etc.). Each is silently
        // returning 0 before this commit.
        bool is_calendar_ext = (part == "WEEK" || part == "WEEKS" ||
                                part == "QUARTER" || part == "QUARTERS" ||
                                part == "DOY" || part == "DAYOFYEAR" ||
                                part == "ISODOW" || part == "ISOYEAR" ||
                                part == "CENTURY" || part == "DECADE" ||
                                part == "MILLENNIUM");

        auto floor_mod = [](int64_t a, int64_t m) -> int64_t {
            int64_t r = a % m;
            if (r < 0) r += m;
            return r;
        };
        auto floor_div = [](int64_t a, int64_t m) -> int64_t {
            int64_t q = a / m;
            if ((a % m) != 0 && ((a < 0) != (m < 0))) q--;
            return q;
        };

        // Vectorized fast path: timestamps are commonly BIGINT epoch seconds,
        // and the arithmetic parts (HOUR/MINUTE/SECOND/EPOCH/DOW) need only
        // direct integer arithmetic. Skip Value-boxing each row — go straight
        // from int64_t input slot to int64_t output slot. Cuts per-row cost
        // from ~hundreds of ns to a handful of cycles. A minute extract
        // dropped from >30s timeout to a few seconds.
        auto ts_tid = ts_vec.GetType().id();
        if (is_arith && ts_tid == LogicalTypeId::BIGINT) {
            const int64_t *ts_data = ts_vec.GetData<int64_t>();
            int64_t *out_data = result.GetData<int64_t>();
            const auto &ts_valid = ts_vec.GetValidity();
            auto &out_valid = result.GetValidity();
            for (idx_t i = 0; i < count; i++) {
                if (!ts_valid.RowIsValid(i)) { out_valid.SetInvalid(i); continue; }
                int64_t raw = ts_data[i];
                int64_t abs_raw = raw < 0 ? -raw : raw;
                bool is_micros = (abs_raw >= 10000000000000LL);
                int64_t seconds = is_micros ? raw / 1000000 : raw;
                int64_t extracted = 0;
                if (part == "SECOND") extracted = floor_mod(seconds, 60);
                else if (part == "MINUTE") extracted = floor_mod(floor_div(seconds, 60), 60);
                else if (part == "HOUR") extracted = floor_mod(floor_div(seconds, 3600), 24);
                else if (part == "EPOCH") extracted = seconds;
                else if (part == "DOW" || part == "DAYOFWEEK")
                    extracted = floor_mod(floor_div(seconds, 86400) + 4, 7);
                else if (part == "MICROSECOND" || part == "MICROSECONDS") {
                    extracted = is_micros ? floor_mod(raw, 1000000) : 0;
                } else if (part == "MILLISECOND" || part == "MILLISECONDS") {
                    extracted = is_micros ? floor_mod(raw, 1000000) / 1000 : 0;
                }
                out_data[i] = extracted;
            }
            return;
        }

        auto to_seconds = [](const Value &v) -> int64_t {
            int64_t raw = 0;
            if (v.type().id() == LogicalTypeId::TIMESTAMP)
                return v.GetValue<int64_t>() / 1000000;  // micros -> seconds
            else if (v.type().id() == LogicalTypeId::DATE)
                return static_cast<int64_t>(v.GetValue<int32_t>()) * 86400;  // days -> seconds
            else if (v.type().id() == LogicalTypeId::BIGINT) raw = v.GetValue<int64_t>();
            else if (v.type().id() == LogicalTypeId::INTEGER)
                raw = static_cast<int64_t>(v.GetValue<int32_t>());
            else return 0;
            // Heuristic: values with magnitude >= 1e13 are microseconds; the
            // INTEGER path can never reach that, so it stays in seconds.
            int64_t abs_raw = raw < 0 ? -raw : raw;
            return (abs_raw >= 10000000000000LL) ? raw / 1000000 : raw;
        };

        // Extract micros-of-second from the boxed value. For
        // TIMESTAMP-typed inputs we have full precision; for BIGINT
        // input we use the same |raw|>=1e13 heuristic as the fast path.
        auto to_subsecond_micros = [](const Value &v) -> int64_t {
            if (v.type().id() == LogicalTypeId::TIMESTAMP ||
                v.type().id() == LogicalTypeId::TIMESTAMP_TZ) {
                int64_t micros = v.GetValue<int64_t>();
                int64_t r = micros % 1000000;
                if (r < 0) r += 1000000;
                return r;
            }
            if (v.type().id() == LogicalTypeId::BIGINT) {
                int64_t raw = v.GetValue<int64_t>();
                int64_t abs_raw = raw < 0 ? -raw : raw;
                if (abs_raw < 10000000000000LL) return 0;
                int64_t r = raw % 1000000;
                if (r < 0) r += 1000000;
                return r;
            }
            return 0;
        };

        for (idx_t i = 0; i < count; i++) {
            auto ts_val = ts_vec.GetValue(i);
            if (ts_val.IsNull()) {
                result.GetValidity().SetInvalid(i);
                continue;
            }
            int64_t seconds = to_seconds(ts_val);

            int64_t extracted = 0;
            if (is_sub_second) {
                int64_t micros = to_subsecond_micros(ts_val);
                if (part == "MICROSECOND" || part == "MICROSECONDS") extracted = micros;
                else extracted = micros / 1000;  // MILLISECOND[S]
            } else if (is_arith) {
                if (part == "SECOND") extracted = floor_mod(seconds, 60);
                else if (part == "MINUTE") extracted = floor_mod(floor_div(seconds, 60), 60);
                else if (part == "HOUR") extracted = floor_mod(floor_div(seconds, 3600), 24);
                else if (part == "EPOCH") extracted = seconds;
                else if (part == "DOW" || part == "DAYOFWEEK")
                    extracted = floor_mod(floor_div(seconds, 86400) + 4, 7);
            } else if (is_calendar || is_calendar_ext) {
                // Decompose seconds into civil (year, month, day, day-
                // of-year) via Hinnant days_from_civil reverse — used
                // by YEAR/MONTH/DAY and the new WEEK/QUARTER/DOY/
                // ISO*/CENTURY/DECADE/MILLENNIUM parts.
                int64_t days = seconds / 86400;
                if (seconds < 0 && (seconds % 86400) != 0) --days;
                int year; unsigned mo, d, doy;
                {
                    int64_t z = days + 719468;
                    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                    unsigned doe = static_cast<unsigned>(z - era * 146097);
                    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
                    int yi = static_cast<int>(yoe) + static_cast<int>(era) * 400;
                    unsigned dy = doe - (365*yoe + yoe/4 - yoe/100);
                    unsigned mp = (5*dy + 2) / 153;
                    d = dy - (153*mp + 2) / 5 + 1;
                    mo = mp < 10 ? mp + 3 : mp - 9;
                    year = yi + (mo <= 2 ? 1 : 0);
                    // Compute day-of-year (1-based).
                    // Days at month start for non-leap year:
                    static const unsigned mdays[12] =
                        {0,31,59,90,120,151,181,212,243,273,304,334};
                    bool leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
                    doy = mdays[mo - 1] + d + (leap && mo > 2 ? 1u : 0u);
                }
                if (part == "YEAR")        extracted = year;
                else if (part == "MONTH")  extracted = mo;
                else if (part == "DAY")    extracted = d;
                else if (part == "DOY" || part == "DAYOFYEAR")
                                            extracted = doy;
                else if (part == "QUARTER" || part == "QUARTERS")
                                            extracted = (mo - 1) / 3 + 1;
                else if (part == "DECADE") extracted = year / 10;
                else if (part == "CENTURY")    extracted = (year > 0 ? (year - 1) / 100 + 1
                                                                    : year / 100);
                else if (part == "MILLENNIUM") extracted = (year > 0 ? (year - 1) / 1000 + 1
                                                                    : year / 1000);
                else if (part == "ISODOW") {
                    // ISO day-of-week: Mon=1..Sun=7.
                    int64_t dow_zero = floor_mod(floor_div(seconds, 86400) + 4, 7); // Sun=0..Sat=6
                    extracted = ((dow_zero + 6) % 7) + 1;
                }
                else if (part == "WEEK" || part == "WEEKS" || part == "ISOYEAR") {
                    // ISO 8601 week: week containing Thursday determines
                    // year. Compute the Thursday of the same ISO week.
                    int64_t dow_zero = floor_mod(floor_div(seconds, 86400) + 4, 7); // Sun=0
                    int64_t iso_dow = ((dow_zero + 6) % 7) + 1; // Mon=1..Sun=7
                    int64_t thursday_days = days + (4 - iso_dow);
                    // Decompose Thursday_days to civil.
                    int64_t tz = thursday_days + 719468;
                    int64_t tera = (tz >= 0 ? tz : tz - 146096) / 146097;
                    unsigned tdoe = static_cast<unsigned>(tz - tera * 146097);
                    unsigned tyoe = (tdoe - tdoe/1460 + tdoe/36524 - tdoe/146096) / 365;
                    int tyi = static_cast<int>(tyoe) + static_cast<int>(tera) * 400;
                    unsigned tdy = tdoe - (365*tyoe + tyoe/4 - tyoe/100);
                    unsigned tmp = (5*tdy + 2) / 153;
                    unsigned tmo = tmp < 10 ? tmp + 3 : tmp - 9;
                    int thursday_year = tyi + (tmo <= 2 ? 1 : 0);
                    if (part == "ISOYEAR") {
                        extracted = thursday_year;
                    } else {
                        // Find the ordinal of Jan 1 of thursday_year and
                        // compute the week number as ((thursday_days -
                        // jan1_thursday_of_year_days)/7) + 1.
                        // Days-from-civil for jan 1 of thursday_year:
                        int y = thursday_year;
                        unsigned m_ = 1u, d_ = 1u;
                        int y_adj = y - (m_ <= 2 ? 1 : 0);
                        int era1 = (y_adj >= 0 ? y_adj : y_adj - 399) / 400;
                        unsigned yoe1 = static_cast<unsigned>(y_adj - era1 * 400);
                        unsigned doy1 = (153 * (m_ > 2 ? m_ - 3 : m_ + 9) + 2) / 5 + d_ - 1;
                        unsigned doe1 = yoe1 * 365 + yoe1/4 - yoe1/100 + doy1;
                        int64_t jan1_days = era1 * 146097LL + static_cast<int64_t>(doe1) - 719468;
                        // Move to thursday of week 1 (the Thursday on
                        // or after jan 1).
                        int64_t jan1_dow_zero = floor_mod(jan1_days + 4, 7);
                        int64_t jan1_iso_dow = ((jan1_dow_zero + 6) % 7) + 1;
                        int64_t week1_thursday = jan1_days + (4 - jan1_iso_dow);
                        extracted = (thursday_days - week1_thursday) / 7 + 1;
                    }
                }
            }

            // Unknown part — silent 0 was hiding bugs. Surface clearly.
            if (!is_arith && !is_sub_second && !is_calendar &&
                !is_calendar_ext) {
                throw NotImplementedException(
                    "EXTRACT field '" + part + "' not supported "
                    "(supported: YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, "
                    "MILLISECOND, MICROSECOND, EPOCH, DOW, DAYOFWEEK, "
                    "ISODOW, DOY, DAYOFYEAR, WEEK, QUARTER, DECADE, "
                    "CENTURY, MILLENNIUM, ISOYEAR)");
            }
            result.SetValue(i, Value::BIGINT(extracted));
        }
        return;
    }

    if (name == "DATE_TRUNC") {
        auto part = StringUtil::Upper(
            ExpressionExecutor::ExecuteScalar(*expr.arguments[0]).GetValue<std::string>());
        Vector ts_vec(expr.arguments[1]->GetReturnType(), count);
        Execute(*expr.arguments[1], input, ts_vec, count);

        for (idx_t i = 0; i < count; i++) {
            auto ts_val = ts_vec.GetValue(i);
            if (ts_val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            // Auto-detect input scale (matches DATE_PART/EXTRACT logic):
            //   |raw| >= 1e13  -> microseconds since epoch (NOW/TO_TIMESTAMP)
            //   otherwise      -> seconds since epoch (common BIGINT epoch-second timestamps)
            // Output preserves the input scale so downstream GROUP BY/ORDER BY
            // remain self-consistent against the original column.
            int64_t raw = 0;
            bool input_is_micros = false;
            if (ts_val.type().id() == LogicalTypeId::TIMESTAMP) {
                raw = ts_val.GetValue<int64_t>();
                input_is_micros = true;
            } else if (ts_val.type().id() == LogicalTypeId::DATE) {
                raw = static_cast<int64_t>(ts_val.GetValue<int32_t>()) * 86400;
                input_is_micros = false;
            } else if (ts_val.type().id() == LogicalTypeId::INTEGER) {
                raw = static_cast<int64_t>(ts_val.GetValue<int32_t>());
                input_is_micros = false;
            } else {
                raw = ts_val.GetValue<int64_t>();
                int64_t abs_raw = raw < 0 ? -raw : raw;
                input_is_micros = (abs_raw >= 10000000000000LL);
            }
            int64_t micros = input_is_micros ? raw : raw * 1000000;

            // Sub-second truncation needs no tm round-trip. DATE_TRUNC
            // return type is BIGINT-microseconds-since-epoch (see
            // binder.cpp DATE_TRUNC return_type), so always emit micros
            // regardless of input scale. Previously the result was
            // stored as seconds when input was DATE/INT, which the
            // TIMESTAMP rendering interpreted as ~1970-01-01 + 28 min.
            if (part == "MICROSECOND" || part == "MICROSECONDS") {
                result.SetValue(i, Value::BIGINT(micros));
                continue;
            }
            if (part == "MILLISECOND" || part == "MILLISECONDS") {
                result.SetValue(i, Value::BIGINT((micros / 1000) * 1000));
                continue;
            }

            auto seconds = micros / 1000000;
            auto time_t_val = static_cast<time_t>(seconds);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &time_t_val);
#else
            gmtime_r(&time_t_val, &tm_buf);
#endif
            if (part == "SECOND" || part == "SECONDS") { /* already second-precise */ }
            else if (part == "MINUTE" || part == "MINUTES") { tm_buf.tm_sec = 0; }
            else if (part == "HOUR" || part == "HOURS") { tm_buf.tm_min = 0; tm_buf.tm_sec = 0; }
            else if (part == "DAY" || part == "DAYS") { tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0; }
            else if (part == "WEEK" || part == "WEEKS") {
                // Truncate to the Monday of that week (ISO 8601).
                int days_back = (tm_buf.tm_wday == 0) ? 6 : tm_buf.tm_wday - 1;
                tm_buf.tm_mday -= days_back;
                tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "MONTH" || part == "MONTHS") {
                tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "QUARTER" || part == "QUARTERS") {
                tm_buf.tm_mon = (tm_buf.tm_mon / 3) * 3;
                tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "YEAR" || part == "YEARS") {
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "DECADE" || part == "DECADES") {
                int year = tm_buf.tm_year + 1900;
                tm_buf.tm_year = (year - (year % 10)) - 1900;
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "CENTURY" || part == "CENTURIES") {
                int year = tm_buf.tm_year + 1900;
                tm_buf.tm_year = (year - (year % 100)) - 1900;
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }
            else if (part == "MILLENNIUM" || part == "MILLENNIA") {
                int year = tm_buf.tm_year + 1900;
                tm_buf.tm_year = (year - (year % 1000)) - 1900;
                tm_buf.tm_mon = 0; tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            }

            int64_t trunc_secs = static_cast<int64_t>(
#ifdef _MSC_VER
                _mkgmtime(&tm_buf)
#else
                timegm(&tm_buf)
#endif
            );
            // Always emit microseconds to match the BIGINT-micros
            // return type bound by the binder. Previously DATE/INT
            // inputs emitted seconds, which the TIMESTAMP renderer
            // showed as ~1970-01-01 + 28 min.
            result.SetValue(i, Value::BIGINT(trunc_secs * 1000000LL));
        }
        return;
    }

    if (name == "TO_TIMESTAMP") {
        // Convert epoch seconds to timestamp (microseconds).
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t epoch_sec = 0;
            if (val.type().id() == LogicalTypeId::INTEGER)
                epoch_sec = val.GetValue<int32_t>();
            else if (val.type().id() == LogicalTypeId::BIGINT)
                epoch_sec = val.GetValue<int64_t>();
            else if (val.type().id() == LogicalTypeId::DOUBLE)
                epoch_sec = static_cast<int64_t>(val.GetValue<double>());
            result.SetValue(i, Value::BIGINT(epoch_sec * 1000000));
        }
        return;
    }

    if (name == "MAKE_TIMESTAMP") {
        // MAKE_TIMESTAMP(year,month,day,hour,minute,second) -> TIMESTAMP.
        // Previously emitted YYYYMMDD-style INT garbage; now properly
        // typed TIMESTAMP via civil_from_days + intra-day micros.
        if (expr.arguments.size() != 6) {
            throw NotImplementedException(
                "MAKE_TIMESTAMP requires 6 arguments: year, month, day, hour, minute, second");
        }
        std::vector<Vector> vecs;
        vecs.reserve(6);
        for (size_t a = 0; a < 6; a++) {
            vecs.emplace_back(expr.arguments[a]->GetReturnType(), count);
            Execute(*expr.arguments[a], input, vecs.back(), count);
        }
        auto last_day_of = [](int y, unsigned m) -> unsigned {
            static const unsigned dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (m == 2) {
                bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
                return leap ? 29u : 28u;
            }
            return dim[m - 1];
        };
        for (idx_t i = 0; i < count; i++) {
            Value vs[6];
            bool any_null = false;
            for (int a = 0; a < 6; a++) {
                vs[a] = vecs[a].GetValue(i);
                if (vs[a].IsNull()) { any_null = true; break; }
            }
            if (any_null) { result.GetValidity().SetInvalid(i); continue; }
            auto to_i32 = [](const Value &v) -> int32_t {
                return v.type().id() == LogicalTypeId::BIGINT
                    ? static_cast<int32_t>(v.GetValue<int64_t>())
                    : v.GetValue<int32_t>();
            };
            int32_t y = to_i32(vs[0]);
            int32_t mo = to_i32(vs[1]);
            int32_t d = to_i32(vs[2]);
            int32_t h = to_i32(vs[3]);
            int32_t mi = to_i32(vs[4]);
            // SECOND can be DOUBLE (sub-second fraction) per Postgres.
            double sec = (vs[5].type().id() == LogicalTypeId::DOUBLE ||
                          vs[5].type().id() == LogicalTypeId::FLOAT)
                ? vs[5].GetValue<double>()
                : static_cast<double>(to_i32(vs[5]));
            if (mo < 1 || mo > 12) {
                throw ConversionException("MAKE_TIMESTAMP: month out of range: " +
                                          std::to_string(mo));
            }
            unsigned max_d = last_day_of(y, static_cast<unsigned>(mo));
            if (d < 1 || static_cast<unsigned>(d) > max_d) {
                throw ConversionException("MAKE_TIMESTAMP: day " + std::to_string(d) +
                                          " out of range for " +
                                          std::to_string(y) + "-" + std::to_string(mo));
            }
            if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0.0 || sec >= 60.0) {
                throw ConversionException("MAKE_TIMESTAMP: time component out of range");
            }
            // civil_from_days.
            int y_adj = y - (mo <= 2 ? 1 : 0);
            int era = (y_adj >= 0 ? y_adj : y_adj - 399) / 400;
            unsigned yoe = static_cast<unsigned>(y_adj - era * 400);
            unsigned doy = (153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + d - 1;
            unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
            int64_t days = era * 146097LL + static_cast<int64_t>(doe) - 719468;
            int64_t intra_day_micros = (static_cast<int64_t>(h) * 3600LL +
                                        static_cast<int64_t>(mi) * 60LL) * 1000000LL +
                                       static_cast<int64_t>(sec * 1000000.0);
            int64_t micros = days * 86400LL * 1000000LL + intra_day_micros;
            result.SetValue(i, Value::TIMESTAMP(micros));
        }
        return;
    }

    if (name == "EPOCH_MS") {
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = val.GetValue<int64_t>();
            result.SetValue(i, Value::DOUBLE(static_cast<double>(micros) / 1000.0));
        }
        return;
    }

    if (name == "MONTHNAME" || name == "DAYNAME") {
        static const char* const month_names[] = {
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December"
        };
        static const char* const day_names[] = {
            "Sunday", "Monday", "Tuesday", "Wednesday",
            "Thursday", "Friday", "Saturday"
        };
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t micros = val.GetValue<int64_t>();
            auto seconds = micros / 1000000;
            auto time_t_val = static_cast<time_t>(seconds);
            struct tm tm_buf;
#ifdef _MSC_VER
            gmtime_s(&tm_buf, &time_t_val);
#else
            gmtime_r(&time_t_val, &tm_buf);
#endif
            if (name == "MONTHNAME") {
                int m = tm_buf.tm_mon;
                if (m < 0) m = 0; if (m > 11) m = 11;
                result.SetValue(i, Value::VARCHAR(month_names[m]));
            } else {
                int d = tm_buf.tm_wday;
                if (d < 0) d = 0; if (d > 6) d = 6;
                result.SetValue(i, Value::VARCHAR(day_names[d]));
            }
        }
        return;
    }

    if (name == "LAST_DAY") {
        // Return the DATE of the last day of the month. Accepts DATE
        // or TIMESTAMP input. Previously read everything as int64
        // micros, which gave nonsense (the same number for every
        // input) for DATE inputs.
        Vector arg(expr.arguments[0]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, arg, count);
        auto last_day_of = [](int y, unsigned m) -> unsigned {
            static const unsigned dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (m == 2) {
                bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
                return leap ? 29u : 28u;
            }
            return dim[m - 1];
        };
        for (idx_t i = 0; i < count; i++) {
            auto val = arg.GetValue(i);
            if (val.IsNull()) { result.GetValidity().SetInvalid(i); continue; }
            int64_t days_in;
            auto tid = val.type().id();
            if (tid == LogicalTypeId::DATE) {
                days_in = val.GetValue<int32_t>();
            } else if (tid == LogicalTypeId::TIMESTAMP || tid == LogicalTypeId::TIMESTAMP_TZ) {
                int64_t micros = val.GetValue<int64_t>();
                days_in = micros / (86400LL * 1000000LL);
                if (micros < 0 && (micros % (86400LL * 1000000LL)) != 0) days_in--;
            } else {
                // BIGINT input: treat as micros if magnitude warrants.
                int64_t raw = val.GetValue<int64_t>();
                int64_t abs_raw = raw < 0 ? -raw : raw;
                days_in = (abs_raw >= 10000000000000LL)
                    ? raw / (86400LL * 1000000LL)
                    : raw / 86400;
            }
            // Decompose days_in -> civil (year, month).
            int64_t z = days_in + 719468;
            int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            unsigned doe = static_cast<unsigned>(z - era * 146097);
            unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
            int yi = static_cast<int>(yoe) + static_cast<int>(era) * 400;
            unsigned dy = doe - (365*yoe + yoe/4 - yoe/100);
            unsigned mp = (5*dy + 2) / 153;
            unsigned m = mp < 10 ? mp + 3 : mp - 9;
            int year = yi + (m <= 2 ? 1 : 0);
            unsigned last_d = last_day_of(year, m);
            // Re-encode (year, month, last_d) back to days.
            int y_adj = year - (m <= 2 ? 1 : 0);
            int era2 = (y_adj >= 0 ? y_adj : y_adj - 399) / 400;
            unsigned yoe2 = static_cast<unsigned>(y_adj - era2 * 400);
            unsigned doy2 = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + last_d - 1;
            unsigned doe2 = yoe2 * 365 + yoe2/4 - yoe2/100 + doy2;
            int32_t result_days = static_cast<int32_t>(
                era2 * 146097LL + static_cast<int64_t>(doe2) - 719468);
            result.SetValue(i, Value::DATE(result_days));
        }
        return;
    }

    if (name == "MAKE_DATE") {
        // MAKE_DATE(year, month, day) -> DATE (epoch days). Previously
        // returned a YYYYMMDD INTEGER which broke EXTRACT and DATE
        // arithmetic — same bug that CURRENT_DATE had before 2dddb51.
        if (expr.arguments.size() != 3) {
            throw NotImplementedException("MAKE_DATE requires 3 arguments: year, month, day");
        }
        Vector y_vec(expr.arguments[0]->GetReturnType(), count);
        Vector m_vec(expr.arguments[1]->GetReturnType(), count);
        Vector d_vec(expr.arguments[2]->GetReturnType(), count);
        Execute(*expr.arguments[0], input, y_vec, count);
        Execute(*expr.arguments[1], input, m_vec, count);
        Execute(*expr.arguments[2], input, d_vec, count);
        auto last_day_of = [](int y, unsigned m) -> unsigned {
            static const unsigned dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (m == 2) {
                bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
                return leap ? 29u : 28u;
            }
            return dim[m - 1];
        };
        for (idx_t i = 0; i < count; i++) {
            auto yv = y_vec.GetValue(i);
            auto mv = m_vec.GetValue(i);
            auto dv = d_vec.GetValue(i);
            if (yv.IsNull() || mv.IsNull() || dv.IsNull()) {
                result.GetValidity().SetInvalid(i); continue;
            }
            int32_t y = yv.type().id() == LogicalTypeId::BIGINT
                ? static_cast<int32_t>(yv.GetValue<int64_t>()) : yv.GetValue<int32_t>();
            int32_t m = mv.type().id() == LogicalTypeId::BIGINT
                ? static_cast<int32_t>(mv.GetValue<int64_t>()) : mv.GetValue<int32_t>();
            int32_t d = dv.type().id() == LogicalTypeId::BIGINT
                ? static_cast<int32_t>(dv.GetValue<int64_t>()) : dv.GetValue<int32_t>();
            if (m < 1 || m > 12) {
                throw ConversionException("MAKE_DATE: month out of range: " +
                                          std::to_string(m));
            }
            unsigned max_d = last_day_of(y, static_cast<unsigned>(m));
            if (d < 1 || static_cast<unsigned>(d) > max_d) {
                throw ConversionException("MAKE_DATE: day " + std::to_string(d) +
                                          " out of range for " +
                                          std::to_string(y) + "-" + std::to_string(m));
            }
            // Hinnant civil_from_days.
            int y_adj = y - (m <= 2 ? 1 : 0);
            int era = (y_adj >= 0 ? y_adj : y_adj - 399) / 400;
            unsigned yoe = static_cast<unsigned>(y_adj - era * 400);
            unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
            unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
            int32_t days = static_cast<int32_t>(era * 146097LL +
                                                 static_cast<int64_t>(doe) - 719468);
            result.SetValue(i, Value::DATE(days));
        }
        return;
    }

    throw NotImplementedException("Function execution for: " + name);
}

// ============================================================================
// CAST execution
// ============================================================================

// Parse a complete integer from a string. Rejects trailing junk
// ('42abc' -> error), decimal fractions ('1.5' -> error), and SQL
// scientific notation ('1e3' -> reject as int but allow as double).
// Accepts surrounding whitespace + optional sign. Returns false on
// any parse failure so the caller can surface a clean error.
static bool ParseIntStrict(const std::string &s, int64_t &out) {
    if (s.empty()) return false;
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= s.size()) return false;
    size_t start = i;
    if (s[i] == '+' || s[i] == '-') i++;
    size_t digit_start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
    if (i == digit_start) return false;
    size_t digit_end = i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != s.size()) return false;
    try {
        out = std::stoll(s.substr(start, digit_end - start));
        return true;
    } catch (...) {
        return false;
    }
}

// Parse a complete double from a string. Rejects trailing junk and
// empty/whitespace-only input. Accepts SQL scientific notation.
static bool ParseDoubleStrict(const std::string &s, double &out) {
    if (s.empty()) return false;
    size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    try {
        size_t consumed = 0;
        out = std::stod(s.substr(first), &consumed);
        size_t end = first + consumed;
        while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) end++;
        return end == s.size();
    } catch (...) {
        return false;
    }
}

void ExpressionExecutor::ExecuteCast(const BoundCast &expr, DataChunk &input,
                                      Vector &result, idx_t count) {
    Vector child(expr.child->GetReturnType(), count);
    Execute(*expr.child, input, child, count);

    auto to_type = expr.GetReturnType().id();
    auto from_type = expr.child->GetReturnType().id();
    bool from_is_double = (from_type == LogicalTypeId::DOUBLE ||
                           from_type == LogicalTypeId::FLOAT);

    for (idx_t i = 0; i < count; i++) {
        auto val = child.GetValue(i);
        if (val.IsNull()) {
            result.GetValidity().SetInvalid(i);
            continue;
        }
        try {
            // Floating -> integer goes through the typed double, not
            // ToString (which renders 1e10 as "1e+10" and then
            // std::stoi truncates at 'e' producing 1).
            if (from_is_double && (to_type == LogicalTypeId::TINYINT ||
                                    to_type == LogicalTypeId::SMALLINT ||
                                    to_type == LogicalTypeId::INTEGER ||
                                    to_type == LogicalTypeId::BIGINT ||
                                    to_type == LogicalTypeId::UTINYINT ||
                                    to_type == LogicalTypeId::USMALLINT ||
                                    to_type == LogicalTypeId::UINTEGER ||
                                    to_type == LogicalTypeId::UBIGINT)) {
                double d = val.GetValue<double>();
                if (!std::isfinite(d) ||
                    d > static_cast<double>(std::numeric_limits<int64_t>::max()) ||
                    d < static_cast<double>(std::numeric_limits<int64_t>::min())) {
                    throw ConversionException("Value " + val.ToString() +
                        " is out of range for the destination type");
                }
                // Round half-to-even (banker's rounding) per PG/DuckDB
                // convention. Previously truncated toward zero — every
                // CAST(1.6 AS INT) produced 1 instead of 2.
                int64_t v = static_cast<int64_t>(std::nearbyint(d));
                switch (to_type) {
                case LogicalTypeId::TINYINT:
                    if (v < std::numeric_limits<int8_t>::min() || v > std::numeric_limits<int8_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for TINYINT");
                    result.SetValue(i, Value::TINYINT((int8_t)v)); break;
                case LogicalTypeId::SMALLINT:
                    if (v < std::numeric_limits<int16_t>::min() || v > std::numeric_limits<int16_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for SMALLINT");
                    result.SetValue(i, Value::SMALLINT((int16_t)v)); break;
                case LogicalTypeId::INTEGER:
                    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for INTEGER");
                    result.SetValue(i, Value::INTEGER((int32_t)v)); break;
                case LogicalTypeId::BIGINT:
                    result.SetValue(i, Value::BIGINT(v)); break;
                case LogicalTypeId::UTINYINT:
                    if (v < 0 || v > std::numeric_limits<uint8_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for UTINYINT");
                    result.SetValue(i, Value::UTINYINT((uint8_t)v)); break;
                case LogicalTypeId::USMALLINT:
                    if (v < 0 || v > std::numeric_limits<uint16_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for USMALLINT");
                    result.SetValue(i, Value::USMALLINT((uint16_t)v)); break;
                case LogicalTypeId::UINTEGER:
                    if (v < 0 || v > (int64_t)std::numeric_limits<uint32_t>::max())
                        throw ConversionException("Value " + val.ToString() + " out of range for UINTEGER");
                    result.SetValue(i, Value::UINTEGER((uint32_t)v)); break;
                case LogicalTypeId::UBIGINT:
                    if (v < 0)
                        throw ConversionException("Value " + val.ToString() + " is negative; cannot cast to UBIGINT");
                    result.SetValue(i, Value::UBIGINT((uint64_t)v)); break;
                default: break;
                }
                continue;
            }
            // Typed TIMESTAMP/TIMESTAMP_TZ -> DATE: truncate micros to
            // epoch days via floor-division. Previously the cast went
            // through Value::ToString() ("YYYY-MM-DD HH:MM:SS") which
            // the DATE parser rejected because it requires exactly
            // YYYY-MM-DD; non-TRY casts threw ConversionException and
            // TRY_CAST silently returned NULL.
            if ((from_type == LogicalTypeId::TIMESTAMP ||
                 from_type == LogicalTypeId::TIMESTAMP_TZ) &&
                to_type == LogicalTypeId::DATE) {
                int64_t micros = val.GetValue<int64_t>();
                const int64_t MPD = 86400LL * 1000000LL;
                int64_t days = micros / MPD;
                if (micros < 0 && (micros % MPD) != 0) days--;
                result.SetValue(i, Value::DATE(static_cast<int32_t>(days)));
                continue;
            }
            // Numeric -> BOOLEAN (Postgres/DuckDB rule): nonzero is true,
            // zero is false, NaN/Inf rejected (TRY_CAST -> NULL).
            // Previously the cast string-roundtripped: "5" was rejected
            // as not in the {true,false,t,f,1,0,yes,no} word list.
            if (to_type == LogicalTypeId::BOOLEAN &&
                (from_type == LogicalTypeId::TINYINT ||
                 from_type == LogicalTypeId::SMALLINT ||
                 from_type == LogicalTypeId::INTEGER ||
                 from_type == LogicalTypeId::BIGINT ||
                 from_type == LogicalTypeId::UTINYINT ||
                 from_type == LogicalTypeId::USMALLINT ||
                 from_type == LogicalTypeId::UINTEGER ||
                 from_type == LogicalTypeId::UBIGINT ||
                 from_is_double)) {
                if (from_is_double) {
                    double d = val.GetValue<double>();
                    if (!std::isfinite(d)) {
                        throw ConversionException(
                            "Cannot cast " + val.ToString() + " to BOOLEAN");
                    }
                    result.SetValue(i, Value::BOOLEAN(d != 0.0));
                } else if (from_type == LogicalTypeId::UTINYINT ||
                           from_type == LogicalTypeId::USMALLINT ||
                           from_type == LogicalTypeId::UINTEGER ||
                           from_type == LogicalTypeId::UBIGINT) {
                    uint64_t u = val.GetValue<uint64_t>();
                    result.SetValue(i, Value::BOOLEAN(u != 0));
                } else {
                    int64_t v;
                    switch (from_type) {
                    case LogicalTypeId::TINYINT:
                        v = val.GetValue<int8_t>(); break;
                    case LogicalTypeId::SMALLINT:
                        v = val.GetValue<int16_t>(); break;
                    case LogicalTypeId::INTEGER:
                        v = val.GetValue<int32_t>(); break;
                    default:
                        v = val.GetValue<int64_t>(); break;
                    }
                    result.SetValue(i, Value::BOOLEAN(v != 0));
                }
                continue;
            }
            auto str = val.ToString();
            switch (to_type) {
            case LogicalTypeId::TINYINT:
            case LogicalTypeId::SMALLINT:
            case LogicalTypeId::INTEGER:
            case LogicalTypeId::BIGINT:
            case LogicalTypeId::UTINYINT:
            case LogicalTypeId::USMALLINT:
            case LogicalTypeId::UINTEGER:
            case LogicalTypeId::UBIGINT: {
                int64_t v;
                if (!ParseIntStrict(str, v)) {
                    throw ConversionException("Could not convert string '" +
                        str + "' to " + expr.GetReturnType().ToString());
                }
                switch (to_type) {
                case LogicalTypeId::TINYINT:
                    if (v < std::numeric_limits<int8_t>::min() || v > std::numeric_limits<int8_t>::max())
                        throw ConversionException("Value " + str + " out of range for TINYINT");
                    result.SetValue(i, Value::TINYINT((int8_t)v)); break;
                case LogicalTypeId::SMALLINT:
                    if (v < std::numeric_limits<int16_t>::min() || v > std::numeric_limits<int16_t>::max())
                        throw ConversionException("Value " + str + " out of range for SMALLINT");
                    result.SetValue(i, Value::SMALLINT((int16_t)v)); break;
                case LogicalTypeId::INTEGER:
                    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
                        throw ConversionException("Value " + str + " out of range for INTEGER");
                    result.SetValue(i, Value::INTEGER((int32_t)v)); break;
                case LogicalTypeId::BIGINT:
                    result.SetValue(i, Value::BIGINT(v)); break;
                case LogicalTypeId::UTINYINT:
                    if (v < 0 || v > std::numeric_limits<uint8_t>::max())
                        throw ConversionException("Value " + str + " out of range for UTINYINT");
                    result.SetValue(i, Value::UTINYINT((uint8_t)v)); break;
                case LogicalTypeId::USMALLINT:
                    if (v < 0 || v > std::numeric_limits<uint16_t>::max())
                        throw ConversionException("Value " + str + " out of range for USMALLINT");
                    result.SetValue(i, Value::USMALLINT((uint16_t)v)); break;
                case LogicalTypeId::UINTEGER:
                    if (v < 0 || v > (int64_t)std::numeric_limits<uint32_t>::max())
                        throw ConversionException("Value " + str + " out of range for UINTEGER");
                    result.SetValue(i, Value::UINTEGER((uint32_t)v)); break;
                case LogicalTypeId::UBIGINT:
                    if (v < 0)
                        throw ConversionException("Value " + str + " is negative; cannot cast to UBIGINT");
                    result.SetValue(i, Value::UBIGINT((uint64_t)v)); break;
                default: break;
                }
                break;
            }
            case LogicalTypeId::DOUBLE: {
                double d;
                if (!ParseDoubleStrict(str, d)) {
                    throw ConversionException("Could not convert string '" +
                        str + "' to DOUBLE");
                }
                result.SetValue(i, Value::DOUBLE(d)); break;
            }
            case LogicalTypeId::FLOAT: {
                double d;
                if (!ParseDoubleStrict(str, d)) {
                    throw ConversionException("Could not convert string '" +
                        str + "' to FLOAT");
                }
                result.SetValue(i, Value::FLOAT((float)d)); break;
            }
            case LogicalTypeId::VARCHAR:
                result.SetValue(i, Value::VARCHAR(str)); break;
            case LogicalTypeId::DATE: {
                int32_t days;
                if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                    throw ConversionException(
                        "Could not convert string '" + str + "' to DATE");
                }
                result.SetValue(i, Value::DATE(days));
                break;
            }
            case LogicalTypeId::TIMESTAMP:
            case LogicalTypeId::TIMESTAMP_TZ: {
                int64_t micros;
                if (!Value::TryParseTimestampMicros(str.data(), str.size(), micros)) {
                    // Fall back to DATE-only string (midnight TIMESTAMP).
                    int32_t days;
                    if (!Value::TryParseDateStringEpochDays(str.data(), str.size(), days)) {
                        throw ConversionException(
                            "Could not convert string '" + str + "' to TIMESTAMP");
                    }
                    micros = static_cast<int64_t>(days) * 86400LL * 1000000LL;
                }
                result.SetValue(i, Value::TIMESTAMP(micros));
                break;
            }
            case LogicalTypeId::BOOLEAN: {
                // Case-insensitive bool parse matching DuckDB/Postgres:
                // 't', 'true', 'yes', 'y', '1' -> true
                // 'f', 'false', 'no', 'n', '0' -> false
                // anything else -> error (not silently false)
                std::string lower;
                lower.reserve(str.size());
                for (char c : str) {
                    lower += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                }
                if (lower == "true" || lower == "t" || lower == "yes" ||
                    lower == "y" || lower == "1") {
                    result.SetValue(i, Value::BOOLEAN(true));
                } else if (lower == "false" || lower == "f" || lower == "no" ||
                           lower == "n" || lower == "0") {
                    result.SetValue(i, Value::BOOLEAN(false));
                } else {
                    throw ConversionException("Could not convert string '" +
                        str + "' to BOOLEAN");
                }
                break;
            }
            default:
                result.SetValue(i, Value::VARCHAR(str)); break;
            }
        } catch (...) {
            if (expr.is_try) {
                result.GetValidity().SetInvalid(i); // TRY_CAST returns NULL
            } else {
                throw ConversionException("Cannot cast '" + val.ToString() + "' to " +
                                           expr.GetReturnType().ToString());
            }
        }
    }
}

// Static member.
Catalog *ExpressionExecutor::catalog_ = nullptr;

// ============================================================================
// Subquery execution
// ============================================================================

void ExpressionExecutor::ExecuteSubquery(const BoundSubqueryExpression &expr,
                                          DataChunk &input, Vector &result, idx_t count) {
    if (!catalog_) {
        throw InternalException("Catalog not set for subquery execution");
    }

    auto *parsed_query = static_cast<SelectStatement *>(expr.parsed_query.get());
    if (!parsed_query) {
        throw InternalException("Subquery has no parsed query");
    }

    // Bind and execute the subquery.
    Binder binder(*catalog_);
    auto bound = binder.Bind(*parsed_query);
    auto logical = Planner::Plan(*bound);
    PhysicalPlanner pp(*catalog_);
    auto physical = pp.Plan(*logical);
    physical->Init();

    // Collect subquery results.
    std::vector<std::vector<Value>> sub_rows;
    DataChunk chunk;
    while (true) {
        if (!physical->GetData(chunk)) break;
        for (idx_t i = 0; i < chunk.size(); i++) {
            std::vector<Value> row;
            for (idx_t c = 0; c < chunk.ColumnCount(); c++)
                row.push_back(chunk.GetValue(c, i));
            sub_rows.push_back(std::move(row));
        }
    }

    auto *out = result.GetData<bool>();

    switch (expr.subtype) {
    case BoundSubqueryExpression::Type::EXISTS:
        for (idx_t i = 0; i < count; i++) {
            out[i] = !sub_rows.empty();
        }
        break;

    case BoundSubqueryExpression::Type::NOT_EXISTS:
        for (idx_t i = 0; i < count; i++) {
            out[i] = sub_rows.empty();
        }
        break;

    case BoundSubqueryExpression::Type::IN_SUBQUERY: {
        // SQL three-valued logic for IN (SELECT ...):
        //   NULL IN (anything)              -> NULL
        //   v   IN (..., v, ...)             -> TRUE
        //   v   IN (...) with NULL in subq   -> NULL (when no match)
        //   v   IN (...) with no NULL, no match -> FALSE
        // Previously this used row[0].ToString() == val.ToString()
        // which incorrectly matched NULLs against the string "NULL"
        // and never propagated UNKNOWN, breaking NOT IN with a NULL
        // subquery row (returned wrong rows).
        if (expr.child) {
            Vector child_vec(expr.child->GetReturnType(), count);
            Execute(*expr.child, input, child_vec, count);
            for (idx_t i = 0; i < count; i++) {
                auto val = child_vec.GetValue(i);
                if (val.IsNull()) {
                    result.GetValidity().SetInvalid(i);
                    out[i] = false;
                    continue;
                }
                bool found = false;
                bool saw_null = false;
                for (auto &row : sub_rows) {
                    if (row.empty()) continue;
                    if (row[0].IsNull()) { saw_null = true; continue; }
                    if (row[0] == val) { found = true; break; }
                }
                if (found) {
                    out[i] = true;
                } else if (saw_null) {
                    result.GetValidity().SetInvalid(i);
                    out[i] = false;
                } else {
                    out[i] = false;
                }
            }
        }
        break;
    }

    case BoundSubqueryExpression::Type::SCALAR:
        // SQL standard: scalar subquery must produce at most one row.
        // More than one row -> runtime cardinality error. Zero rows
        // -> NULL.
        if (sub_rows.size() > 1) {
            throw InvalidInputException(
                "Scalar subquery returned more than one row");
        }
        for (idx_t i = 0; i < count; i++) {
            if (!sub_rows.empty() && !sub_rows[0].empty()) {
                result.SetValue(i, sub_rows[0][0]);
            } else {
                result.GetValidity().SetInvalid(i);
            }
        }
        break;
    }
}

} // namespace slothdb
