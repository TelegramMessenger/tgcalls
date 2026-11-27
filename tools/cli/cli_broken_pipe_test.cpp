// Host test for cliIgnoreBrokenPipes(): after it, a write to a TCP connection the other end reset fails with an error
// instead of ending the process (SIGPIPE). The socket is a plain one, as WebRTC's PhysicalSocket on the macOS host
// (no SO_NOSIGPIPE without WEBRTC_IOS, no MSG_NOSIGNAL outside Linux).

#include "cli_signals.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    cliIgnoreBrokenPipes();

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    if (listener < 0 || bind(listener, (sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, (sockaddr *)&address, &length) != 0) {
        std::printf("FAIL: no loopback listener: %s\n", std::strerror(errno));
        return 1;
    }
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (sockaddr *)&address, sizeof(address)) != 0) {
        std::printf("FAIL: no connection: %s\n", std::strerror(errno));
        return 1;
    }
    const int accepted = accept(listener, nullptr, nullptr);
    // Close with a reset: the next write finds the connection gone.
    linger reset{1, 0};
    setsockopt(accepted, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    close(accepted);
    usleep(100 * 1000);

    const char data[1024] = {};
    ssize_t result = 0;
    int error = 0;
    for (int i = 0; i < 5 && result >= 0; i++) {
        result = send(client, data, sizeof(data), 0);
        error = errno;
        usleep(10 * 1000);
    }
    if (result >= 0) {
        std::printf("FAIL: the write to a reset connection succeeded\n");
        return 1;
    }
    std::printf("ok: the write failed with %s, the process goes on\n", std::strerror(error));
    close(client);
    close(listener);
    return 0;
}
