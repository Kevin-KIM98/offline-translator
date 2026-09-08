#include "translator/Sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace translator {

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
inline std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (~x & z); }
inline std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline std::uint32_t bsig0(std::uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline std::uint32_t bsig1(std::uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline std::uint32_t ssig0(std::uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline std::uint32_t ssig1(std::uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

} // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85; state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c; state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
    bitLength_ = 0;
    bufferLen_ = 0;
    finalized_ = false;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::transform(const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + bsig1(e) + ch(e, f, g) + K[i] + w[i];
        const std::uint32_t t2 = bsig0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bitLength_ += static_cast<std::uint64_t>(len) * 8;

    if (bufferLen_ > 0) {
        const std::size_t take = std::min(len, std::size_t(64) - bufferLen_);
        std::memcpy(buffer_ + bufferLen_, p, take);
        bufferLen_ += take;
        p += take;
        len -= take;
        if (bufferLen_ == 64) {
            transform(buffer_);
            bufferLen_ = 0;
        }
    }
    while (len >= 64) {
        transform(p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(buffer_, p, len);
        bufferLen_ = len;
    }
}

std::array<std::uint8_t, 32> Sha256::digest() {
    if (!finalized_) {
        const std::uint64_t bits = bitLength_;
        // Padding: 0x80, zeros until length % 64 == 56, then 64-bit big-endian bit length.
        std::uint8_t block[64];
        std::memcpy(block, buffer_, bufferLen_);
        std::size_t n = bufferLen_;
        block[n++] = 0x80;
        if (n > 56) {
            std::memset(block + n, 0, 64 - n);
            transform(block);
            n = 0;
        }
        std::memset(block + n, 0, 56 - n);
        for (int i = 0; i < 8; ++i) block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
        transform(block);
        bufferLen_ = 0;
        finalized_ = true;
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<std::uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return out;
}

std::string Sha256::toHex(const std::array<std::uint8_t, 32>& d) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (auto b : d) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xF]);
    }
    return s;
}

std::string Sha256::hexDigest() { return toHex(digest()); }

std::string Sha256::hashString(const std::string& s) {
    Sha256 h;
    h.update(s);
    return h.hexDigest();
}

std::string Sha256::hashFile(const std::string& path, const ProgressFn& progress) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};

    std::uint64_t total = 0;
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    total = static_cast<std::uint64_t>(_ftelli64(f));
    _fseeki64(f, 0, SEEK_SET);
#else
    std::fseek(f, 0, SEEK_END);
    total = static_cast<std::uint64_t>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
#endif

    Sha256 h;
    std::vector<std::uint8_t> buf(1 << 20); // 1 MiB chunks
    std::uint64_t done = 0;
    while (true) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        h.update(buf.data(), n);
        done += n;
        if (progress && !progress(done, total)) {
            std::fclose(f);
            return {};
        }
    }
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return {};
    return h.hexDigest();
}

bool Sha256::equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

} // namespace translator
