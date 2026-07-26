#include "v2wasm/SignalingFraming.h"

#include <algorithm>

#include "v2wasm/CoreGzip.h"

namespace tgcalls {
namespace v2wasm {

namespace {

// All constants verbatim from EncryptedConnection.cpp (Type::Signaling).
constexpr uint32_t kSingleMessagePacketSeqBit = (uint32_t(1) << 31);
constexpr uint32_t kMessageRequiresAckSeqBit = (uint32_t(1) << 30);
constexpr uint32_t kMaxAllowedCounter = 0xFFFFFFFFu & ~kSingleMessagePacketSeqBit & ~kMessageRequiresAckSeqBit;
constexpr size_t kAckSerializedSize = sizeof(uint32_t) + sizeof(uint8_t);
constexpr size_t kNotAckedMessagesLimit = 64 * 1024;
constexpr size_t kKeepIncomingCountersCount = 64;
constexpr size_t kMaxSignalingPacketSize = 16 * 1024;
constexpr int kMinDelayBeforeMessageResend = 3000;
constexpr int kMaxDelayBeforeMessageResend = 5000;
constexpr int kMaxDelayBeforeAckResend = 5000;
constexpr size_t kV2DecompressSizeLimit = 2 * 1024 * 1024;

constexpr uint8_t kAckId = uint8_t(-1);
constexpr uint8_t kEmptyId = uint8_t(-2);
constexpr uint8_t kCustomId = uint8_t(127);

void appendSeq(std::vector<uint8_t> &buffer, uint32_t seq) {
    buffer.push_back(uint8_t((seq >> 24) & 0xff));
    buffer.push_back(uint8_t((seq >> 16) & 0xff));
    buffer.push_back(uint8_t((seq >> 8) & 0xff));
    buffer.push_back(uint8_t(seq & 0xff));
}

uint32_t readSeqAt(const uint8_t *bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

uint32_t counterFromSeq(uint32_t seq) {
    return seq & ~kSingleMessagePacketSeqBit & ~kMessageRequiresAckSeqBit;
}

} // namespace

SignalingFraming::SignalingFraming(bool isV2, Delegate delegate) :
_isV2(isV2),
_delegate(std::move(delegate)) {
}

std::optional<uint32_t> SignalingFraming::computeNextSeq(bool messageRequiresAck, bool singleMessagePacket) {
    if (messageRequiresAck && _myNotYetAckedMessages.size() >= kNotAckedMessagesLimit) {
        _delegate.log("ERROR! Too many not ACKed messages.");
        return std::nullopt;
    } else if (_counter == kMaxAllowedCounter) {
        _delegate.log("ERROR! Outgoing packet limit reached.");
        return std::nullopt;
    }
    return (++_counter)
        | (singleMessagePacket ? kSingleMessagePacketSeqBit : 0)
        | (messageRequiresAck ? kMessageRequiresAckSeqBit : 0);
}

bool SignalingFraming::enoughSpaceInPacket(std::vector<uint8_t> const &buffer, size_t amount) const {
    return (amount < kMaxSignalingPacketSize)
        && (16 + buffer.size() + amount <= kMaxSignalingPacketSize);
}

bool SignalingFraming::haveAdditionalMessages() const {
    return !_myNotYetAckedMessages.empty() || !_acksToSendSeqs.empty();
}

void SignalingFraming::appendAcksToSend(std::vector<uint8_t> &buffer) {
    auto i = _acksToSendSeqs.begin();
    while (i != _acksToSendSeqs.end() && enoughSpaceInPacket(buffer, kAckSerializedSize)) {
        _delegate.log("(signaling) Add ACK#" + std::to_string(counterFromSeq(*i)));
        appendSeq(buffer, *i);
        buffer.push_back(kAckId);
        ++i;
    }
    _acksToSendSeqs.erase(_acksToSendSeqs.begin(), i);
    for (const auto seq : _acksToSendSeqs) {
        _delegate.log("(signaling) Skip ACK#" + std::to_string(counterFromSeq(seq))
            + " (no space, length: " + std::to_string(kAckSerializedSize)
            + ", already: " + std::to_string(buffer.size()) + ")");
    }
}

void SignalingFraming::appendAdditionalMessages(std::vector<uint8_t> &buffer) {
    appendAcksToSend(buffer);

    if (_myNotYetAckedMessages.empty()) {
        return;
    }

    const int64_t now = _delegate.nowMs();
    for (auto &resending : _myNotYetAckedMessages) {
        const auto sent = resending.lastSent;
        const int64_t when = sent ? (sent + kMinDelayBeforeMessageResend) : 0;

        const auto counter = counterFromSeq(readSeqAt(resending.data.data()));
        const auto type = resending.data[4];
        if (when > now) {
            _delegate.log("(signaling) Skip RESEND:type" + std::to_string((int)type) + "#" + std::to_string(counter)
                + " (wait " + std::to_string(when - now) + "ms).");
            break;
        } else if (enoughSpaceInPacket(buffer, resending.data.size())) {
            _delegate.log("(signaling) Add RESEND:type" + std::to_string((int)type) + "#" + std::to_string(counter));
            buffer.insert(buffer.end(), resending.data.begin(), resending.data.end());
            resending.lastSent = now;
        } else {
            _delegate.log("(signaling) Skip RESEND:type" + std::to_string((int)type) + "#" + std::to_string(counter)
                + " (no space, length: " + std::to_string(resending.data.size())
                + ", already: " + std::to_string(buffer.size()) + ")");
            break;
        }
    }
    if (!_resendTimerActive) {
        _resendTimerActive = true;
        _delegate.requestService(kServiceCauseResend, kMaxDelayBeforeMessageResend);
    }
}

void SignalingFraming::sendMessage(std::string const &message) {
    if (_isV2) {
        // Stock V2 path: encryptRawPacket(gzip(json)) — seq has no flag bits
        // and no limit checks (EncryptedConnection::encryptRawPacket).
        std::vector<uint8_t> body(message.begin(), message.end());
        auto compressed = coreGzipData(body);
        if (!compressed) {
            _delegate.log("ERROR! Could not gzip signaling message");
            return;
        }
        const uint32_t seq = ++_counter;
        std::vector<uint8_t> packet;
        packet.reserve(4 + compressed->size());
        appendSeq(packet, seq);
        packet.insert(packet.end(), compressed->begin(), compressed->end());
        _delegate.sendPacket(std::move(packet));
        return;
    }

    // V1: port of prepareForSendingRawMessage(message, true) +
    // prepareForSendingMessageInternal — the pump profile always requires
    // acks, matching stock InstanceV2ReferenceImpl.
    const bool messageRequiresAck = true;
    const bool singleMessagePacket = !haveAdditionalMessages() && !messageRequiresAck; // always false
    const auto maybeSeq = computeNextSeq(messageRequiresAck, singleMessagePacket);
    if (!maybeSeq) {
        return;
    }
    const auto seq = *maybeSeq;

    // SerializeRawMessageWithSeq: seq(4) || kCustomId(1) || length(4) || bytes
    std::vector<uint8_t> serialized;
    serialized.reserve(4 + 1 + 4 + message.size());
    appendSeq(serialized, seq);
    serialized.push_back(kCustomId);
    appendSeq(serialized, (uint32_t)message.size()); // same big-endian u32 encoding
    serialized.insert(serialized.end(), message.begin(), message.end());

    if (!enoughSpaceInPacket(serialized, 0)) {
        _delegate.log("ERROR! Too large packet: " + std::to_string(serialized.size()));
        return;
    }
    const auto notYetAckedCopy = serialized;
    const bool sendEnqueued = !_myNotYetAckedMessages.empty();
    if (sendEnqueued) {
        // All requiring-ack messages are sent in order within one packet,
        // starting with the least not-yet-acked one (stock comment).
        _delegate.log("(signaling) Enqueue SEND:type" + std::to_string((int)kCustomId) + "#" + std::to_string(counterFromSeq(seq)));
    } else {
        _delegate.log("(signaling) Add SEND:type" + std::to_string((int)kCustomId) + "#" + std::to_string(counterFromSeq(seq)));
        appendAdditionalMessages(serialized);
    }
    _myNotYetAckedMessages.push_back({ notYetAckedCopy, _delegate.nowMs() });
    if (!sendEnqueued) {
        _delegate.sendPacket(std::move(serialized));
        return;
    }
    for (auto &queued : _myNotYetAckedMessages) {
        queued.lastSent = 0;
    }
    sendServicePacket(); // stock: return prepareForSendingService(0) — synchronous
}

void SignalingFraming::sendServicePacket() {
    const auto maybeSeq = computeNextSeq(false, false);
    if (!maybeSeq) {
        return;
    }
    // SerializeEmptyMessageWithSeq: seq(4) || kEmptyId(1)
    std::vector<uint8_t> serialized;
    serialized.reserve(5);
    appendSeq(serialized, *maybeSeq);
    serialized.push_back(kEmptyId);
    _delegate.log("(signaling) SEND:empty#" + std::to_string(counterFromSeq(*maybeSeq)));
    appendAdditionalMessages(serialized);
    _delegate.sendPacket(std::move(serialized));
}

void SignalingFraming::onServiceTimer(int cause) {
    if (_isV2) {
        return;
    }
    if (cause == kServiceCauseAcks) {
        _sendAcksTimerActive = false;
    } else if (cause == kServiceCauseResend) {
        _resendTimerActive = false;
    }
    if (!haveAdditionalMessages()) {
        return;
    }
    sendServicePacket();
}

void SignalingFraming::sendKeepalive() {
    if (_isV2) {
        return;
    }
    sendServicePacket();
}

bool SignalingFraming::registerIncomingCounter(uint32_t incomingCounter) {
    auto &list = _largestIncomingCounters;

    const auto position = std::lower_bound(list.begin(), list.end(), incomingCounter);
    const auto largest = list.empty() ? 0 : list.back();
    if (position != list.end() && *position == incomingCounter) {
        return false;
    } else if (incomingCounter + kKeepIncomingCountersCount <= largest) {
        return false;
    }
    const auto eraseTill = std::find_if(list.begin(), list.end(), [&](uint32_t counter) {
        return (counter + kKeepIncomingCountersCount > incomingCounter);
    });
    const auto eraseCount = eraseTill - list.begin();
    const auto positionIndex = (position - list.begin()) - eraseCount;
    list.erase(list.begin(), eraseTill);
    list.insert(list.begin() + positionIndex, incomingCounter);
    return true;
}

bool SignalingFraming::registerSentAck(uint32_t counter, bool firstInPacket) {
    auto &list = _acksSentCounters;

    const auto position = std::lower_bound(list.begin(), list.end(), counter);
    const auto already = (position != list.end()) && (*position == counter);

    if (firstInPacket) {
        list.erase(list.begin(), position);
        if (!already) {
            list.insert(list.begin(), counter);
        }
    } else if (!already) {
        list.insert(position, counter);
    }
    return !already;
}

void SignalingFraming::sendAckPostponed(uint32_t incomingSeq) {
    auto &list = _acksToSendSeqs;
    const auto already = std::find(list.begin(), list.end(), incomingSeq);
    if (already == list.end()) {
        list.push_back(incomingSeq);
    }
}

void SignalingFraming::ackMyMessage(uint32_t seq) {
    uint8_t type = 0;
    auto &list = _myNotYetAckedMessages;
    for (auto i = list.begin(), e = list.end(); i != e; ++i) {
        if (readSeqAt(i->data.data()) == seq) {
            type = i->data[4];
            list.erase(i);
            break;
        }
    }
    _delegate.log("(signaling) " + (type
        ? "Got ACK:type" + std::to_string((int)type) + "#"
        : std::string("Repeated ACK#")) + std::to_string(counterFromSeq(seq)));
}

void SignalingFraming::receivePacket(std::vector<uint8_t> const &packet) {
    if (packet.size() < 5) {
        return;
    }
    const uint32_t packetSeq = readSeqAt(packet.data());

    if (_isV2) {
        std::vector<uint8_t> body(packet.begin() + 4, packet.end());
        if (coreIsGzip(body)) {
            auto decompressed = coreGunzipData(body, kV2DecompressSizeLimit);
            if (!decompressed) {
                _delegate.log("ERROR! Could not decompress signaling data");
                return;
            }
            body = std::move(*decompressed);
        }
        _delegate.deliverMessage(std::string(body.begin(), body.end()));
        return;
    }

    // Shadow stock's shared counter list: handleIncomingRawPacket registers
    // the packet-level counter into the same list the additional-message
    // dedup reads. The host already replay-checked the packet, so the return
    // value is ignored — this keeps the list state identical to stock.
    registerIncomingCounter(counterFromSeq(packetSeq));

    // Port of processRawPacket.
    bool additionalMessage = false;
    bool firstMessageRequiringAck = true;
    bool newRequiringAckReceived = false;
    uint32_t currentSeq = packetSeq;
    uint32_t currentCounter = counterFromSeq(currentSeq);
    size_t pos = 4;
    std::vector<std::string> received;

    while (true) {
        const uint8_t type = packet[pos];
        const bool singleMessagePacket = (currentSeq & kSingleMessagePacketSeqBit) != 0;
        if (singleMessagePacket && additionalMessage) {
            _delegate.log("ERROR! Single message packet bit in not first message.");
            return;
        }

        if (type == kEmptyId) {
            if (additionalMessage) {
                _delegate.log("ERROR! Empty message should be only the first one in the packet.");
                return;
            }
            _delegate.log("(signaling) Got RECV:empty#" + std::to_string(currentCounter));
            pos += 1;
        } else if (type == kAckId) {
            if (!additionalMessage) {
                _delegate.log("ERROR! Ack message must not be the first one in the packet.");
                return;
            }
            ackMyMessage(currentSeq);
            pos += 1;
        } else if (type == kCustomId) {
            pos += 1;
            // DeserializeRawMessage: length(4) || bytes, capped at 1 MiB.
            if (packet.size() - pos < 4) {
                _delegate.log("ERROR! Could not parse message from packet, type: " + std::to_string((int)type));
                return;
            }
            const uint32_t length = readSeqAt(packet.data() + pos);
            pos += 4;
            if (length > 1024 * 1024 || packet.size() - pos < length) {
                _delegate.log("ERROR! Could not parse message from packet, type: " + std::to_string((int)type));
                return;
            }
            std::string message((const char *)packet.data() + pos, length);
            pos += length;

            const bool messageRequiresAck = (currentSeq & kMessageRequiresAckSeqBit) != 0;
            const bool skipMessage = messageRequiresAck
                ? !registerSentAck(currentCounter, firstMessageRequiringAck)
                : (additionalMessage && !registerIncomingCounter(currentCounter));
            if (messageRequiresAck) {
                firstMessageRequiringAck = false;
                if (!skipMessage) {
                    newRequiringAckReceived = true;
                }
                sendAckPostponed(currentSeq);
                _delegate.log(std::string("(signaling) ") + (skipMessage ? "Repeated RECV:type" : "Got RECV:type")
                    + std::to_string((int)type) + "#" + std::to_string(currentCounter));
            }
            if (!skipMessage) {
                received.push_back(std::move(message));
            }
        } else {
            _delegate.log("ERROR! Could not parse message from packet, type: " + std::to_string((int)type));
            return;
        }

        if (pos == packet.size()) {
            break;
        } else if (singleMessagePacket) {
            _delegate.log("ERROR! Single message didn't fill the entire packet.");
            return;
        } else if (packet.size() - pos < 5) {
            _delegate.log("ERROR! Bad remaining data size: " + std::to_string(packet.size() - pos));
            return;
        }
        currentSeq = readSeqAt(packet.data() + pos);
        pos += 4;
        currentCounter = counterFromSeq(currentSeq);
        additionalMessage = true;
    }

    if (!_acksToSendSeqs.empty()) {
        if (newRequiringAckReceived) {
            // Stock: _requestSendService(0, 0) — a deferred immediate send.
            _delegate.requestService(kServiceCauseNow, 0);
        } else if (!_sendAcksTimerActive) {
            _sendAcksTimerActive = true;
            _delegate.requestService(kServiceCauseAcks, kMaxDelayBeforeAckResend);
        }
    }

    // Stock processes decrypted messages after the full packet parse.
    for (auto &message : received) {
        _delegate.deliverMessage(std::move(message));
    }
}

} // namespace v2wasm
} // namespace tgcalls
