#include <iostream>
#include <cstdlib>
#include <string>
#include <sstream>
#include "httplib.h"
#include "inverted_index.h"
#include "dataset_indexer.h"

int main() {
    const char* env_path = std::getenv("DATASET_PATH");
    std::string dataset_path = env_path ? env_path : "./data";
    size_t default_threads = 6;

    std::cout << "Initializing Inverted Index (256 shards)..." << std::endl;
    InvertedIndex index(256);

    std::cout << "Pre-indexing dataset from: " << dataset_path << "with" << default_threads << " threads..." << std::endl;

    IndexingStats stats = DatasetIndexer::index_directory(dataset_path, index, default_threads);

    std::cout << "Dataset indexed: " << stats.documents_indexed << " docs, " << stats.total_terms << " terms in " << stats.time_ms << " ms." << std::endl;

    httplib::Server svr;

    // GET /api/health
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status": "ok","ollama":false})", "application/json");
    });

    // GET /api/stats
    svr.Get("/api/stats", [&stats](const httplib::Request&, httplib::Response& res) {
        std::stringstream ss;
        ss << "{"
           << R"("documents":)" << stats.documents_indexed << ","
           << R"("index_build_time_ms":)" << stats.time_ms << ","
           << R"("threads_used":)" << stats.threads_used
           << "}";
        res.set_content(ss.str(), "application/json");
    });

    std::cout << "Search server listening on 0.0.0.0:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);

    return 0;
}