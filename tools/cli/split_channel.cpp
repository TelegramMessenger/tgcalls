#include "split_channel.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// A frame larger than this is a corrupt stream, not a message.
constexpr uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;

bool fillAddress(const std::string &path, sockaddr_un &address, std::string &error) {
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        error = "split socket path must be 1-" + std::to_string(sizeof(address.sun_path) - 1) + " bytes, got " +
            std::to_string(path.size()) + ": " + path;
        return false;
    }
    memcpy(address.sun_path, path.c_str(), path.size());
    return true;
}

void noSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

int sendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

using Clock = std::chrono::steady_clock;

// Waits until fd is readable; false on timeout or error. timeoutMs < 0 waits forever.
bool waitReadable(int fd, Clock::time_point deadline, bool forever) {
    while (true) {
        int waitMs = -1;
        if (!forever) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) {
                return false;
            }
            waitMs = (int)left;
        }
        pollfd entry{fd, POLLIN, 0};
        const int rc = poll(&entry, 1, waitMs);
        if (rc > 0) {
            return true;
        }
        if (rc == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

bool readExactly(int fd, uint8_t *buffer, size_t size, Clock::time_point deadline, bool forever) {
    size_t done = 0;
    while (done < size) {
        if (!waitReadable(fd, deadline, forever)) {
            return false;
        }
        const ssize_t n = ::recv(fd, buffer + done, size - done, 0);
        if (n > 0) {
            done += (size_t)n;
        } else if (n == 0) {
            return false;
        } else if (errno != EINTR && errno != EAGAIN) {
            return false;
        }
    }
    return true;
}

} // namespace

std::unique_ptr<SplitChannel> SplitChannel::listen(const std::string &path, std::string &error) {
    sockaddr_un address;
    if (!fillAddress(path, address, error)) {
        return nullptr;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + strerror(errno);
        return nullptr;
    }
    unlink(path.c_str());
    if (bind(fd, (sockaddr *)&address, sizeof(address)) != 0 || ::listen(fd, 1) != 0) {
        error = "bind/listen " + path + ": " + strerror(errno);
        close(fd);
        return nullptr;
    }
    std::unique_ptr<SplitChannel> channel(new SplitChannel());
    channel->_listenFd = fd;
    channel->_listenPath = path;
    return channel;
}

bool SplitChannel::accept(int timeoutMs, std::string &error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    bool ok = waitReadable(_listenFd, deadline, timeoutMs < 0);
    if (ok) {
        _fd = ::accept(_listenFd, nullptr, nullptr);
        ok = _fd >= 0;
        if (!ok) {
            error = std::string("accept: ") + strerror(errno);
        } else {
            noSigpipe(_fd);
        }
    } else {
        error = "no peer connected to " + _listenPath + " within " + std::to_string(timeoutMs) + " ms";
    }
    close(_listenFd);
    _listenFd = -1;
    unlink(_listenPath.c_str());
    return ok;
}

std::unique_ptr<SplitChannel> SplitChannel::connect(const std::string &path, int timeoutMs, std::string &error) {
    sockaddr_un address;
    if (!fillAddress(path, address, error)) {
        return nullptr;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            error = std::string("socket: ") + strerror(errno);
            return nullptr;
        }
        if (::connect(fd, (sockaddr *)&address, sizeof(address)) == 0) {
            noSigpipe(fd);
            std::unique_ptr<SplitChannel> channel(new SplitChannel());
            channel->_fd = fd;
            return channel;
        }
        const int connectErrno = errno;
        close(fd);
        if (connectErrno != ENOENT && connectErrno != ECONNREFUSED) {
            error = "connect " + path + ": " + strerror(connectErrno);
            return nullptr;
        }
        if (Clock::now() >= deadline) {
            error = "no listener at " + path + " within " + std::to_string(timeoutMs) + " ms";
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

SplitChannel::~SplitChannel() {
    if (_fd >= 0) {
        close(_fd);
    }
    if (_listenFd >= 0) {
        close(_listenFd);
        unlink(_listenPath.c_str());
    }
}

bool SplitChannel::send(const std::vector<uint8_t> &frame) {
    std::lock_guard<std::mutex> lock(_sendMutex);
    if (_fd < 0) {
        return false;
    }
    const uint32_t size = (uint32_t)frame.size();
    uint8_t header[4] = {(uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};
    std::vector<uint8_t> bytes;
    bytes.reserve(4 + frame.size());
    bytes.insert(bytes.end(), header, header + 4);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::send(_fd, bytes.data() + done, bytes.size() - done, sendFlags());
        if (n > 0) {
            done += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool SplitChannel::send(const std::string &frame) {
    return send(std::vector<uint8_t>(frame.begin(), frame.end()));
}

bool SplitChannel::receive(std::vector<uint8_t> &frame, int timeoutMs) {
    if (_fd < 0) {
        return false;
    }
    const bool forever = timeoutMs < 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(forever ? 0 : timeoutMs);
    uint8_t header[4];
    if (!readExactly(_fd, header, 4, deadline, forever)) {
        return false;
    }
    const uint32_t size = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) |
        (uint32_t)header[3];
    if (size > kMaxFrameBytes) {
        return false;
    }
    frame.resize(size);
    return size == 0 || readExactly(_fd, frame.data(), size, deadline, forever);
}

bool SplitChannel::receive(std::string &frame, int timeoutMs) {
    std::vector<uint8_t> bytes;
    if (!receive(bytes, timeoutMs)) {
        return false;
    }
    frame.assign(bytes.begin(), bytes.end());
    return true;
}

void SplitChannel::shutdown() {
    if (_fd >= 0) {
        ::shutdown(_fd, SHUT_RDWR);
    }
}
