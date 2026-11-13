#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace allam {

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string expand(const std::string& s) {
    if (s.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME")) return home + s.substr(1);
    }
    return s;
}

[[noreturn]] void fail(int line, const std::string& msg) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + msg);
}

int to_int(const std::string& v, int line) {
    try {
        size_t n = 0;
        int x = std::stoi(v, &n);
        if (n == v.size() && x >= 0) return x;
    } catch (const std::exception&) {
    }
    fail(line, "not a number: " + v);
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

}

std::string default_config_path() {
    return env_or("ALLAM_CONFIG",
                  env_or("XDG_CONFIG_HOME", env_or("HOME", ".") + "/.config") + "/allam/allam.conf");
}

std::string default_socket_path() {
    return env_or("ALLAM_SOCKET", env_or("XDG_RUNTIME_DIR", "/tmp") + "/allam.sock");
}

Config load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);

    Config c;
    std::string raw;
    for (int n = 1; std::getline(in, raw); ++n) {
        auto line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) fail(n, "expected key = value");
        auto key = trim(line.substr(0, eq));
        auto val = trim(line.substr(eq + 1));

        if (key == "server") c.server = expand(val);
        else if (key == "preset") c.preset = expand(val);
        else if (key == "host") c.host = val;
        else if (key == "port") c.port = to_int(val, n);
        else if (key == "ubatch") c.ubatch = to_int(val, n);
        else if (key == "batch") c.batch = to_int(val, n);
        else if (key == "nice") c.nice = to_int(val, n);
        else if (key == "stop_timeout") c.stop_timeout = to_int(val, n);
        else if (key == "log") c.log = expand(val);
        else if (key == "env") {
            auto e = val.find('=');
            if (e == std::string::npos) fail(n, "expected env = NAME=value");
            c.env.emplace_back(trim(val.substr(0, e)), trim(val.substr(e + 1)));
        } else if (key == "arg") {
            std::istringstream words(val);
            for (std::string w; words >> w;) c.args.push_back(expand(w));
        } else {
            fail(n, "unknown key " + key);
        }
    }

    if (c.preset.empty()) throw std::runtime_error(path + ": preset is required");
    return c;
}

}
