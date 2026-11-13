#pragma once

#include <string>
#include <utility>
#include <vector>

namespace allam {

struct Config {
    std::string server = "llama-server";
    std::string preset;
    std::string host = "127.0.0.1";
    int port = 8080;
    int ubatch = 512;
    int batch = 512;
    int nice = 19;
    int stop_timeout = 120;
    std::string log;
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> args;
};

Config load_config(const std::string& path);
std::string default_config_path();
std::string default_socket_path();
std::string default_log_path();

}
