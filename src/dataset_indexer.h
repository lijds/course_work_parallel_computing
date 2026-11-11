#pragma once 

#include <string>
#include <vector>
#include <chrono>
#include "inverted_index.h"
#include "thread_pool.h"

struct IndexingStats {
    size_t documents_indexed = 0;
    size_t total_terms{0};
    uint64_t time_ms{0};
    size_t threads_used{0};
};

class DatasetIndexer {
public:
    static IndexingStats index_directory(const std::string& directory_path, InvertedIndex& index, size_t num_threads);
private:
    static std::vector<std::string> tokenize(const std::string& text);

    static void index_single_file(const std::string& file_path, InvertedIndex& index);
};