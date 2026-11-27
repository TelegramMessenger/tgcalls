#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The channel between the two processes of a split call (--split): a Unix stream socket carrying frames of a u32
// big-endian length followed by that many bytes. A p2p call sends its signaling packets over it, a group call the
// joining process's SFU requests (split_group.h). Both binaries of a mixed-version run must speak the same protocol:
// the first frame each way is a hello naming kSplitProtocolVersion, and a mismatch ends the run.
constexpr int kSplitProtocolVersion = 1;

class SplitChannel {
public:
    // Binds PATH (an existing file there is removed first) and listens; accept() then waits for the one peer. A path
    // longer than a socket address holds (about 104 bytes) is an error, not a truncation.
    static std::unique_ptr<SplitChannel> listen(const std::string &path, std::string &error);
    // Connects to PATH, retrying until the listener exists or timeoutMs passes.
    static std::unique_ptr<SplitChannel> connect(const std::string &path, int timeoutMs, std::string &error);

    ~SplitChannel();

    // The listening side: waits up to timeoutMs for the peer. Closes the listening socket and unlinks PATH either way.
    bool accept(int timeoutMs, std::string &error);

    // Thread-safe. False once the peer is gone.
    bool send(const std::vector<uint8_t> &frame);
    bool send(const std::string &frame);
    // One frame; false on end of stream, error or timeout (timeoutMs < 0 waits forever). One reader at a time.
    bool receive(std::vector<uint8_t> &frame, int timeoutMs);
    bool receive(std::string &frame, int timeoutMs);

    // Shuts the socket down both ways: a receive() blocked on another thread returns false.
    void shutdown();

private:
    SplitChannel() = default;

    int _fd = -1;
    int _listenFd = -1;
    std::string _listenPath;
    std::mutex _sendMutex;
};
