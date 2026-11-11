#include "dataset_indexer.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <unordered_map>

namespace fs = std::filesystem;

std::vector<std::string> DatasetIndexer::tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string word;
    word.reserve(32);

    for (char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (word.length() >= 2 && word.length() <= 30) {
                tokens.push_back(word);
            }
            word.clear();
        }
    }
    if (word.length() >= 2 && word.length() <= 30) {
        tokens.push_back(word);
    }
    return tokens;
}

void DatasetIndexer::index_single_file(const std::string& file_path, InvertedIndex& index) {
    std::ifstream file(file_path);
    if(!file.is_open()) {
        return;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto words = tokenize(content);

    // Локальний підрахунок слів для конкретного документа,
    std::unordered_map<std::string, size_t> local_counts;
    for (const auto& w : words) {
        local_counts[w]++;
    }

    std::string doc_id = fs::path(file_path).filename().string();

    for (const auto& [term, count] : local_counts) {
        index.add(term, doc_id, count);
    }
}

IndexingStats DatasetIndexer::index_directory(const std::string& directory_path, InvertedIndex& index, size_t num_threads) {
    IndexingStats stats;
    stats.threads_used = num_threads;

    std::vector<std::string> files;
    if (fs::exists(directory_path) && fs::is_directory(directory_path)) {
        for (const auto& entry : fs::recursive_directory_iterator(directory_path)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path().string());
            }
        }
    } else {
        std::cerr << "Directory not found: " << directory_path << std::endl;
        return stats;
    }

    stats.documents_indexed = files.size();

    auto start_time = std::chrono::high_resolution_clock::now();

    {
        ThreadPool pool(num_threads);

        for (const auto& file_path : files) {
            pool.enqueue([file_path, &index]() {
                index_single_file(file_path, index);
            });
        }

        pool.wait_done();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    stats.total_terms = index.total_terms();

    return stats;
}