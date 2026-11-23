#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

// Допоміжна функція для обчислення Sec-WebSocket-Accept (SHA-1 + Base64)
inline std::string compute_ws_accept(const std::string& client_key) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = client_key + magic;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), hash);

    char base64_out[64] = {0};
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64_out), hash, SHA_DIGEST_LENGTH);
    return std::string(base64_out);
}

// Надсилання текстового WebSocket-фрейму (opcode 0x1) клієнту
inline bool ws_send_text(int client_fd, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN = 1, opcode = 0x1 (text)

    size_t len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
        }
    }

    frame.insert(frame.end(), payload.begin(), payload.end());
    ssize_t sent = send(client_fd, frame.data(), frame.size(), 0);
    return sent == static_cast<ssize_t>(frame.size());
}

// Зчитування одного вхідного текстового фрейму від клієнта (з розмаскуванням)
inline bool ws_recv_text(int client_fd, std::string& out_message) {
    uint8_t header[2];
    if (recv(client_fd, header, 2, MSG_WAITALL) != 2) return false;

    uint8_t opcode = header[0] & 0x0F;
    if (opcode == 0x8) return false; // Close frame

    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(client_fd, ext, 2, MSG_WAITALL) != 2) return false;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv(client_fd, ext, 8, MSG_WAITALL) != 8) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv(client_fd, mask, 4, MSG_WAITALL) != 4) return false;
    }

    if (payload_len > 1024 * 1024) return false; // Захист від занадто великих пакетів

    std::vector<char> data(payload_len);
    if (payload_len > 0) {
        if (recv(client_fd, data.data(), payload_len, MSG_WAITALL) != static_cast<ssize_t>(payload_len)) {
            return false;
        }
        if (masked) {
            for (uint64_t i = 0; i < payload_len; ++i) {
                data[i] ^= mask[i % 4];
            }
        }
    }

    out_message.assign(data.begin(), data.end());
    return true;
}

#include <thread>
#include <iostream>

// Запуск багатопотокового WebSocket-сервера на вказаному порту для маршруту /ws/search
inline void start_ws_server(int port,
                            std::function<void(int client_fd, const std::string& query_json)> on_search_request) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[WS] Failed to create socket" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[WS] Failed to bind port " << port << std::endl;
        close(server_fd);
        return;
    }

    listen(server_fd, 64);
    std::cout << "WebSocket server listening on 0.0.0.0:" << port << " (/ws/search)" << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) continue;

        // Обробляємо кожного WebSocket-клієнта в окремому потоці
        std::thread([client_fd, on_search_request]() {
            char buf[4096] = {0};
            ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                close(client_fd);
                return;
            }

            std::string req(buf, n);
            // Перевіряємо, що клієнт звернувся саме на /ws/search
            if (req.find("GET /ws/search ") == std::string::npos) {
                std::string bad = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                send(client_fd, bad.data(), bad.size(), 0);
                close(client_fd);
                return;
            }

            // Витягуємо Sec-WebSocket-Key із заголовків HTTP Upgrade
            std::string key_header = "Sec-WebSocket-Key:";
            size_t key_pos = req.find(key_header);
            if (key_pos == std::string::npos) {
                close(client_fd);
                return;
            }

            size_t val_start = key_pos + key_header.size();
            while (val_start < req.size() && req[val_start] == ' ') val_start++;
            size_t val_end = req.find("\r\n", val_start);
            std::string client_key = req.substr(val_start, val_end - val_start);

            // Відправляємо відповідь HTTP 101 Switching Protocols
            std::string accept_key = compute_ws_accept(client_key);
            std::string handshake =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept_key + "\r\n\r\n";

            if (send(client_fd, handshake.data(), handshake.size(), 0) < 0) {
                close(client_fd);
                return;
            }

            // Читаємо повідомлення клієнта в циклі поки з'єднання активне
            std::string message;
            while (ws_recv_text(client_fd, message)) {
                on_search_request(client_fd, message);
            }

            close(client_fd);
        }).detach();
    }
}