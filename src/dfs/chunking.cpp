#include "dfs/chunking.h"

#include <algorithm>
#include <utility>

#include "common/sha256.h"

namespace atlas {

std::vector<Chunk> ChunkBytes(const std::string& bytes, size_t chunk_size) {
  std::vector<Chunk> chunks;
  if (chunk_size == 0) return chunks;
  for (size_t off = 0; off < bytes.size(); off += chunk_size) {
    const size_t n = std::min(chunk_size, bytes.size() - off);
    Chunk c;
    c.data = bytes.substr(off, n);
    c.id = Sha256Hex(c.data);
    chunks.push_back(std::move(c));
  }
  return chunks;
}

std::string Reassemble(const std::vector<Chunk>& chunks) {
  std::string out;
  for (const auto& c : chunks) out += c.data;
  return out;
}

}  // namespace atlas
