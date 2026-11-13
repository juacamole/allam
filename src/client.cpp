#include "client.hpp"

#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace allam {

std::string request(const std::string& socket_path, const std::string& line) {
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) throw std::runtime_error("socket path too long");
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("daemon not running (" + socket_path + ")");
    }

    auto msg = line + "\n";
    if (write(fd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size())) {
        close(fd);
        throw std::runtime_error("write failed");
    }

    std::string reply;
    char buf[4096];
    for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) reply.append(buf, n);
    close(fd);

    while (!reply.empty() && reply.back() == '\n') reply.pop_back();
    return reply;
}

}
