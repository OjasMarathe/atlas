#ifndef ATLAS_DFS_CHUNKING_H_
#define ATLAS_DFS_CHUNKING_H_

#include <cstddef>
#include <string>
#include <vector>

namespace atlas {

// Atlas splits files into fixed-size, content-addressed chunks. See docs/concepts/chunking.md.
inline constexpr size_t kChunkSize = 4 * 1024 * 1024;  // 4 MiB

struct Chunk {
  std::string id;    // hex SHA-256 of `data` — the content address
  std::string data;  // the chunk bytes (<= kChunkSize; the last chunk may be smaller)
};

// Split `bytes` into chunks of `chunk_size` (the last may be smaller). Each chunk's id is the
// SHA-256 of its bytes, so identical content always yields the same chunk id (dedup for free).
std::vector<Chunk> ChunkBytes(const std::string& bytes, size_t chunk_size = kChunkSize);

// Concatenate chunk data (in order) back into the original bytes.
std::string Reassemble(const std::vector<Chunk>& chunks);

}  // namespace atlas

#endif  // ATLAS_DFS_CHUNKING_H_
