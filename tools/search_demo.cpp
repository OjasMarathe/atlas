// Phase 3a demo / Definition-of-Done check.
//
//   ./build/atlas_search_demo <corpus-dir> [query ...]
//
// Indexes a directory of documents into one shard, runs each query, and prints BM25-ranked
// results with timings — the "index the corpus, keyword query returns the right documents on
// top, sub-second" bar from docs/plan/roadmap.md.

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "search/corpus.h"
#include "search/search_engine.h"

namespace {

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: atlas_search_demo <corpus-dir> [query ...]\n";
    return 2;
  }

  const std::string corpus_dir = argv[1];
  std::vector<std::string> queries(argv + 2, argv + argc);
  if (queries.empty()) {
    // Note the explicit AND before NOT: with the implicit operator being OR, "chunk NOT search"
    // would parse as "chunk OR (NOT search)", which excludes nothing.
    queries = {"consistent hashing ring", "replication AND checksum", "bm25 ranking",
               "chunk AND NOT search"};
  }

  const auto load_start = Clock::now();
  const std::vector<atlas::search::Document> documents = atlas::search::LoadCorpus(corpus_dir);
  if (documents.empty()) {
    std::cerr << "no .md/.txt documents found under " << corpus_dir << "\n";
    return 1;
  }
  const double load_ms = MillisecondsSince(load_start);

  atlas::search::SearchEngine engine;
  const auto index_start = Clock::now();
  for (const auto& document : documents) {
    engine.IndexDocument(document.file_id, document.text);
  }
  const double index_ms = MillisecondsSince(index_start);

  const atlas::search::ShardStatistics stats = engine.Stats();
  std::cout << "corpus: " << corpus_dir << "\n"
            << "  documents   : " << stats.document_count << " (read in " << std::fixed
            << std::setprecision(1) << load_ms << " ms)\n"
            << "  unique terms: " << stats.unique_terms << "\n"
            << "  avg doc len : " << std::setprecision(1) << stats.average_document_length
            << " terms\n"
            << "  indexed in  : " << std::setprecision(1) << index_ms << " ms\n";

  for (const std::string& query : queries) {
    std::string error;
    const auto search_start = Clock::now();
    const std::vector<atlas::search::SearchHit> hits = engine.Search(query, 5, &error);
    const double search_ms = MillisecondsSince(search_start);

    std::cout << "\nquery: \"" << query << "\"  (" << std::setprecision(3) << search_ms << " ms)\n";
    if (!error.empty()) {
      std::cout << "  parse error: " << error << "\n";
      continue;
    }
    if (hits.empty()) {
      std::cout << "  (no matches)\n";
      continue;
    }
    for (std::size_t i = 0; i < hits.size(); ++i) {
      std::cout << "  " << (i + 1) << ". " << std::setw(8) << std::setprecision(3) << hits[i].score
                << "  " << hits[i].file_id << "\n";
    }
  }
  return 0;
}
