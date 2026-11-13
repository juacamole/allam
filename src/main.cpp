#include "client.hpp"
#include "config.hpp"
#include "daemon.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr const char* usage =
    "usage: allam daemon [-c config]\n"
    "       allam status\n"
    "       allam stop\n";

int daemon_main(int argc, char** argv) {
    std::string config = allam::default_config_path();
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config = argv[++i];
        } else {
            std::cerr << usage;
            return 2;
        }
    }
    allam::Daemon daemon(allam::load_config(config), allam::default_socket_path());
    return daemon.run();
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
        if (std::strcmp(argv[1], "daemon") == 0) return daemon_main(argc, argv);
        return client_main(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "allam: " << e.what() << "\n";
        return 1;
    }
}
