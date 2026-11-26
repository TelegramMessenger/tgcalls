#ifndef TGCALLS_SIGNALING_SCTP_CONNECTION_H_
#define TGCALLS_SIGNALING_SCTP_CONNECTION_H_

#ifdef WEBRTC_WIN
#include <cstdint>
#include <WinSock2.h>
#endif // WEBRTC_WIN

#include "rtc_base/socket_address.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/byte_buffer.h"
#include "media/base/media_channel.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <absl/types/optional.h>

#include "StaticThreads.h"
#include "SignalingConnection.h"
#include "third-party/json11.hpp"

namespace webrtc {
class Socket;
}

namespace webrtc {
class SctpTransportInternal;
};

#include "api/transport/data_channel_transport_interface.h"
#include "api/field_trials_view.h"

namespace tgcalls {

class SignalingPacketTransport;

// The fast path (network_sctp_fast_path). Until its own association is up, a
// side also sends every message at once in an envelope beside SCTP, so the
// first offer and answer cross in one signaling leg each instead of waiting for
// the four-leg handshake. SCTP still carries every message and stays the
// reliable, ordered copy. The caller sends envelopes when the option is on; the
// callee only once it has received one, so a peer without the fast path never
// gets them. dcsctp rejects an envelope as a malformed packet, which is all a
// build without the fast path does with it.
namespace signaling_fast_path {

// The layout, big-endian:
//   0..3    ports 0 and 0 (the signaling SCTP packets carry 5000 and 5000)
//   4..7    'T' 'G' 'F' 'P', where SCTP has its verification tag
//   8..11   a fixed non-zero checksum, so dcsctp verifies the CRC32c
//   12..15  a chunk header of length 0, which dcsctp rejects whatever the
//           checksum: a chunk is never shorter than its 4-byte header
//   16..19  the message's position in the sender's send order, from 0
//   20..    the message: the same bytes SCTP carries
constexpr size_t kHeaderSize = 20;
// A sender puts its first kMaxPosition messages at most in envelopes, and a
// receiver keeps none further on.
constexpr uint32_t kMaxPosition = 64;

struct Envelope {
    uint32_t position = 0;
    std::vector<uint8_t> message;
};

std::vector<uint8_t> encode(uint32_t position, std::vector<uint8_t> const &message);
// nullopt for anything that is not an envelope, an SCTP packet included.
std::optional<Envelope> decode(std::vector<uint8_t> const &packet);

// Delivers the peer's messages once each, in its send order, from whichever
// copy arrives first. SCTP delivers every message, in order, so its j-th
// message is the one at position j; an envelope names its position. A copy of a
// position already delivered is dropped. An SCTP copy that differs from the
// envelope delivered at its position means the positions no longer line up:
// SCTP's copy is delivered, and from then on SCTP passes straight through and
// envelopes are dropped.
class Receiver {
public:
    std::vector<std::vector<uint8_t>> onEnvelope(Envelope &&envelope);
    std::vector<std::vector<uint8_t>> onSctpMessage(std::vector<uint8_t> &&message);

    bool isBroken() const {
        return _isBroken;
    }

private:
    void deliverHeld(std::vector<std::vector<uint8_t>> &out);

    uint64_t _nextPosition = 0;
    uint64_t _sctpMessageCount = 0;
    // Envelopes ahead of _nextPosition.
    std::map<uint64_t, std::vector<uint8_t>> _held;
    // Envelopes delivered whose SCTP copy has not arrived yet.
    std::map<uint64_t, std::vector<uint8_t>> _deliveredFromEnvelopes;
    bool _isBroken = false;
};

}  // namespace signaling_fast_path

class SignalingSctpConnection : public SignalingConnection, public webrtc::DataChannelSink {
private:
    struct PacketReadState {
        webrtc::CopyOnWriteBuffer headerData;
        int remainingHeaderSize = 0;
        bool isHeaderCompleted = false;

