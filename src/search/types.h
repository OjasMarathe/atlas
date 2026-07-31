#pragma once

#include <cstdint>

namespace atlas::search {

// Dense, shard-local document id. The externally visible identity is the file_id string; DocId
// exists so posting lists hold small sortable integers (and so ADR-0007's delta encoding has
// ascending values to take gaps between).
using DocId = std::uint32_t;

}  // namespace atlas::search
