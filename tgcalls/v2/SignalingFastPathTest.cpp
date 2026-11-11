// The signaling fast path (SignalingSctpConnection.h, signaling_fast_path):
// the envelope, which a peer without the fast path must reject, and the
// receiver, which must deliver each of the peer's messages once, in its send
// order, from whichever copy arrives first.

#include "v2/SignalingSctpConnection.h"

#include "net/dcsctp/packet/sctp_packet.h"
#include "net/dcsctp/public/dcsctp_options.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

using namespace tgcalls::signaling_fast_path;

std::vector<uint8_t> bytes(std::string const &text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<std::vector<uint8_t>> list(std::vector<std::string> const &texts) {
    std::vector<std::vector<uint8_t>> result;
    for (const auto &text : texts) {
        result.push_back(bytes(text));
    }
    return result;
}

Envelope envelope(uint32_t position, std::string const &text) {
    Envelope result;
    result.position = position;
    result.message = bytes(text);
    return result;
}

void TestRoundTrip() {
    const auto packet = encode(7, bytes("offer"));
    CHECK_TRUE(packet.size() == kHeaderSize + 5);
    const auto decoded = decode(packet);
    CHECK_TRUE(decoded.has_value());
    CHECK_TRUE(decoded->position == 7);
    CHECK_TRUE(decoded->message == bytes("offer"));

    const auto empty = decode(encode(0, {}));
    CHECK_TRUE(empty.has_value() && empty->message.empty());
}

// What reaches decode() is mostly SCTP packets: ports 5000 and 5000.
void TestSctpPacketsAreNotEnvelopes() {
    auto packet = encode(0, bytes("offer"));
    packet[0] = 0x13;
    packet[1] = 0x88;
    packet[2] = 0x13;
    packet[3] = 0x88;
    CHECK_TRUE(!decode(packet).has_value());

    auto wrongMagic = encode(0, bytes("offer"));
    wrongMagic[7] = 'Q';
    CHECK_TRUE(!decode(wrongMagic).has_value());

    auto otherVersion = encode(0, bytes("offer"));
    otherVersion[13] = 2;
    CHECK_TRUE(!decode(otherVersion).has_value());

    auto shortPacket = encode(0, {});
    shortPacket.pop_back();
    CHECK_TRUE(!decode(shortPacket).has_value());
}

// A build without the fast path hands an envelope to dcsctp, which must reject
// it whatever its checksum options.
void TestDcsctpRejectsEnvelopes() {
    const auto packet = encode(3, bytes("an offer of some length, past the chunk header"));

    dcsctp::DcSctpOptions defaults;
    CHECK_TRUE(!dcsctp::SctpPacket::Parse(packet, defaults).has_value());

    dcsctp::DcSctpOptions noChecksum;
    noChecksum.disable_checksum_verification = true;
    CHECK_TRUE(!dcsctp::SctpPacket::Parse(packet, noChecksum).has_value());

    dcsctp::DcSctpOptions zeroChecksum;
    zeroChecksum.zero_checksum_alternate_error_detection_method = dcsctp::ZeroChecksumAlternateErrorDetectionMethod::LowerLayerDtls();
    CHECK_TRUE(!dcsctp::SctpPacket::Parse(packet, zeroChecksum).has_value());

    const auto empty = encode(0, {});
    CHECK_TRUE(!dcsctp::SctpPacket::Parse(empty, noChecksum).has_value());
}

void TestEnvelopesFirstThenTheirSctpCopies() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "offer")) == list({"offer"}));
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate 1")) == list({"candidate 1"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate 1")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate 2")) == list({"candidate 2"}));
    CHECK_TRUE(!receiver.isBroken());
}

void TestSctpFirstThenTheEnvelopes() {
    Receiver receiver;
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")) == list({"offer"}));
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "offer")).empty());
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate")) == list({"candidate"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate")).empty());
    CHECK_TRUE(!receiver.isBroken());
}

// Envelopes travel unordered: one ahead of a gap waits for it.
void TestAnEnvelopeAheadOfAGapWaits() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate")).empty());
    CHECK_TRUE(receiver.onEnvelope(envelope(2, "media state")).empty());
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "offer")) == list({"offer", "candidate", "media state"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("media state")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("answer 2")) == list({"answer 2"}));
}

// A lost envelope: SCTP fills its position and releases what waited behind it.
void TestSctpFillsAGap() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")) == list({"offer", "candidate"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate")).empty());
    CHECK_TRUE(!receiver.isBroken());
}

// The callee answers after its first sends went out without envelopes: its
// envelopes start at a later position.
void TestEnvelopesStartingLater() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(2, "answer")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("setup")) == list({"setup"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate")) == list({"candidate", "answer"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("answer")).empty());
}

void TestRepeatedEnvelopesAreDropped() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "offer")) == list({"offer"}));
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "offer")).empty());
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate")) == list({"candidate"}));
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate")).empty());
}

// The same text sent twice is two messages: the second is not a repeat.
void TestEqualMessagesAtTwoPositions() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "muted")) == list({"muted"}));
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "muted")) == list({"muted"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("muted")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("muted")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("muted")) == list({"muted"}));
}

void TestPositionsBeyondTheLimitAreDropped() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(kMaxPosition, "late")).empty());
    for (uint32_t i = 0; i < kMaxPosition; i++) {
        CHECK_TRUE(receiver.onSctpMessage(bytes("m" + std::to_string(i))).size() == 1);
    }
    CHECK_TRUE(receiver.onSctpMessage(bytes("late")) == list({"late"}));
}

// SCTP is authoritative: a copy that differs means the positions slipped, so
// SCTP's is delivered and from then on SCTP passes through.
void TestAMismatchDeliversSctpAndStopsTheFastPath() {
    Receiver receiver;
    CHECK_TRUE(receiver.onEnvelope(envelope(0, "forged")) == list({"forged"}));
    CHECK_TRUE(receiver.onEnvelope(envelope(2, "held")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")) == list({"offer"}));
    CHECK_TRUE(receiver.isBroken());
    CHECK_TRUE(receiver.onEnvelope(envelope(1, "candidate")).empty());
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate")) == list({"candidate"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("held")) == list({"held"}));
}

void TestWithoutEnvelopesSctpPassesThrough() {
    Receiver receiver;
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")) == list({"offer"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("offer")) == list({"offer"}));
    CHECK_TRUE(receiver.onSctpMessage(bytes("candidate")) == list({"candidate"}));
    CHECK_TRUE(!receiver.isBroken());
}

} // namespace

int main() {
    TestRoundTrip();
    TestSctpPacketsAreNotEnvelopes();
    TestDcsctpRejectsEnvelopes();
    TestEnvelopesFirstThenTheirSctpCopies();
    TestSctpFirstThenTheEnvelopes();
    TestAnEnvelopeAheadOfAGapWaits();
    TestSctpFillsAGap();
    TestEnvelopesStartingLater();
    TestRepeatedEnvelopesAreDropped();
    TestEqualMessagesAtTwoPositions();
    TestPositionsBeyondTheLimitAreDropped();
    TestAMismatchDeliversSctpAndStopsTheFastPath();
    TestWithoutEnvelopesSctpPassesThrough();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
