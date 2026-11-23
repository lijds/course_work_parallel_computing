#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options] \"<search query>\"\n"
              << "Options:\n"
              << "  --keyword     Use exact keyword search without LLM expansion\n"
              << "  --expanded    Use Ollama query expansion (default)\n"
              << "  --help        Show this help message\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string mode = "expanded";
    std::string query;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--keyword") {
            mode = "keyword";
        } else if (arg == "--expanded") {
            mode = "expanded";
        } else {
            if (!query.empty()) query += " ";
            query += arg;
        }
    }

    if (query.empty()) {
        std::cerr << "Error: Search query cannot be empty.\n";
        print_usage(argv[0]);
        return 1;
    }

    const char* env_url = std::getenv("SEARCH_SERVER_URL");
    std::string server_url = env_url ? env_url : "http://localhost:8080";

    httplib::Client cli(server_url);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);

    json req_body = {
        {"query", query},
        {"mode", mode}
    };

    auto res = cli.Post("/api/search", req_body.dump(), "application/json");
    if (!res) {
        std::cerr << "Error: Failed to connect to search server at " << server_url << "\n";
        return 1;
    }

    if (res->status != 200) {
        std::cerr << "Server returned HTTP " << res->status << ": " << res->body << "\n";
        return 1;
    }

    try {
        auto data = json::parse(res->body);

        std::cout << "Query:  \"" << query << "\" (mode: " << mode << ")\n";
        if (data.contains("expanded_terms") && data["expanded_terms"].is_array()) {
            std::cout << "Terms:  ";
            for (size_t i = 0; i < data["expanded_terms"].size(); ++i) {
                std::cout << data["expanded_terms"][i].get<std::string>()
                          << (i + 1 < data["expanded_terms"].size() ? ", " : "\n");
            }
        }

        long long time_ms = data.value("time_ms", 0LL);
        const auto& results = data["results"];
        std::cout << "Found:  " << results.size() << " top documents in " << time_ms << " ms\n";
        std::cout << "------------------------------------------------------------\n";

        if (results.empty()) {
            std::cout << "No matching documents found.\n";
        } else {
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                std::cout << "[" << (i + 1) << "] " << r.value("doc_id", "unknown")
                          << " (score: " << r.value("score", 0.0) << ")\n"
                          << "    " << r.value("snippet", "") << "\n\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing server response: " << e.what() << "\n";
        return 1;
    }

    return 0;
}