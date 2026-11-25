#ifndef TGCALLS_SCTP_DATA_CHANNEL_PROVIDER_IMPL_H
#define TGCALLS_SCTP_DATA_CHANNEL_PROVIDER_IMPL_H

#include "rtc_base/weak_ptr.h"
#include "api/turn_customizer.h"
#include "api/data_channel_interface.h"
#include "api/environment/environment.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "p2p/dtls/dtls_transport_internal.h"
#include "pc/sctp_data_channel.h"
#include "media/sctp/sctp_transport_factory.h"

#include "StaticThreads.h"

namespace tgcalls {

class SctpDataChannelProviderInterfaceImpl : public webrtc::SctpDataChannelControllerInterface, public webrtc::DataChannelObserver, public webrtc::DataChannelSink {
public:
    // `transportChannel` hands SCTP up as kDtlsDecrypted packets, the only kind WebRTC's DcSctpTransport takes.
    SctpDataChannelProviderInterfaceImpl(
        const webrtc::Environment &env,
        webrtc::DtlsTransportInternal *transportChannel,
        bool isOutgoing,
        std::function<void(bool)> onStateChanged,
        std::function<void()> onTerminated,
        std::function<void(std::string const &)> onMessageReceived,
        std::shared_ptr<Threads> threads
    );
    virtual ~SctpDataChannelProviderInterfaceImpl();

    void updateIsConnected(bool isConnected);
    void sendDataChannelMessage(std::string const &message);

    // webrtc::DataChannelObserver
    virtual void OnStateChange() override;
    virtual void OnMessage(const webrtc::DataBuffer& buffer) override;

    // webrtc::SctpDataChannelControllerInterface
    virtual webrtc::RTCError SendData(
        webrtc::StreamId sid,
        const webrtc::SendDataParams& params,
        const webrtc::CopyOnWriteBuffer& payload) override;
    virtual webrtc::RTCError AddSctpDataStream(webrtc::StreamId sid, webrtc::PriorityValue priority) override;
    virtual void RemoveSctpDataStream(webrtc::StreamId sid) override;
    virtual void OnChannelStateChanged(webrtc::SctpDataChannel *data_channel, webrtc::DataChannelInterface::DataState state) override;
    virtual size_t buffered_amount(webrtc::StreamId sid) const override;
    virtual size_t buffered_amount_low_threshold(webrtc::StreamId sid) const override;
    virtual void SetBufferedAmountLowThreshold(webrtc::StreamId sid, size_t bytes) override;

    // webrtc::DataChannelSink
    // Our one channel's sid is fixed (0): nothing to allocate when the association comes up.
    virtual void OnTransportConnected() override {}
    virtual void OnDataReceived(int channel_id,
                                webrtc::DataMessageType type,
                                const webrtc::CopyOnWriteBuffer& buffer) override;
    virtual void OnReadyToSend() override;
    virtual void OnTransportClosed(webrtc::RTCError error) override;
    virtual void OnBufferedAmountLow(int channel_id) override;
    virtual void OnMaxMessageSize(int max_message_size) override;

    // Unused
    virtual void OnChannelClosing(int channel_id) override{}
    virtual void OnChannelClosed(int channel_id) override{}

private:
    webrtc::WeakPtrFactory<SctpDataChannelProviderInterfaceImpl> _weakFactory;
    std::shared_ptr<Threads> _threads;
    std::function<void(bool)> _onStateChanged;
    std::function<void()> _onTerminated;
    std::function<void(std::string const &)> _onMessageReceived;

    // Outlives this object: the custom engines replace or drop the data channel while the DTLS transport stays.
    webrtc::DtlsTransportInternal *_transportChannel = nullptr;
    std::unique_ptr<webrtc::SctpTransportFactory> _sctpTransportFactory;
    std::unique_ptr<webrtc::SctpTransportInternal> _sctpTransport;
    webrtc::scoped_refptr<webrtc::SctpDataChannel> _dataChannel;
    webrtc::ScopedTaskSafety _safety;

    bool _isSctpTransportStarted = false;
    bool _isDataChannelOpen = false;

};

} // namespace tgcalls

#endif
