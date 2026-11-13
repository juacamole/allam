#include "daemon.hpp"

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

std::optional<std::string> active(const json& models) {
    for (auto& m : models) {
        auto s = m["status"].value("value", "");
        if (s == "loaded" || s == "loading") return m.value("id", "");
    }
    return std::nullopt;
}

json error(const std::string& msg) {
    return {{"ok", false}, {"error", msg}};
}

}

Daemon::Daemon(Config cfg, std::string socket)
    : cfg_(cfg), socket_(std::move(socket)), server_(cfg), ubatch_(cfg.ubatch), batch_(cfg.batch) {}

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
        {"ubatch", ubatch_},
        {"batch", batch_},
    };
}

void Daemon::refresh() {
    bool healthy = server_.healthy();
    auto model = healthy ? active(server_.models()) : std::nullopt;

    std::lock_guard lock(mu_);
    if (healthy && phase_ == "starting") phase_ = "running";
    model_ = model;
}

void Daemon::work() {
    while (!quit) {
        if (!server_.alive() && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(5)) {
            {
                std::lock_guard lock(mu_);
                if (phase_ == "running") std::cerr << "allam: llama-server exited, restarting\n";
                phase_ = "starting";
            }
            try {
                started_ = std::chrono::steady_clock::now();
                server_.start();
            } catch (const std::exception& e) {
                std::cerr << "allam: " << e.what() << "\n";
            }
        }

        refresh();

        std::unique_lock lock(mu_);
        wake_.wait_for(lock, std::chrono::seconds(1), [] { return quit.load(); });
    }

    {
        std::lock_guard lock(mu_);
        phase_ = "stopping";
    }
    while (!server_.stop()) std::cerr << "allam: llama-server still busy, waiting\n";
}

}
