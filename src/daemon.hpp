#pragma once

#include "config.hpp"
#include "server.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace allam {

class Daemon {
public:
    Daemon(Config cfg, std::string socket);
    int run();

private:
    void work();
    void refresh();
    void start_server();
    void restart(std::pair<int, int> sizes, std::optional<std::string> model);
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
    std::string model_state_ = "unloaded";
    int ubatch_;
    int batch_;
    bool busy_ = false;
    bool poke_ = false;
    std::optional<std::pair<int, int>> pending_;
    std::chrono::steady_clock::time_point started_ {};
};

}
