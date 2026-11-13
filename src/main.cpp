#include "client.hpp"
#include "config.hpp"
#include "daemon.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr const char* usage =
    "usage: allam start [-c config]\n"
    "       allam daemon [-c config]\n"
    "       allam status\n"
    "       allam ls | list | models\n"
    "       allam load <model>\n"
    "       allam unload [model]\n"
    "       allam batch <ubatch> [batch]\n"
    "       allam stop\n";

std::string config_arg(int argc, char** argv) {
    std::string config = allam::default_config_path();
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) config = argv[++i];
        else throw std::invalid_argument(usage);
    }
    return config;
}

std::string try_status(const std::string& socket) {
    try {
        return allam::request(socket, "status");
    } catch (const std::exception&) {
        return {};
    }
}

int daemon_main(int argc, char** argv) {
    allam::Daemon daemon(allam::load_config(config_arg(argc, argv)), allam::default_socket_path());
    return daemon.run();
}

int start_main(int argc, char** argv) {
    auto cfg = allam::load_config(config_arg(argc, argv));
    auto socket = allam::default_socket_path();
    if (!try_status(socket).empty()) {
        std::cerr << "allam: already running\n";
        return 1;
    }

    auto log = cfg.log.empty() ? allam::default_log_path() : cfg.log;
    std::filesystem::create_directories(std::filesystem::path(log).parent_path());
    int logfd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (logfd < 0) throw std::runtime_error("cannot open " + log);

    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int null = open("/dev/null", O_RDONLY);
        dup2(null, STDIN_FILENO);
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        try {
            allam::Daemon daemon(cfg, socket);
            std::exit(daemon.run());
        } catch (const std::exception& e) {
            std::cerr << "allam: " << e.what() << "\n";
            std::exit(1);
        }
    }
    close(logfd);
    waitpid(pid, nullptr, 0);

    for (int i = 0; i < 50; ++i) {
        auto status = try_status(socket);
        if (!status.empty()) {
            std::cout << status << "\n";
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "allam: daemon did not start, see " << log << "\n";
    return 1;
}

int client_main(int argc, char** argv) {
    std::string line;
    for (int i = 1; i < argc; ++i) line += (i > 1 ? " " : "") + std::string(argv[i]);

    auto reply = allam::request(allam::default_socket_path(), line);
    std::cout << reply << "\n";

    auto j = nlohmann::json::parse(reply, nullptr, false);
    return j.is_object() && j.value("ok", false) ? 0 : 1;
}

}

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
        std::cerr << usage;
        return argc < 2 ? 2 : 0;
    }
    try {
        if (std::strcmp(argv[1], "start") == 0) return start_main(argc, argv);
        if (std::strcmp(argv[1], "daemon") == 0) return daemon_main(argc, argv);
        return client_main(argc, argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << e.what();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "allam: " << e.what() << "\n";
        return 1;
    }
}
