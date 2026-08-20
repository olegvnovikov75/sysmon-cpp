#ifndef API_H
#define API_H

#include "collector.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

// Порт API по умолчанию (8080 на oleg-ai занят Ollama)
constexpr int API_PORT_DEFAULT = 18080;

// Collector → JSON (сервер отдаёт этот снимок)
std::string collector_to_json(const Collector& c);
// JSON → Collector (клиент); false — ошибка парсинга (в err)
bool json_to_collector(const std::string& json, Collector& out, std::string& err);

// HTTP-сервер: GET / → последний JSON-снимок (hidden-режим, -h)
class ApiServer {
public:
    bool start(int port, std::string& err);
    void stop();
    void set_snapshot(const std::string& json);
private:
    void loop();
    std::mutex mu_;
    std::string snap_;
    std::atomic<bool> run_{false};
    std::thread th_;
    int fd_ = -1;
};

// Клиент: GET http://host[:port]/ (порт по умолчанию API_PORT_DEFAULT) → Collector
bool fetch_remote(const std::string& hostport, Collector& out, std::string& err);

#endif // API_H
