#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>
#include <cstdint>

struct Posting {
    std::string doc_id;
    size_t term_frequency{0};
};

// Результат пошуку для повернення
struct SearchResult {
    std::string doc_id;
    double score{0.0};
    std::string snippet; // Короткий контекстний уривок тексту зі знайденим словом
};

// Інвертований індекс на основі шардованої хеш-таблиці
class InvertedIndex {
public:
    // За замовчуванням створює 256 шардових секцій
    explicit InvertedIndex(size_t num_shards = 256);

    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;

    // Додавання слова з документа до індексу
    void add(const std::string& term, const std::string& doc_id, size_t count = 1);

    // Отримання списку документів за словом
    std::vector<Posting> get_postings(const std::string& term) const;
    void clear();

    // Загальна кількість проіндексованих унікальних слів по всіх шардах
    size_t total_terms() const;

private:
    struct Shard {
        mutable std::shared_mutex mutex; // Замок Readers-Writer
        std::unordered_map<std::string, std::unordered_map<std::string, size_t>> table;

    };

    size_t num_shards_;
    std::vector<std::unique_ptr<Shard>> shards_;

    // Обчислення індексу шарда за хешем слова
    size_t get_shard_index(const std::string& term) const;
};