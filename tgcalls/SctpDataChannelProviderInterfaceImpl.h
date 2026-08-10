#ifndef TGCALLS_SCTP_DATA_CHANNEL_PROVIDER_IMPL_H
#define TGCALLS_SCTP_DATA_CHANNEL_PROVIDER_IMPL_H

#include "rtc_base/weak_ptr.h"
#include "api/turn_customizer.h"
#include "api/data_channel_interface.h"
#include "api/environment/environment.h"
#include "api/priority.h"
#include "api/transport/data_channel_transport_interface.h"
#include "pc/sctp_data_channel.h"
#include "media/sctp/sctp_transport_factory.h"
#include "rtc_base/third_party/sigslot/sigslot.h"

#include "StaticThreads.h"

namespace webrtc {
class DtlsTransport;
} // namespace webrtc

namespace tgcalls {

class SctpDataChannelProviderInterfaceImpl : public sigslot::has_slots<>, public webrtc::SctpDataChannelControllerInterface, public webrtc::DataChannelObserver, public webrtc::DataChannelSink {
public:
    SctpDataChannelProviderInterfaceImpl(
        webrtc::PacketTransportInternal *transportChannel,
        bool isOutgoing,
        std::function<void(bool)> onStateChanged,
        std::function<void()> onTerminated,
        std::function<void(std::string const &)> onMessageReceived,
        std::shared_ptr<Threads> threads
    );
    virtual ~SctpDataChannelProviderInterfaceImpl();

    virtual bool IsOkToCallOnTheNetworkThread() override;
    
    void updateIsConnected(bool isConnected);
    void sendDataChannelMessage(std::string const &message);

    virtual void OnStateChange() override;
    virtual void OnMessage(const webrtc::DataBuffer& buffer) override;
    virtual webrtc::RTCError SendData(
        webrtc::StreamId sid,
        const webrtc::SendDataParams& params,
        const webrtc::CopyOnWriteBuffer& payload) override;
    
    virtual webrtc::RTCError AddSctpDataStream(webrtc::StreamId sid, webrtc::PriorityValue priority) override;
    virtual void RemoveSctpDataStream(webrtc::StreamId sid) override;
    virtual void OnChannelStateChanged(webrtc::SctpDataChannel *data_channel, webrtc::DataChannelInterface::DataState state) override;
    virtual size_t buffered_amount(webrtc::StreamId sid) const override { return 0; }
    virtual size_t buffered_amount_low_threshold(webrtc::StreamId sid) const override { return 0; }
    virtual void SetBufferedAmountLowThreshold(webrtc::StreamId sid, size_t bytes) override {}

    virtual void OnDataReceived(int channel_id,
                                webrtc::DataMessageType type,
                                const webrtc::CopyOnWriteBuffer& buffer) override;
    virtual void OnTransportConnected() override;
    virtual void OnReadyToSend() override;
    virtual void OnTransportClosed(webrtc::RTCError error) override;

    // Unused
    virtual void OnChannelClosing(int channel_id) override{}
    virtual void OnChannelClosed(int channel_id) override{}
    virtual void OnBufferedAmountLow(int channel_id) override{}
    virtual void OnMaxMessageSize(int max_message_size) override{}

private:
    webrtc::WeakPtrFactory<SctpDataChannelProviderInterfaceImpl> _weakFactory;
    std::shared_ptr<Threads> _threads;
    std::function<void(bool)> _onStateChanged;
    std::function<void()> _onTerminated;
    std::function<void(std::string const &)> _onMessageReceived;

    webrtc::Environment _environment;
    std::unique_ptr<webrtc::SctpTransportFactory> _sctpTransportFactory;
    std::unique_ptr<TgcallDtlsTransportAdapter> _dtlsTransportAdapter;
    std::unique_ptr<webrtc::SctpTransportInternal> _sctpTransport;
    webrtc::scoped_refptr<webrtc::SctpDataChannel> _dataChannel;

    bool _isSctpTransportStarted = false;
    bool _isDataChannelOpen = false;

};

} // namespace tgcalls

#endif
