// Host test for SctpDataChannelProviderInterfaceImpl's life on a DTLS transport that outlives it, as the custom group
// engine's restartDataChannel() and both custom engines' stop() have it.
//
// Since M159 a packet transport holds its listeners in callback lists keyed by a tag, and a single close callback.
// M123's sigslot disconnected a destroyed DcSctpTransport by itself; M159's DcSctpTransport never removes its
// callbacks (its DisconnectTransportSignals has no caller), and the transport clears its close callback after running
// it. The provider detaches the transport's four registrations itself; dcsctp_registrations_test keeps that list equal
// to DcSctpTransport::ConnectTransportSignals.
//
// A stale callback runs on freed memory, which an ordinary run rarely notices. Here every block freed while the
// provider is destroyed is filled with 0xA5 and never reused (PoisonedFrees), so a callback that reaches the destroyed
// transport dereferences a 0xA5A5... pointer and the test fails at once with a message (onFault). ASan shows the same
// as a use-after-free:
//   bazel test //submodules/TgVoipWebrtc:sctp_data_channel_provider_test --copt=-fsanitize=address \
//     --linkopt=-fsanitize=address --platform_suffix=asan

#include "SctpDataChannelProviderInterfaceImpl.h"
#include "StaticThreads.h"

#include "api/environment/environment_factory.h"
#include "p2p/dtls/dtls_transport_internal.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/thread.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <unistd.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

namespace {

// While true, operator delete fills a block with 0xA5 and leaks it instead of freeing it (WebRTC's threads free too).
std::atomic<bool> g_poisonFrees{false};

void poisonOrFree(void *pointer) noexcept {
    if (!pointer) {
        return;
    }
    if (g_poisonFrees) {
#if defined(__APPLE__)
        const size_t size = malloc_size(pointer);
#else
        const size_t size = malloc_usable_size(pointer);
#endif
        std::memset(pointer, 0xA5, size);
        return;
    }
    std::free(pointer);
}

class PoisonedFrees {
public:
    PoisonedFrees() {
        g_poisonFrees = true;
    }
    ~PoisonedFrees() {
        g_poisonFrees = false;
    }
};

void onFault(int) {
    static const char message[] = "FAIL: a transport callback reached the destroyed SCTP transport (freed memory)\n";
    (void)write(STDOUT_FILENO, message, sizeof(message) - 1);
    _exit(1);
}

}  // namespace

void operator delete(void *pointer) noexcept {
    poisonOrFree(pointer);
}
void operator delete(void *pointer, std::size_t) noexcept {
    poisonOrFree(pointer);
}
void operator delete[](void *pointer) noexcept {
    poisonOrFree(pointer);
}
void operator delete[](void *pointer, std::size_t) noexcept {
    poisonOrFree(pointer);
}

namespace {

int g_failures = 0;

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

class TestThreads : public tgcalls::Threads {
public:
    explicit TestThreads(webrtc::Thread *networkThread) : _networkThread(networkThread) {
    }

    webrtc::Thread *getNetworkThread() override {
        return _networkThread;
    }
    webrtc::Thread *getMediaThread() override {
        return _networkThread;
    }
    webrtc::Thread *getWorkerThread() override {
        return _networkThread;
    }

private:
    webrtc::Thread *_networkThread;
};

// A DTLS transport the test drives by hand: its writable state, an incoming packet, and closing.
class FakeDtlsTransport : public webrtc::DtlsTransportInternal {
public:
    FakeDtlsTransport() : _transportName("fake-dtls") {
    }

    void setWritable(bool writable) {
        if (_writable != writable) {
            _writable = writable;
            NotifyWritableState(this);
        }
    }

    void receivePacket() {
        // A truncated SCTP common header: DcSctpSocket drops it after DcSctpTransport has taken it.
        const std::vector<uint8_t> data = { 0x13, 0x88, 0x13, 0x88 };
        NotifyPacketReceived(webrtc::ReceivedIpPacket(
            std::span<const uint8_t>(data.data(), data.size()), webrtc::SocketAddress(), std::nullopt,
            webrtc::EcnMarking::kNotEct, webrtc::ReceivedIpPacket::kDtlsDecrypted));
    }

    void close() {
        NotifyOnClose();
    }

    void setDtlsState(webrtc::DtlsTransportState state) {
        SendDtlsState(this, state);
    }

