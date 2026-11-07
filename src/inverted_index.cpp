#include "inverted_index.h"
#include <algorithm>

InvertedIndex::InvertedIndex(size_t num_shards) : num_shards_(num_shards) {
    shards_.reserve(num_shards_);
    for (size_t i = 0; i < num_shards_; ++i) {
        shards_.emplace_back(std::make_unique<Shard>());
    }
}

size_t InvertedIndex::get_shard_index(const std::string& term) const {
    return std::hash<std::string>{}(term) % num_shards_;
}

void InvertedIndex::add(const std::string& term, const std::string& doc_id, size_t count) {
    if (term.empty() || doc_id.empty() || count == 0) {
        return;
    }

    size_t shard_id = get_shard_index(term);
    Shard& shard = *shards_[shard_id];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.table[term][doc_id] += count;
}

std::vector<Posting> InvertedIndex::get_postings(const std::string& term) const {
    if (term.empty()) {
        return {};
    }

    size_t shard_id = get_shard_index(term);
    const Shard& shard = *shards_[shard_id];

    // shared_lock дозволяє паралельний доступ іншим читачам без блокування
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.table.find(term);
    if (it == shard.table.end()) {
        return {};
    }

    std::vector<Posting> results;
    results.reserve(it->second.size());
    for (const auto& [doc, freq] : it->second) {
        results.push_back({doc, freq});
    }
    return results;
}

void InvertedIndex::clear() {
    for (auto& shard : shards_) {
        std::unique_lock<std::shared_mutex> lock(shard->mutex);
        shard->table.clear();
    }
}

// Підрахунок загальної кількості унікальних слів блокує шарди в режимі читання
size_t InvertedIndex::total_terms() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock<std::shared_mutex> lock(shard->mutex);
        total += shard->table.size();
    }
    return total;
}