#ifndef ATLAS_COMMON_SHA256_H_
#define ATLAS_COMMON_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace atlas {

// Streaming SHA-256 (FIPS 180-4), implemented from scratch.
// See docs/concepts/sha256-checksums.md for how it works and why we hand-roll it.
class Sha256 {
 public:
  Sha256();

  void Update(const void* data, size_t len);

  // Finalizes and returns the 32-byte digest as 64 lowercase hex chars.
  // Consumes the state — construct a fresh Sha256 for the next message.
  std::string HexDigest();

 private:
  void Transform(const uint8_t block[64]);

  std::array<uint32_t, 8> h_;
  uint8_t buffer_[64];
  size_t buffer_len_ = 0;
  uint64_t total_len_ = 0;  // message length in bytes
};

// One-shot convenience wrappers.
std::string Sha256Hex(const void* data, size_t len);
std::string Sha256Hex(const std::string& data);

}  // namespace atlas

#endif  // ATLAS_COMMON_SHA256_H_
