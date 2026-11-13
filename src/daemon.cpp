#include "daemon.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace allam {

using nlohmann::json;

namespace {

std::atomic<bool> quit{false};

void on_signal(int) { quit = true; }

void install_signals() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

sockaddr_un address(const std::string& path) {
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) throw std::runtime_error("socket path too long");
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

int listen_on(const std::string& path) {
    auto addr = address(path);

    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool taken = connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    close(probe);
    if (taken) throw std::runtime_error("already running on " + path);
    unlink(path.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        throw std::runtime_error("cannot listen on " + path + ": " + std::strerror(errno));
    }
    return fd;
}

struct Active {
    std::string id;
    std::string state;
};

std::optional<Active> active(const json& models) {
    for (auto& m : models) {
        auto s = m["status"].value("value", "");
        if (s == "loaded" || s == "loading" || s == "sleeping") return Active{m.value("id", ""), s};
    }
    return std::nullopt;
}

int to_size(const std::string& s) {
    try {
        size_t n = 0;
        int v = std::stoi(s, &n);
        if (n == s.size() && v >= 0 && v <= 65536) return v;
    } catch (const std::exception&) {
    }
    return -1;
}

json error(const std::string& msg) {
    return {{"ok", false}, {"error", msg}};
}

}

Daemon::Daemon(Config cfg, std::string socket)
    : socket_(std::move(socket)), server_(cfg), ubatch_(cfg.ubatch), batch_(cfg.batch) {}

int Daemon::run() {
    install_signals();
    int fd = listen_on(socket_);
    std::thread worker([this] { work(); });

    while (!quit) {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        serve(client);
        close(client);
    }

    wake_.notify_all();
    worker.join();
    close(fd);
    unlink(socket_.c_str());
    return 0;
}

void Daemon::serve(int fd) {
    timeval timeout {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string line;
    char buf[512];
    while (line.find('\n') == std::string::npos && line.size() < 4096) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        line.append(buf, n);
    }
    line = line.substr(0, line.find('\n'));

    auto reply = handle(line).dump() + "\n";
    ssize_t ignored = write(fd, reply.data(), reply.size());
    (void)ignored;
}

json Daemon::handle(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    if (words.empty()) return error("empty command");

    const auto& cmd = words[0];
    if (cmd == "status") return status();
    if (cmd == "models" || cmd == "ls" || cmd == "list") {
        json list = json::array();
        for (auto& m : server_.models()) {
            list.push_back({{"index", list.size() + 1}, {"id", m.value("id", "")}, {"status", m["status"].value("value", "")}});
        }
        return {{"ok", true}, {"models", list}};
    }
    if (cmd == "load" || cmd == "unload") {
        std::optional<std::string> model;
        {
            std::lock_guard lock(mu_);
            if (phase_ != "running") return error("server is " + phase_);
            model = model_;
        }
        if (words.size() > 1) model = words[1];
        else if (cmd == "load") return error("usage: load <model>");
        if (!model) return error("no model loaded");

        auto err = cmd == "load" ? server_.load(*model) : server_.unload(*model);
        if (err) return error(*err);
        {
            std::lock_guard lock(mu_);
            poke_ = true;
        }
        wake_.notify_all();
        return {{"ok", true}};
    }
    if (cmd == "batch") {
        if (words.size() < 2 || words.size() > 3) return error("usage: batch <ubatch> [batch]");
        int ub = to_size(words[1]);
        int b = words.size() > 2 ? to_size(words[2]) : 0;
        if (ub < 1 || b < 0) return error("batch sizes must be 1-65536");
        {
            std::lock_guard lock(mu_);
            if (b == 0) b = std::max(ub, batch_);
            if (ub > b) return error("ubatch must not exceed batch");
            if (ub == ubatch_ && b == batch_) pending_.reset();
            else pending_ = {ub, b};
            poke_ = true;
        }
        wake_.notify_all();
        return status();
    }
    if (cmd == "stop") {
        quit = true;
        return {{"ok", true}};
    }
    return error("unknown command " + cmd);
}

json Daemon::status() {
    std::lock_guard lock(mu_);
    return {
        {"ok", true},
        {"phase", phase_},
        {"model", model_ ? json(*model_) : json(nullptr)},
        {"model_state", model_state_},
        {"ubatch", ubatch_},
        {"batch", batch_},
        {"busy", busy_},
        {"pending", pending_ ? json{{"ubatch", pending_->first}, {"batch", pending_->second}} : json(nullptr)},
    };
}

void Daemon::refresh() {
    bool healthy = server_.healthy();
    auto models = healthy ? server_.models() : json::array();
    auto found = active(models);
    bool busy = healthy && server_.busy(models);

    std::lock_guard lock(mu_);
    if (healthy && phase_ == "starting") phase_ = "running";
    model_ = found ? std::optional(found->id) : std::nullopt;
    model_state_ = found ? found->state : "unloaded";
    busy_ = busy;
}

void Daemon::start_server() {
    try {
        started_ = std::chrono::steady_clock::now();
        server_.start();
    } catch (const std::exception& e) {
        std::cerr << "allam: " << e.what() << "\n";
    }
}

void Daemon::restart(std::pair<int, int> sizes, std::optional<std::string> model) {
    while (!server_.stop()) std::cerr << "allam: llama-server still busy, waiting\n";
    server_.set_batch(sizes.first, sizes.second);
    start_server();
    {
        std::lock_guard lock(mu_);
        ubatch_ = sizes.first;
        batch_ = sizes.second;
        if (pending_ == sizes) pending_.reset();
        phase_ = "starting";
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!quit && server_.alive() && !server_.healthy() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (model && !quit) {
        if (auto err = server_.load(*model)) std::cerr << "allam: reload " << *model << ": " << *err << "\n";
    }
    refresh();
}

void Daemon::work() {
    while (!quit) {
        if (!server_.alive() && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(5)) {
            {
                std::lock_guard lock(mu_);
                if (phase_ == "running") std::cerr << "allam: llama-server exited, restarting\n";
                phase_ = "starting";
            }
            start_server();
        }

        refresh();

        std::optional<std::pair<int, int>> sizes;
        std::optional<std::string> model;
        {
            std::lock_guard lock(mu_);
            if (pending_ && phase_ == "running" && !busy_) {
                sizes = pending_;
                if (model_state_ == "loaded" || model_state_ == "loading") model = model_;
                phase_ = "restarting";
            }
        }
        if (sizes) restart(*sizes, model);

        std::unique_lock lock(mu_);
        wake_.wait_for(lock, std::chrono::seconds(1), [this] { return quit || poke_; });
        poke_ = false;
    }

    {
        std::lock_guard lock(mu_);
        phase_ = "stopping";
    }
    while (!server_.stop()) std::cerr << "allam: llama-server still busy, waiting\n";
}

}
