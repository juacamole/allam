#pragma once

#include "config.hpp"
#include "memory.hpp"

#include <nlohmann/json.hpp>

#include <memory>
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
    bool busy(const nlohmann::json& models) const;
    nlohmann::json slots(const std::string& model) const;
    Footprint footprint() const;
    std::optional<std::string> load(const std::string& model) const;
    std::optional<std::string> unload(const std::string& model) const;

private:
    std::vector<std::string> command() const;
    std::vector<std::string> environment() const;
    std::optional<std::string> post_model(const std::string& path, const std::string& model) const;
    std::optional<bool> busy_metrics(const std::string& model) const;

    Config cfg_;
    pid_t pid_ = -1;
    bool terminating_ = false;
    std::vector<pid_t> kids_;
    std::shared_ptr<LoadLog> load_log_ = std::make_shared<LoadLog>();
};

}
