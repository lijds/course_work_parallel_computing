#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <thread>
#include <filesystem>

#include "httplib.h"
#include "json.hpp"
#include "inverted_index.h"
#include "dataset_indexer.h"
#include "ws_server.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// Розбиваємо текст запиту на окремі слова за тими ж правилами, що й при індексації
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

// Потоково вирізаємо уривок тексту навколо першого точного збігу слова
std::string extract_snippet(const std::string& dataset_path,
                            const std::string& doc_id,
                            const std::vector<std::string>& terms) {
    fs::path file_path = fs::path(dataset_path) / doc_id;
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    const size_t CHUNK_SIZE = 16384;
    const size_t OVERLAP = 64;
    std::string buffer;
    std::string chunk(CHUNK_SIZE, '\0');
    std::string fallback_snippet;
    bool is_first_chunk = true;

    while (file) {
        file.read(&chunk[0], CHUNK_SIZE);
        size_t bytes_read = static_cast<size_t>(file.gcount());
        if (bytes_read == 0) break;

        buffer.append(chunk, 0, bytes_read);

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

        if (best_pos != std::string::npos && (best_pos + 110 <= buffer.size() || file.eof())) {
            size_t start = (best_pos > 50) ? (best_pos - 50) : 0;
            size_t len = std::min<size_t>(160, buffer.size() - start);
            std::string snippet = buffer.substr(start, len);

            for (char& c : snippet) {
                if (c == '\r' || c == '\n' || c == '\t') {
                    c = ' ';
                }
            }
            return (start > 0 ? "..." : "") + snippet +
                   (start + len < buffer.size() || !file.eof() ? "..." : "");
        }

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

// Перевірка доступності сервісу Ollama
bool check_ollama_health(const std::string& ollama_host) {
    httplib::Client cli(ollama_host);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(1, 0);
    auto res = cli.Get("/api/tags");
    return res && res->status == 200;
}

// Розширення запиту синонімами через Ollama (/api/generate) із чергою на м'ютексі
std::vector<std::string> expand_query_with_ollama(const std::string& query,
                                                   const std::string& ollama_host,
                                                   const std::string& ollama_model,
                                                   std::mutex& ollama_mutex) {
    std::vector<std::string> terms = tokenize_query(query);
    std::unordered_set<std::string> unique_terms(terms.begin(), terms.end());

    std::lock_guard<std::mutex> lock(ollama_mutex);

    httplib::Client cli(ollama_host);
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(15, 0);

    std::string prompt =
        "Generate 5 single-word English search synonyms or closely related keywords for the query: \"" +
        query + "\". Return ONLY comma-separated single words, no explanations, no numbering.";

    json req_body = {
        {"model", ollama_model},
        {"prompt", prompt},
        {"stream", false},
        {"options", {
            {"temperature", 0.3},
            {"num_predict", 40}
        }}
    };

    auto res = cli.Post("/api/generate", req_body.dump(), "application/json");
    if (res && res->status == 200) {
        try {
            auto resp_json = json::parse(res->body);
            if (resp_json.contains("response") && resp_json["response"].is_string()) {
                std::string generated = resp_json["response"].get<std::string>();
                auto extra_tokens = tokenize_query(generated);
                for (const auto& tok : extra_tokens) {
                    if (unique_terms.insert(tok).second) {
                        terms.push_back(tok);
                    }
                    if (terms.size() >= 10) break;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Ollama] JSON parse error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "[Ollama] Request failed or timed out, falling back to original query." << std::endl;
    }

    return terms;
}

// Спільна функція пошуку за списком токенів для REST та WebSocket
json execute_keyword_search(InvertedIndex& index,
                            const std::string& dataset_path,
                            const std::vector<std::string>& terms) {
    std::unordered_map<std::string, double> doc_scores;

    for (const auto& term : terms) {
        auto postings = index.get_postings(term);
        for (const auto& p : postings) {
            doc_scores[p.doc_id] += static_cast<double>(p.term_frequency);
        }
    }

    std::vector<SearchResult> results;
    results.reserve(doc_scores.size());
    for (const auto& [doc_id, score] : doc_scores) {
        results.push_back({doc_id, score, ""});
    }

    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.score > b.score;
    });

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
    return results_json;
}

int main() {
    const char* env_path = std::getenv("DATASET_PATH");
    std::string dataset_path = env_path ? env_path : "./data";

    const char* env_ollama_host = std::getenv("OLLAMA_HOST");
    std::string ollama_host = env_ollama_host ? env_ollama_host : "http://ollama:11434";

    const char* env_ollama_model = std::getenv("OLLAMA_MODEL");
    std::string ollama_model = env_ollama_model ? env_ollama_model : "qwen2.5:1.5b";

    size_t default_threads = 6;

    std::cout << "Initializing Inverted Index (256 shards)..." << std::endl;
    InvertedIndex index(256);

    std::cout << "Pre-indexing dataset from: " << dataset_path
              << " with " << default_threads << " threads..." << std::endl;
    IndexingStats current_stats = DatasetIndexer::index_directory(dataset_path, index, default_threads);
    std::mutex stats_mutex;
    std::mutex ollama_mutex;

    std::cout << "Dataset indexed: " << current_stats.documents_indexed << " docs, "
              << current_stats.total_terms << " terms in " << current_stats.time_ms << " ms." << std::endl;

    // Запускаємо WebSocket-сервер (/ws/search) на порту 8081 у фоновому потоці
    std::thread ws_thread([&index, &dataset_path, &ollama_host, &ollama_model, &ollama_mutex]() {
        start_ws_server(8081, [&](int client_fd, const std::string& raw_msg) {
            std::string query_str;
            try {
                auto body = json::parse(raw_msg);
                if (!body.contains("query") || !body["query"].is_string()) {
                    ws_send_text(client_fd, json({{"error", "Missing 'query' field"}}).dump());
                    return;
                }
                query_str = body["query"].get<std::string>();
            } catch (const std::exception&) {
                ws_send_text(client_fd, json({{"error", "Invalid JSON"}}).dump());
                return;
            }

            // Етап 1: query_expansion через Ollama
            std::vector<std::string> expanded_terms =
                expand_query_with_ollama(query_str, ollama_host, ollama_model, ollama_mutex);

            json stage1 = {
                {"stage", "query_expansion"},
                {"data", {
                    {"original", query_str},
                    {"expanded_terms", expanded_terms}
                }}
            };
            if (!ws_send_text(client_fd, stage1.dump())) return;

            // Етап 2: keyword_search по інвертованому індексу
            auto search_start = std::chrono::high_resolution_clock::now();
            json results_json = execute_keyword_search(index, dataset_path, expanded_terms);
            auto search_end = std::chrono::high_resolution_clock::now();
            auto search_ms = std::chrono::duration_cast<std::chrono::milliseconds>(search_end - search_start).count();

            json stage2 = {
                {"stage", "keyword_search"},
                {"data", {
                    {"results", results_json},
                    {"time_ms", search_ms}
                }}
            };
            if (!ws_send_text(client_fd, stage2.dump())) return;

            // Етап 3: завершення пайплайну
            json stage3 = {
                {"stage", "done"}
            };
            ws_send_text(client_fd, stage3.dump());
        });
    });
    ws_thread.detach();

    httplib::Server svr;

    // Роздаємо статичні файли для Web UI з папки ./web
    svr.set_mount_point("/", "./web");

    // GET /api/health
    svr.Get("/api/health", [&ollama_host](const httplib::Request&, httplib::Response& res) {
        bool ollama_ok = check_ollama_health(ollama_host);
        json j = {
            {"status", "ok"},
            {"ollama", ollama_ok}
        };
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/stats
    svr.Get("/api/stats", [&current_stats, &stats_mutex](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        json j = {
            {"documents", current_stats.documents_indexed},
            {"index_build_time_ms", current_stats.time_ms},
            {"threads_used", current_stats.threads_used}
        };
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/index
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

    // POST /api/search
    svr.Post("/api/search", [&index, &dataset_path, &ollama_host, &ollama_model, &ollama_mutex](const httplib::Request& req, httplib::Response& res) {
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

        std::vector<std::string> terms = (mode == "expanded" || mode == "llm")
            ? expand_query_with_ollama(query_str, ollama_host, ollama_model, ollama_mutex)
            : tokenize_query(query_str);

        json results_json = execute_keyword_search(index, dataset_path, terms);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        json response = {
            {"results", results_json},
            {"expanded_terms", terms},
            {"time_ms", elapsed_ms}
        };
        res.set_content(response.dump(), "application/json");
    });

    std::cout << "Search server listening on 0.0.0.0:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);

    return 0;
}