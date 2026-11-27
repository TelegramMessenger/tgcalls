#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "group_participant.h"
#include "split_channel.h"

// A group call split over two processes (--mode group --split host|join). The host runs the Go SFU and its own
// participants; the joining process runs only participants, and every SFU call they make (the join, the SSRC lookups,
// the end-of-run counts) is a request to the host over the split channel, answered from its SFU. The media goes
// between each participant and the SFU over UDP on 127.0.0.1, as in one process.
//
// Requests and replies are JSON objects, one frame each, one request in flight at a time:
//   hello {protocol}            -> {protocol, participants: [the host's participants]}  (after they exist)
//   join {id, payload, iceControlling, info} -> {response}   (the host records `info` as a remote participant)
//   querySsrc {ssrc}, queryVideoSsrcs {id}, queryRequestedLayer {receiver, sender}, querySsrcPackets {ssrc},
//   queryPayloadTypeStats {sender}, setNetworkParams {...}  -> {value}
//   ready {}  -> {}   the joiner's participants exist: both processes start the call's run
//   done {}   -> {}   the joiner has asked the SFU everything it needs: the host may destroy it
// Both sides log and fail the run on a protocol mismatch or a lost peer; no request waits forever.

// The host's end: serves the joiner's requests on a thread of its own.
class SplitGroupHost {
public:
    SplitGroupHost(std::shared_ptr<SplitChannel> channel, GoInt sfuHandle, std::vector<SplitParticipantInfo> participants);
    ~SplitGroupHost();

    // False when the joiner was not ready (or gone) within timeoutMs.
    bool waitReady(int timeoutMs);
    // False when the joiner did not finish (or went away first) within timeoutMs.
    bool waitDone(int timeoutMs);
    std::string error() const;

private:
    void serve();
    std::string handle(const std::string& request);

    std::shared_ptr<SplitChannel> _channel;
    GoInt _sfuHandle;
    std::vector<SplitParticipantInfo> _participants;
    std::thread _thread;
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    bool _ready = false;
    bool _done = false;
    bool _gone = false;
    std::string _error;
};

// The joiner's end: the host's SFU as a RemoteSfu, plus the run's handshake.
class SplitGroupClient : public RemoteSfu {
public:
    explicit SplitGroupClient(std::shared_ptr<SplitChannel> channel);

    // Fills `hostParticipants`; false (with `error`) on a protocol mismatch or no answer.
    bool hello(std::vector<SplitParticipantInfo>& hostParticipants, std::string& error);
    bool ready();
    bool done();
    // True once any request failed (no answer, a broken channel): the run is not trusted then.
    bool failed() const { return _failed.load(); }

    std::string join(int participantId, const std::string& payload, bool iceControlling,
                     const SplitParticipantInfo& info) override;
    int64_t querySsrc(uint32_t ssrc) override;
    std::string queryVideoSsrcs(int participantId) override;
    int queryRequestedLayer(int receiverId, int senderId) override;
    int64_t querySsrcPackets(uint32_t ssrc) override;
    std::string queryPayloadTypeStats(int senderId) override;
    void setNetworkParams(int participantId, int direction, int delayMs, int jitterMs, double dropRate,
                          int64_t bandwidthBps) override;

private:
    // Sends `request` and waits for its reply (timeoutMs); an empty string when none came.
    std::string call(const std::string& request, int timeoutMs);

    std::shared_ptr<SplitChannel> _channel;
    std::mutex _callMutex;
    std::atomic<bool> _failed{false};
};
