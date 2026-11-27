#pragma once

#include <csignal>

// A write to a TCP connection whose other end is gone raises SIGPIPE, which ends the process without a word. WebRTC
// keeps it off its sockets only on iOS (SO_NOSIGPIPE, under WEBRTC_IOS) and Linux (MSG_NOSIGNAL); the macOS host build
// has neither, so the CLI ignores the signal for the whole process: the write fails with EPIPE, which WebRTC handles.
// Test: cli_broken_pipe_test.
inline void cliIgnoreBrokenPipes() {
    std::signal(SIGPIPE, SIG_IGN);
}