    const std::string &transport_name() const override {
        return _transportName;
    }
    bool writable() const override {
        return _writable;
    }
    bool receiving() const override {
        return false;
    }
    int SendPacket(const char *data, size_t len, const webrtc::AsyncSocketPacketOptions &options, int flags = 0) override {
        return (int)len;
    }
    int SetOption(webrtc::Socket::Option opt, int value) override {
        return 0;
    }
    bool GetOption(webrtc::Socket::Option opt, int *value) override {
        return false;
    }
    int GetError() override {
        return 0;
    }
    std::optional<webrtc::NetworkRoute> network_route() const override {
        return std::nullopt;
    }
    webrtc::DtlsTransportState dtls_state() const override {
        return webrtc::DtlsTransportState::kConnected;
    }
    int component() const override {
        return 1;
    }
    bool IsDtlsActive() const override {
        return false;
    }
    bool GetDtlsRole(webrtc::SSLRole *role) const override {
        return false;
    }
    bool SetDtlsRole(webrtc::SSLRole role) override {
        return true;
    }
    bool GetSslVersionBytes(int *version) const override {
        return false;
    }
    uint16_t GetSslGroupId() const override {
        return 0;
    }
    bool GetSrtpCryptoSuite(int *cipher) const override {
        return false;
    }
    bool GetSslCipherSuite(int *cipher) const override {
        return false;
    }
    std::optional<absl::string_view> GetTlsCipherSuiteName() const override {
        return std::nullopt;
    }
    uint16_t GetSslPeerSignatureAlgorithm() const override {
        return 0;
    }
    bool SetLocalCertificate(const webrtc::scoped_refptr<webrtc::RTCCertificate> &certificate) override {
        return true;
    }
    std::unique_ptr<webrtc::SSLCertChain> GetRemoteSSLCertChain() const override {
        return nullptr;
    }
    bool AppendSrtpKeyingMaterial(webrtc::ZeroOnFreeBuffer<uint8_t> &keying_material) override {
        return false;
    }
    void MaybeStartDtlsInStun() override {
    }
    webrtc::RTCError SetRemoteParameters(absl::string_view digest_alg, const uint8_t *digest, size_t digest_len, std::optional<webrtc::SSLRole> role) override {
        return webrtc::RTCError::OK();
    }
    webrtc::IceTransportInternal *ice_transport() override {
        return nullptr;
    }

private:
    bool _writable = false;
    std::string _transportName;
};

std::unique_ptr<tgcalls::SctpDataChannelProviderInterfaceImpl> makeProvider(
    FakeDtlsTransport *transport,
    std::function<void()> onTerminated,
    std::shared_ptr<tgcalls::Threads> threads
) {
    return std::make_unique<tgcalls::SctpDataChannelProviderInterfaceImpl>(
        webrtc::CreateEnvironment(),
        transport,
        true,
        [](bool) {},
        std::move(onTerminated),
        [](std::string const &) {},
        threads
    );
}

// The custom group engine restarts its data channel when the transport closes; the new channel must still hear the
// next close.
void TestRestartOnCloseKeepsTheNewChannelsCloseCallback(webrtc::Thread *networkThread, std::shared_ptr<tgcalls::Threads> threads) {
    FakeDtlsTransport transport;
    std::unique_ptr<tgcalls::SctpDataChannelProviderInterfaceImpl> provider;
    int terminations = 0;
    std::function<void()> restart;
    restart = [&]() {
        provider.reset();
        provider = makeProvider(&transport, [&]() {
            terminations++;
            restart();
        }, threads);
    };

    networkThread->BlockingCall([&]() {
        restart();
        transport.close();
    });
    networkThread->BlockingCall([]() {});
    CHECK_TRUE(terminations == 1);

    networkThread->BlockingCall([&]() {
        transport.close();
    });
    networkThread->BlockingCall([]() {});
    CHECK_TRUE(terminations == 2);

    networkThread->BlockingCall([&]() {
        provider.reset();
    });
}

// A destroyed channel leaves nothing behind on the transport: its writable state, packets, DTLS state and closing reach
// no one. Each of the four reaches the destroyed DcSctpTransport's freed (poisoned) memory if its registration stayed.
void TestDestroyedChannelLeavesNoCallbacksOnTheTransport(webrtc::Thread *networkThread, std::shared_ptr<tgcalls::Threads> threads) {
    FakeDtlsTransport transport;
    int terminations = 0;

    networkThread->BlockingCall([&]() {
        std::unique_ptr<tgcalls::SctpDataChannelProviderInterfaceImpl> provider = makeProvider(&transport, [&]() {
            terminations++;
        }, threads);
        {
            PoisonedFrees poisoned;
            provider.reset();
        }

        transport.setWritable(true);
        transport.receivePacket();
        transport.setDtlsState(webrtc::DtlsTransportState::kNew);
        transport.close();
    });
    networkThread->BlockingCall([]() {});

    CHECK_TRUE(terminations == 0);
}

}  // namespace

int main() {
    std::signal(SIGSEGV, onFault);
    std::signal(SIGBUS, onFault);

    std::unique_ptr<webrtc::Thread> networkThread = webrtc::Thread::CreateWithSocketServer();
    networkThread->Start();
    std::shared_ptr<tgcalls::Threads> threads = std::make_shared<TestThreads>(networkThread.get());

    TestRestartOnCloseKeepsTheNewChannelsCloseCallback(networkThread.get(), threads);
    TestDestroyedChannelLeavesNoCallbacksOnTheTransport(networkThread.get(), threads);

    networkThread->Stop();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
