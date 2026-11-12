#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <filesystem>

#include "httplib.h"
#include "json.hpp"
#include "inverted_index.h"
#include "dataset_indexer.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// Розбиваємо текст запиту на окремі слова
std::vector<std::string> tokenize_query(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;

    for (unsigned char c : text) {
        if (std::isalpha(c)) {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else {
            if (current.size() >= 2 && current.size() <= 30) {
                tokens.push_back(current);
            }
            current.clear();
        }
    }

    if (current.size() >= 2 && current.size() <= 30) {
        tokens.push_back(current);
    }
    return tokens;
}

// Шукаємо перший точний збіг слова у файлі та вирізаємо шматок тексту навколо нього.
// Читаємо порціями по 16 КБ, щоб не вантажити весь файл у пам'ять одразу.
std::string extract_snippet(const std::string& dataset_path,
                            const std::string& doc_id,
                            const std::vector<std::string>& terms) {
    fs::path file_path = fs::path(dataset_path) / doc_id;
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    const size_t CHUNK_SIZE = 16384; // 16 КБ за одне читання
    const size_t OVERLAP = 64;      // залишаємо хвіст попереднього блоку, щоб не розрізати слово на стику
    std::string buffer;
    std::string chunk(CHUNK_SIZE, '\0');
    std::string fallback_snippet;
    bool is_first_chunk = true;

    while (file) {
        file.read(&chunk[0], CHUNK_SIZE);
        size_t bytes_read = static_cast<size_t>(file.gcount());
        if (bytes_read == 0) break;

        buffer.append(chunk, 0, bytes_read);

        // Запам'ятовуємо початок файлу про всяк випадок, якщо слово не знайдеться
        if (is_first_chunk) {
            fallback_snippet = buffer.substr(0, std::min<size_t>(160, buffer.size()));
            is_first_chunk = false;
        }

        std::string lower_buf = buffer;
        std::transform(lower_buf.begin(), lower_buf.end(), lower_buf.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        size_t best_pos = std::string::npos;
        for (const auto& term : terms) {
            size_t pos = 0;
            while ((pos = lower_buf.find(term, pos)) != std::string::npos) {
                // Перевіряємо, що це окреме слово, а не частина іншого (напр. "war" у "forward")
                bool left_ok = (pos == 0) || !std::isalpha(static_cast<unsigned char>(lower_buf[pos - 1]));
                bool right_ok = (pos + term.size() >= lower_buf.size()) ||
                                !std::isalpha(static_cast<unsigned char>(lower_buf[pos + term.size()]));

                if (left_ok && right_ok) {
                    if (best_pos == std::string::npos || pos < best_pos) {
                        best_pos = pos;
                    }
                    break;
                }
                pos += term.size();
            }
        }

        // Якщо знайшли слово і після нього ще є трохи тексту для контексту — формуємо сніпет і виходимо
        if (best_pos != std::string::npos && (best_pos + 110 <= buffer.size() || file.eof())) {
            size_t start = (best_pos > 50) ? (best_pos - 50) : 0;
            size_t len = std::min<size_t>(160, buffer.size() - start);
            std::string snippet = buffer.substr(start, len);

            // Прибираємо переноси рядків і табуляцію, щоб JSON виглядав охайно
            for (char& c : snippet) {
                if (c == '\r' || c == '\n' || c == '\t') {
                    c = ' ';
                }
            }
            return (start > 0 ? "..." : "") + snippet +
                   (start + len < buffer.size() || !file.eof() ? "..." : "");
        }

        // Відкидаємо вже перевірений текст, залишаючи тільки перекриття
        if (buffer.size() > OVERLAP) {
            buffer.erase(0, buffer.size() - OVERLAP);
        }
    }

    for (char& c : fallback_snippet) {
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
    }
    return fallback_snippet + "...";
}

int main() {
    const char* env_path = std::getenv("DATASET_PATH");
    std::string dataset_path = env_path ? env_path : "./data";
    size_t default_threads = 6;

    // Створюємо інвертований індекс на 256 шардів 
    std::cout << "Initializing Inverted Index (256 shards)..." << std::endl;
    InvertedIndex index(256);

    // Одразу індексуємо датасет при запуску сервера
    std::cout << "Pre-indexing dataset from: " << dataset_path
              << " with " << default_threads << " threads..." << std::endl;
    IndexingStats current_stats = DatasetIndexer::index_directory(dataset_path, index, default_threads);
    std::mutex stats_mutex; // М'ютекс для безпечного оновлення статистики та переіндексації

    std::cout << "Dataset indexed: " << current_stats.documents_indexed << " docs, "
              << current_stats.total_terms << " terms in " << current_stats.time_ms << " ms." << std::endl;

    httplib::Server svr;

    // Перевірка, що сервер живий 
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status": "ok", "ollama": false})", "application/json");
    });

    // Статистика останньої побудови індексу
    svr.Get("/api/stats", [&current_stats, &stats_mutex](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        json j = {
            {"documents", current_stats.documents_indexed},
            {"index_build_time_ms", current_stats.time_ms},
            {"threads_used", current_stats.threads_used}
        };
        res.set_content(j.dump(), "application/json");
    });

    // Перебудова індексу з нуля на вказаній кількості потоків
    svr.Post("/api/index", [&index, &dataset_path, &current_stats, &stats_mutex](const httplib::Request& req, httplib::Response& res) {
        size_t threads = 4;
        try {
            if (!req.body.empty()) {
                auto body = json::parse(req.body);
                if (body.contains("threads") && body["threads"].is_number_unsigned()) {
                    threads = body["threads"].get<size_t>();
                    if (threads == 0) threads = 1;
                }
            }
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid JSON body"}}).dump(), "application/json");
            return;
        }

        // Блокуємо, щоб два запити одночасно не почали чистити й перебудовувати індекс
        std::lock_guard<std::mutex> lock(stats_mutex);
        index.clear();
        current_stats = DatasetIndexer::index_directory(dataset_path, index, threads);

        json j = {
            {"status", "ok"},
            {"documents_indexed", current_stats.documents_indexed},
            {"time_ms", current_stats.time_ms}
        };
        res.set_content(j.dump(), "application/json");
    });

    // Пошук за ключовими словами
    svr.Post("/api/search", [&index, &dataset_path](const httplib::Request& req, httplib::Response& res) {
        auto start_time = std::chrono::high_resolution_clock::now();

        std::string query_str;
        std::string mode = "keyword";

        try {
            auto body = json::parse(req.body);
            if (!body.contains("query") || !body["query"].is_string()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing 'query' string field"}}).dump(), "application/json");
                return;
            }
            query_str = body["query"].get<std::string>();
            if (body.contains("mode") && body["mode"].is_string()) {
                mode = body["mode"].get<std::string>();
            }
        } catch (const std::exception&) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid JSON body"}}).dump(), "application/json");
            return;
        }

        // Розбиваємо запит на слова і сумуємо частоту їх появи в документах
        std::vector<std::string> terms = tokenize_query(query_str);
        std::unordered_map<std::string, double> doc_scores;

        for (const auto& term : terms) {
            auto postings = index.get_postings(term);
            for (const auto& p : postings) {
                doc_scores[p.doc_id] += static_cast<double>(p.term_frequency);
            }
        }

        // Перекладаємо у вектор і сортуємо за релевантністю (від більшого до меншого)
        std::vector<SearchResult> results;
        results.reserve(doc_scores.size());
        for (const auto& [doc_id, score] : doc_scores) {
            results.push_back({doc_id, score, ""});
        }

        std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
            return a.score > b.score;
        });

        // Беремо тільки топ-10 результатів і витягуємо для них сніпети
        if (results.size() > 10) {
            results.resize(10);
        }

        json results_json = json::array();
        for (auto& r : results) {
            r.snippet = extract_snippet(dataset_path, r.doc_id, terms);
            results_json.push_back({
                {"doc_id", r.doc_id},
                {"score", r.score},
                {"snippet", r.snippet}
            });
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        json response = {
            {"results", results_json},
            {"time_ms", elapsed_ms}
        };
        res.set_content(response.dump(), "application/json");
    });

    std::cout << "Search server listening on 0.0.0.0:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);

    return 0;
}