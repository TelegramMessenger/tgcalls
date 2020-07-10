#ifndef TGCALLS_NETWORK_MANAGER_H
#define TGCALLS_NETWORK_MANAGER_H

#include "rtc_base/thread.h"

#include <functional>
#include <memory>

#include "rtc_base/copy_on_write_buffer.h"
#include "api/candidate.h"
#include "Instance.h"
#include "SignalingMessage.h"

namespace rtc {
class BasicPacketSocketFactory;
class BasicNetworkManager;
class PacketTransportInternal;
} // namespace rtc

namespace cricket {
class BasicPortAllocator;
class P2PTransportChannel;
class IceTransportInternal;
} // namespace cricket

namespace webrtc {
class BasicAsyncResolverFactory;
} // namespace webrtc

namespace tgcalls {

struct SignalingMessage;

class NetworkManager : public sigslot::has_slots<> {
public:
	struct State {
		bool isReadyToSendData = false;
	};

public:
	NetworkManager(
		rtc::Thread *thread,
		EncryptionKey encryptionKey,
		bool enableP2P,
		std::vector<RtcServer> const &rtcServers,
		std::function<void (const NetworkManager::State &)> stateUpdated,
		std::function<void (const rtc::CopyOnWriteBuffer &)> packetReceived,
		std::function<void (const SignalingMessage &)> signalingMessageEmitted);
	~NetworkManager();

	void receiveSignalingMessage(SignalingMessage &&message);
	void sendPacket(const rtc::CopyOnWriteBuffer &packet);

private:
	rtc::Thread *_thread;
	EncryptionKey _encryptionKey;
	std::function<void (const NetworkManager::State &)> _stateUpdated;
	std::function<void (const rtc::CopyOnWriteBuffer &)> _packetReceived;
	std::function<void (const SignalingMessage &)> _signalingMessageEmitted;

	std::unique_ptr<rtc::BasicPacketSocketFactory> _socketFactory;
	std::unique_ptr<rtc::BasicNetworkManager> _networkManager;
	std::unique_ptr<cricket::BasicPortAllocator> _portAllocator;
	std::unique_ptr<webrtc::BasicAsyncResolverFactory> _asyncResolverFactory;
	std::unique_ptr<cricket::P2PTransportChannel> _transportChannel;

private:
	void candidateGathered(cricket::IceTransportInternal *transport, const cricket::Candidate &candidate);
	void candidateGatheringState(cricket::IceTransportInternal *transport);
	void transportStateChanged(cricket::IceTransportInternal *transport);
	void transportReadyToSend(cricket::IceTransportInternal *transport);
	void transportPacketReceived(rtc::PacketTransportInternal *transport, const char *bytes, size_t size, const int64_t &timestamp, int unused);

};

} // namespace tgcalls

#endif
