// Host test for TurnCustomizerImpl, the STUN marking of the custom 1:1 engine's TURN requests (NativeNetworkingImpl,
// with the server's voip_enable_stun_marking): every request carries SOFTWARE "Telegram ".
// M159's TurnPort signs an authenticated request (USERNAME, REALM, NONCE, MESSAGE-INTEGRITY) before it calls the
// customizer, and StunMessage::AddAttribute RTC_CHECKs that nothing but integrity attributes is added to a signed
// message: the customizer must take the signature off, add SOFTWARE and sign again with the same key.

#include "TurnCustomizerImpl.h"

#include "api/transport/stun.h"
#include "rtc_base/byte_buffer.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

// A TURN allocate request as TurnAllocateRequest builds it (turn_port.cc).
std::unique_ptr<webrtc::TurnMessage> makeAllocateRequest() {
    auto message = std::make_unique<webrtc::TurnMessage>(webrtc::STUN_ALLOCATE_REQUEST);
    auto transport = webrtc::StunAttribute::CreateUInt32(webrtc::STUN_ATTR_REQUESTED_TRANSPORT);
    transport->SetValue(17 << 24);
    message->AddAttribute(std::move(transport));
    return message;
}

std::string software(webrtc::StunMessage const &message) {
    const auto attribute = message.GetByteString(webrtc::STUN_ATTR_SOFTWARE);
    return attribute ? std::string(attribute->string_view()) : std::string();
}

// The first allocate goes out unauthenticated: SOFTWARE is simply added.
void TestAnUnsignedRequestGetsTheMarking() {
    auto message = makeAllocateRequest();
    tgcalls::TurnCustomizerImpl customizer;
    customizer.MaybeModifyOutgoingStunMessage(nullptr, message.get());

    CHECK_TRUE(software(*message) == "Telegram ");
    CHECK_TRUE(message->integrity() == webrtc::StunMessage::IntegrityStatus::kNotSet);
}

// After the server's 401 the request is signed before the customizer runs: it keeps the marking and a signature the
// server accepts, MESSAGE-INTEGRITY still last.
void TestASignedRequestGetsTheMarkingAndStaysSigned() {
    std::string key;
    CHECK_TRUE(webrtc::ComputeStunCredentialHash("user", "realm", "password", &key));

    auto message = makeAllocateRequest();
    message->AddAttribute(std::make_unique<webrtc::StunByteStringAttribute>(webrtc::STUN_ATTR_USERNAME, "user"));
    message->AddAttribute(std::make_unique<webrtc::StunByteStringAttribute>(webrtc::STUN_ATTR_REALM, "realm"));
    message->AddAttribute(std::make_unique<webrtc::StunByteStringAttribute>(webrtc::STUN_ATTR_NONCE, "nonce"));
    CHECK_TRUE(message->AddMessageIntegrity(key));

    tgcalls::TurnCustomizerImpl customizer;
    customizer.MaybeModifyOutgoingStunMessage(nullptr, message.get());

    CHECK_TRUE(software(*message) == "Telegram ");
    CHECK_TRUE(message->IntegrityOk());

    webrtc::ByteBufferWriter buffer;
    CHECK_TRUE(message->Write(&buffer));
    CHECK_TRUE(webrtc::StunMessage::ValidateMessageIntegrityForTesting(
        key, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(buffer.Data()), buffer.Length())));

    // The last attribute: MESSAGE-INTEGRITY's header (type 0x0008, length 20) and its 20 bytes end the message.
    const auto *bytes = reinterpret_cast<const uint8_t *>(buffer.Data());
    const size_t length = buffer.Length();
    CHECK_TRUE(length >= 24 && bytes[length - 24] == 0x00 && bytes[length - 23] == 0x08 && bytes[length - 21] == 20);
}

}  // namespace

int main() {
    TestAnUnsignedRequestGetsTheMarking();
    TestASignedRequestGetsTheMarkingAndStaysSigned();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
