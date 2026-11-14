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

void write_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        data += n;
        size -= static_cast<size_t>(n);
    }
}

void pump(int in, int out, std::shared_ptr<LoadLog> log) {
    std::string partial;
    char buf[8192];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        write_all(out, buf, static_cast<size_t>(n));
        partial.append(buf, static_cast<size_t>(n));
        for (size_t nl; (nl = partial.find('\n')) != std::string::npos;) {
            log->feed(partial.substr(0, nl));
            partial.erase(0, nl + 1);
        }
        if (partial.size() > 65536) partial.clear();
    }
    close(in);
    close(out);
}

double metric(const std::string& body, const std::string& name) {
    auto at = body.find("\nllamacpp:" + name + " ");
    if (at == std::string::npos) return 0;
    try {
        return std::stod(body.substr(at + name.size() + 11));
    } catch (const std::exception&) {
        return 0;
    }
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

    int logfd = cfg_.log.empty()
        ? fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0)
        : open(cfg_.log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (logfd < 0) throw std::runtime_error("cannot open log " + cfg_.log);

    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(logfd);
        throw std::runtime_error("pipe failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(logfd);
        close(out[0]);
        close(out[1]);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        setpriority(PRIO_PROCESS, 0, cfg_.nice);
        execvpe(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    close(out[1]);
    pid_ = pid;
    std::thread(pump, out[0], logfd, load_log_).detach();
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


std::optional<std::string> Server::post_model(const std::string& path, const std::string& model) const {
    auto res = http(cfg_).Post(path, json{{"model", model}}.dump(), "application/json");
    if (!res) return "llama-server unreachable";
    if (res->status == 200) return std::nullopt;
    auto body = json::parse(res->body, nullptr, false);
    if (body.is_object() && body.contains("error")) return body["error"].value("message", res->body);
    return res->body;
}

std::optional<std::string> Server::load(const std::string& model) const {
    return post_model("/models/load", model);
}

std::optional<std::string> Server::unload(const std::string& model) const {
    return post_model("/models/unload", model);
}


bool Server::busy(const json& models) const {
    for (auto& m : models) {
        auto state = m["status"].value("value", "");
        if (state == "loading") return true;
        if (state != "loaded") continue;

        auto id = m.value("id", "");
        if (auto busy = busy_metrics(id)) {
            if (*busy) return true;
            continue;
        }
        auto list = slots(id);
        if (list.empty()) return true;
        for (auto& slot : list) {
            if (slot.value("processing", false)) return true;
        }
    }
    return false;
}

std::optional<bool> Server::busy_metrics(const std::string& model) const {
    httplib::Params params{{"model", model}, {"autoload", "false"}};
    auto res = http(cfg_).Get("/metrics", params, httplib::Headers{});
    if (!res || res->status != 200) return std::nullopt;
    return metric(res->body, "requests_processing") > 0 || metric(res->body, "requests_deferred") > 0;
}

json Server::slots(const std::string& model) const {
    httplib::Params params{{"model", model}, {"autoload", "false"}};
    auto res = http(cfg_).Get("/slots", params, httplib::Headers{});
    if (!res || res->status != 200) return json::array();
    auto body = json::parse(res->body, nullptr, false);
    if (!body.is_array()) return json::array();

    json out = json::array();
    for (auto& s : body) {
        long tokens = s.value("n_prompt_tokens", 0L);
        if (s.contains("next_token") && s["next_token"].is_array() && !s["next_token"].empty()) {
            tokens += s["next_token"][0].value("n_decoded", 0L);
        }
        out.push_back({
            {"id", s.value("id", 0)},
            {"processing", s.value("is_processing", false)},
            {"tokens", tokens},
            {"n_ctx", s.value("n_ctx", 0)},
        });
    }
    return out;
}

Footprint Server::footprint() const {
    return load_log_->footprint();
}

}
