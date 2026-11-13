#include "server.hpp"

#include <httplib.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace allam {

using nlohmann::json;

namespace {

httplib::Client http(const Config& c) {
    httplib::Client cli(c.host, c.port);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(5);
    return cli;
}

std::vector<pid_t> children(pid_t pid) {
    std::vector<pid_t> out;
    std::error_code ec;
    auto tasks = std::filesystem::path("/proc") / std::to_string(pid) / "task";
    for (auto& task : std::filesystem::directory_iterator(tasks, ec)) {
        std::ifstream in(task.path() / "children");
        for (pid_t p; in >> p;) out.push_back(p);
    }
    return out;
}

std::vector<char*> c_strings(std::vector<std::string>& v) {
    std::vector<char*> out;
    for (auto& s : v) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

}

Server::Server(Config cfg) : cfg_(std::move(cfg)) {}

std::vector<std::string> Server::command() const {
    std::vector<std::string> c{
        cfg_.server,
        "--models-preset", cfg_.preset,
        "--host", cfg_.host,
        "--port", std::to_string(cfg_.port),
        "-ub", std::to_string(cfg_.ubatch),
        "-b", std::to_string(cfg_.batch),
    };
    c.insert(c.end(), cfg_.args.begin(), cfg_.args.end());
    return c;
}

std::vector<std::string> Server::environment() const {
    std::vector<std::string> out;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        auto name = kv.substr(0, kv.find('='));
        bool overridden = false;
        for (auto& [k, v] : cfg_.env) overridden |= k == name;
        if (!overridden) out.push_back(kv);
    }
    for (auto& [k, v] : cfg_.env) out.push_back(k + "=" + v);
    return out;
}

void Server::start() {
    auto cmd = command();
    auto env = environment();
    auto argv = c_strings(cmd);
    auto envp = c_strings(env);

    int logfd = -1;
    if (!cfg_.log.empty()) {
        logfd = open(cfg_.log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (logfd < 0) throw std::runtime_error("cannot open " + cfg_.log);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (logfd >= 0) close(logfd);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
        }
        setpriority(PRIO_PROCESS, 0, cfg_.nice);
        execvpe(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    if (logfd >= 0) close(logfd);
    pid_ = pid;
}

bool Server::alive() {
    if (pid_ <= 0) return false;
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return false;
    }
    return true;
}


bool Server::stop() {
    if (!terminating_) {
        if (!alive()) return true;
        kids_ = children(pid_);
        kill(pid_, SIGTERM);
        terminating_ = true;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg_.stop_timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        bool done = !alive();
        for (pid_t k : kids_) done &= kill(k, 0) != 0 && errno == ESRCH;
        if (done) {
            terminating_ = false;
            kids_.clear();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

void Server::set_batch(int ubatch, int batch) {
    cfg_.ubatch = ubatch;
    cfg_.batch = batch;
}

bool Server::healthy() const {
    auto res = http(cfg_).Get("/health");
    return res && res->status == 200;
}

json Server::models() const {
    auto res = http(cfg_).Get("/models");
    if (!res || res->status != 200) return json::array();
    auto body = json::parse(res->body, nullptr, false);
    if (!body.is_object() || !body.contains("data")) return json::array();
    return body["data"];
}

}
