#include "NetworkManager.h"

#include "Message.h"

#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/base/p2p_transport_channel.h"
#include "p2p/base/basic_async_resolver_factory.h"
#include "api/packet_socket_factory.h"
#include "rtc_base/task_utils/to_queued_task.h"
#include "p2p/base/ice_credentials_iterator.h"
#include "api/jsep_ice_candidate.h"

extern "C" {
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/modes.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
} // extern "C"

namespace tgcalls {

static void KDF2(unsigned char *encryptionKey, unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv) {
	uint8_t sA[32], sB[32];
	uint8_t buf[16 + 36];
	memcpy(buf, msgKey, 16);
	memcpy(buf + 16, encryptionKey + x, 36);
	SHA256(buf, 16 + 36, sA);
	memcpy(buf, encryptionKey + 40 + x, 36);
	memcpy(buf + 36, msgKey, 16);
	SHA256(buf, 36 + 16, sB);
	memcpy(aesKey, sA, 8);
	memcpy(aesKey + 8, sB + 8, 16);
	memcpy(aesKey + 8 + 16, sA + 24, 8);
	memcpy(aesIv, sB, 8);
	memcpy(aesIv + 8, sA + 8, 16);
	memcpy(aesIv + 8 + 16, sB + 24, 8);
}

static void aesIgeEncrypt(uint8_t *in, uint8_t *out, size_t length, uint8_t *key, uint8_t *iv) {
	AES_KEY akey;
	AES_set_encrypt_key(key, 32*8, &akey);
	AES_ige_encrypt(in, out, length, &akey, iv, AES_ENCRYPT);
}

static void aesIgeDecrypt(uint8_t *in, uint8_t *out, size_t length, uint8_t *key, uint8_t *iv) {
	AES_KEY akey;
	AES_set_decrypt_key(key, 32*8, &akey);
	AES_ige_encrypt(in, out, length, &akey, iv, AES_DECRYPT);
}

NetworkManager::NetworkManager(
	rtc::Thread *thread,
	EncryptionKey encryptionKey,
	bool enableP2P,
	std::vector<RtcServer> const &rtcServers,
	std::function<void(const NetworkManager::State &)> stateUpdated,
	std::function<void(DecryptedMessage &&)> transportMessageReceived,
	std::function<void(Message &&)> sendSignalingMessage) :
_thread(thread),
_transport(EncryptedConnection::Type::Transport, encryptionKey),
_isOutgoing(encryptionKey.isOutgoing),
_stateUpdated(std::move(stateUpdated)),
_transportMessageReceived(std::move(transportMessageReceived)),
_sendSignalingMessage(std::move(sendSignalingMessage)) {
	assert(_thread->IsCurrent());

	_socketFactory.reset(new rtc::BasicPacketSocketFactory(_thread));

	_networkManager = std::make_unique<rtc::BasicNetworkManager>();
	_portAllocator.reset(new cricket::BasicPortAllocator(_networkManager.get(), _socketFactory.get(), nullptr, nullptr));

	uint32_t flags = cricket::PORTALLOCATOR_DISABLE_TCP;
	if (!enableP2P) {
		flags |= cricket::PORTALLOCATOR_DISABLE_UDP;
		flags |= cricket::PORTALLOCATOR_DISABLE_STUN;
	}
	_portAllocator->set_flags(_portAllocator->flags() | flags);
	_portAllocator->Initialize();

	cricket::ServerAddresses stunServers;
	std::vector<cricket::RelayServerConfig> turnServers;

	if (rtcServers.size() == 0) {
		rtc::SocketAddress defaultStunAddress = rtc::SocketAddress("134.122.52.178", 3478);
		stunServers.insert(defaultStunAddress);

		turnServers.push_back(cricket::RelayServerConfig(
			rtc::SocketAddress("134.122.52.178", 3478),
			"openrelay",
			"openrelay",
			cricket::PROTO_UDP
		));
	} else {
		for (auto &server : rtcServers) {
			if (server.isTurn) {
				turnServers.push_back(cricket::RelayServerConfig(
					rtc::SocketAddress(server.host, server.port),
					server.login,
					server.password,
					cricket::PROTO_UDP
				));
			} else {
				rtc::SocketAddress stunAddress = rtc::SocketAddress(server.host, server.port);
				stunServers.insert(stunAddress);
			}
		}
	}

	_portAllocator->SetConfiguration(stunServers, turnServers, 2, webrtc::NO_PRUNE);

	_asyncResolverFactory = std::make_unique<webrtc::BasicAsyncResolverFactory>();
	_transportChannel.reset(new cricket::P2PTransportChannel("transport", 0, _portAllocator.get(), _asyncResolverFactory.get(), nullptr));

	cricket::IceConfig iceConfig;
	iceConfig.continual_gathering_policy = cricket::GATHER_CONTINUALLY;
	_transportChannel->SetIceConfig(iceConfig);

	cricket::IceParameters localIceParameters(
		"gcp3",
		"zWDKozH8/3JWt8he3M/CMj5R",
		false
	);
	cricket::IceParameters remoteIceParameters(
		"acp3",
		"aWDKozH8/3JWt8he3M/CMj5R",
		false
	);

	_transportChannel->SetIceParameters(_isOutgoing ? localIceParameters : remoteIceParameters);
	_transportChannel->SetIceRole(_isOutgoing ? cricket::ICEROLE_CONTROLLING : cricket::ICEROLE_CONTROLLED);

	_transportChannel->SignalCandidateGathered.connect(this, &NetworkManager::candidateGathered);
	_transportChannel->SignalGatheringState.connect(this, &NetworkManager::candidateGatheringState);
	_transportChannel->SignalIceTransportStateChanged.connect(this, &NetworkManager::transportStateChanged);
	_transportChannel->SignalReadPacket.connect(this, &NetworkManager::transportPacketReceived);

	_transportChannel->MaybeStartGathering();

	_transportChannel->SetRemoteIceMode(cricket::ICEMODE_FULL);
	_transportChannel->SetRemoteIceParameters(_isOutgoing ? remoteIceParameters : localIceParameters);
}

NetworkManager::~NetworkManager() {
	assert(_thread->IsCurrent());

	_transportChannel.reset();
	_asyncResolverFactory.reset();
	_portAllocator.reset();
	_networkManager.reset();
	_socketFactory.reset();
}

void NetworkManager::receiveSignalingMessage(DecryptedMessage &&message) {
	const auto list = absl::get_if<CandidatesListMessage>(&message.message.data);
	assert(list != nullptr);

	for (const auto &candidate : list->candidates) {
		_transportChannel->AddRemoteCandidate(candidate);
	}
}

uint32_t NetworkManager::sendMessage(const Message &message) {
	if (auto prepared = _transport.prepareForSending(message)) {
		rtc::PacketOptions packetOptions;
		_transportChannel->SendPacket((const char *)prepared->bytes.data(), prepared->bytes.size(), packetOptions, 0);
		return prepared->counter;
	}
	return 0;
}

void NetworkManager::candidateGathered(cricket::IceTransportInternal *transport, const cricket::Candidate &candidate) {
	assert(_thread->IsCurrent());
	_sendSignalingMessage({ CandidatesListMessage{ std::vector<cricket::Candidate>(1, candidate) } });
}

void NetworkManager::candidateGatheringState(cricket::IceTransportInternal *transport) {
	assert(_thread->IsCurrent());
}

void NetworkManager::transportStateChanged(cricket::IceTransportInternal *transport) {
	assert(_thread->IsCurrent());

	auto state = transport->GetIceTransportState();
	bool isConnected = false;
	switch (state) {
		case webrtc::IceTransportState::kConnected:
		case webrtc::IceTransportState::kCompleted:
			isConnected = true;
			break;
		default:
			break;
	}
	NetworkManager::State emitState;
	emitState.isReadyToSendData = isConnected;
	_stateUpdated(emitState);
}

void NetworkManager::transportReadyToSend(cricket::IceTransportInternal *transport) {
	assert(_thread->IsCurrent());
}

void NetworkManager::transportPacketReceived(rtc::PacketTransportInternal *transport, const char *bytes, size_t size, const int64_t &timestamp, int unused) {
	assert(_thread->IsCurrent());

	if (auto decrypted = _transport.handleIncomingPacket(bytes, size)) {
		if (_transportMessageReceived) {
			_transportMessageReceived(std::move(decrypted->main));
			for (auto &message : decrypted->additional) {
				_transportMessageReceived(std::move(message));
			}
		}
	}
}

} // namespace tgcalls