        webrtc::CopyOnWriteBuffer data;
        int remainingDataSize = 0;
        bool isDataCompleted = false;
    };

public:
    struct Options {
        int t1InitTimeoutMs;   // <= 0: dcsctp's own default (1000 ms)
        int t1CookieTimeoutMs; // <= 0: dcsctp's own default (1000 ms)
        int maxBackoffMs;      // <= 0: dcsctp's own default (3000 ms)
        // Hand messages to SCTP before the association is up, so the first one
        // travels with the COOKIE ECHO instead of after the COOKIE ACK.
        bool sendBeforeConnected;
        // Send messages on the fast path too until the association is up
        // (signaling_fast_path above).
        bool fastPath;
        // The field trials of the connection's Environment: null, the
        // process-wide string; else a copy of these (InstanceV2ReferenceImpl
        // gives the call's own). Never from the server's parameters.
        std::shared_ptr<const webrtc::FieldTrialsView> fieldTrials;
        Options() : t1InitTimeoutMs(400), t1CookieTimeoutMs(400), maxBackoffMs(750), sendBeforeConnected(false), fastPath(false) {}

        // The defaults above with the server's overrides applied
        // (network_sctp_t1_init_ms, network_sctp_t1_cookie_ms,
        // network_sctp_max_backoff_ms; network_sctp_send_before_connected and
        // network_sctp_fast_path, which only an explicit true turns on). An
        // absent key, or a timer outside 1...60000 ms, keeps the default.
        // Every engine that carries its signaling over SCTP builds its options
        // here.
        static Options fromCustomParameters(std::map<std::string, json11::Json> const &customParameters);
    };

    // No default for `options`: a caller that omitted it would silently run the
    // defaults whatever the server sent, which is how 11.0.0 and 18/19 ignored
    // the overrides until 2026-10-27. Pass Options::fromCustomParameters(...).
    SignalingSctpConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::function<void(const std::vector<uint8_t> &)> emitData, bool isInitiator, Options options);
    virtual ~SignalingSctpConnection();

    virtual void receiveExternal(const std::vector<uint8_t> &data) override;
    virtual void start() override;
    virtual void send(const std::vector<uint8_t> &data) override;

    virtual void OnDataReceived(int channel_id,
                                webrtc::DataMessageType type,
                                const webrtc::CopyOnWriteBuffer& buffer) override;
    virtual void OnReadyToSend() override;
    virtual void OnTransportClosed(webrtc::RTCError error) override;

    // Unused
    virtual void OnChannelClosing(int channel_id) override{}
    virtual void OnChannelClosed(int channel_id) override{}
    virtual void OnTransportConnected() override{}
    virtual void OnBufferedAmountLow(int channel_id) override{}
    virtual void OnMaxMessageSize(int max_message_size) override{}

private:
    std::shared_ptr<Threads> _threads;
    std::function<void(const std::vector<uint8_t> &)> _emitData;
    std::function<void(const std::vector<uint8_t> &)> _onIncomingData;

    std::unique_ptr<SignalingPacketTransport> _packetTransport;
    std::unique_ptr<webrtc::SctpTransportInternal> _sctpTransport;
    
    void deliverIncoming(std::vector<std::vector<uint8_t>> &&messages);

    bool _isInitiator = false;
    bool _isReadyToSend = false;
    bool _isAssociationUp = false;
    std::vector<std::vector<uint8_t>> _pendingData;

    bool _fastPath = false;
    bool _peerSendsFastPath = false;
    // Every message enters the SCTP stream in send() order, so this is the
    // position the peer's Receiver gives its SCTP copy.
    uint32_t _nextSendPosition = 0;
    // What the callee sent before it knew the peer takes envelopes (13.0.0's
    // callee sends its ICE candidates at start); sent as envelopes when the
    // first envelope arrives, if the association is still not up.
    std::vector<signaling_fast_path::Envelope> _sentWithoutEnvelope;
    signaling_fast_path::Receiver _fastPathReceiver;
};

}  // namespace tgcalls

#endif  // TGCALLS_SIGNALING_SCTP_CONNECTION_H_
