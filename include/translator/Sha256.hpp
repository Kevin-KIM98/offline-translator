// SHA-256 (FIPS 180-4) — dependency-free streaming implementation.
// Used by ModelManager to verify model file integrity without OpenSSL.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace translator {

class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }

    // Finalizes and returns the 32-byte digest. Call reset() before reusing the object.
    std::array<std::uint8_t, 32> digest();

    // Lower-case hexadecimal string of the digest.
    std::string hexDigest();

    void reset();

    // Convenience helpers -----------------------------------------------------
    static std::string hashString(const std::string& s);

    // Streams a file from disk in 1 MiB chunks. Returns an empty string on I/O error
    // or when `progress` returns false (abort). progress(bytes_done, bytes_total).
    using ProgressFn = std::function<bool(std::uint64_t, std::uint64_t)>;
    static std::string hashFile(const std::string& path, const ProgressFn& progress = nullptr);

    static std::string toHex(const std::array<std::uint8_t, 32>& d);
    static bool equalsIgnoreCase(const std::string& a, const std::string& b);

private:
    void transform(const std::uint8_t block[64]);

    std::uint32_t state_[8];
    std::uint64_t bitLength_;
    std::uint8_t buffer_[64];
    std::size_t bufferLen_;
    bool finalized_;
};

} // namespace translator
