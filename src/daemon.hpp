#pragma once

#include "config.hpp"
#include "server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace allam {

class Daemon {
public:
    Daemon(Config cfg, std::string socket);
    int run();

private:
    void work();
    void refresh();
    void serve(int fd);
    nlohmann::json handle(const std::string& line);
    nlohmann::json status();

    Config cfg_;
    std::string socket_;
    Server server_;

    std::mutex mu_;
    std::condition_variable wake_;
    std::string phase_ = "starting";
    std::optional<std::string> model_;
    int ubatch_;
    int batch_;
    std::chrono::steady_clock::time_point started_ {};
};

}
