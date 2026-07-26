#ifndef TGCALLS_V2WASM_SIGNALING_FRAMING_H
#define TGCALLS_V2WASM_SIGNALING_FRAMING_H

// Core-side signaling framing: everything between JSON messages and the
// plaintext packets (seq(4, network order) || body) that the host seals.
// V1 (wire 10.0.0): faithful port of EncryptedConnection's reliability layer
// (seq flag bits, message packing, ack bookkeeping, resend policy, service
// packets). V2 (wire 11.0.0): gzip'd JSON bodies.
// Constants, byte layout and log strings are copied verbatim from
// EncryptedConnection.cpp — never change them (wire + log parity).
// WASM discipline: std + CoreGzip (host compression service) only.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tgcalls {
namespace v2wasm {

class SignalingFraming {
public:
    // Service causes, verbatim from EncryptedConnection.cpp.
    static constexpr int kServiceCauseNow = 0;
    static constexpr int kServiceCauseAcks = 1;
    static constexpr int kServiceCauseResend = 2;

    struct Delegate {
        std::function<void(std::vector<uint8_t> &&packet)> sendPacket; // full plaintext packet incl. seq
        std::function<void(std::string &&message)> deliverMessage;     // decoded JSON message
        std::function<void(int cause, int delayMs)> requestService;    // schedule onServiceTimer(cause)
        std::function<void(std::string const &line)> log;              // stock-format framing log lines
        std::function<int64_t()> nowMs;                                // core clock (event nowMs)
    };

    SignalingFraming(bool isV2, Delegate delegate);

    void sendMessage(std::string const &message);
    void receivePacket(std::vector<uint8_t> const &packet);
    void onServiceTimer(int cause);
    // Bare empty V1 packet (plus any pending acks/resends) even when idle;
    // variant keepalive seam. No-op on V2.
    void sendKeepalive();

private:
    struct MessageForResend {
        std::vector<uint8_t> data;
        int64_t lastSent = 0;
    };

    std::optional<uint32_t> computeNextSeq(bool messageRequiresAck, bool singleMessagePacket);
    bool enoughSpaceInPacket(std::vector<uint8_t> const &buffer, size_t amount) const;
    void appendAcksToSend(std::vector<uint8_t> &buffer);
    void appendAdditionalMessages(std::vector<uint8_t> &buffer);
    bool registerIncomingCounter(uint32_t incomingCounter);
    bool registerSentAck(uint32_t counter, bool firstInPacket);
    void sendAckPostponed(uint32_t incomingSeq);
    void ackMyMessage(uint32_t seq);
    bool haveAdditionalMessages() const;
    void sendServicePacket();

    bool _isV2 = false;
    Delegate _delegate;
    uint32_t _counter = 0;
    std::vector<uint32_t> _largestIncomingCounters;
    std::vector<uint32_t> _acksToSendSeqs;
    std::vector<uint32_t> _acksSentCounters;
    std::vector<MessageForResend> _myNotYetAckedMessages;
    bool _resendTimerActive = false;
    bool _sendAcksTimerActive = false;
};

} // namespace v2wasm
} // namespace tgcalls

#endif
