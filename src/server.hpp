#pragma once

#include "config.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <sys/types.h>

namespace allam {

class Server {
public:
    explicit Server(Config cfg);

    void start();
    bool stop();
    bool alive();
    void set_batch(int ubatch, int batch);

    bool healthy() const;
    nlohmann::json models() const;

private:
    std::vector<std::string> command() const;
    std::vector<std::string> environment() const;

    Config cfg_;
    pid_t pid_ = -1;
    bool terminating_ = false;
    std::vector<pid_t> kids_;
};

}
