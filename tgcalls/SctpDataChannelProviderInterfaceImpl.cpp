#include "SctpDataChannelProviderInterfaceImpl.h"

#include "p2p/base/dtls_transport.h"
#include "api/environment/environment_factory.h"
#include "FieldTrialsConfig.h"

namespace tgcalls {

SctpDataChannelProviderInterfaceImpl::SctpDataChannelProviderInterfaceImpl(
    webrtc::PacketTransportInternal *transportChannel,
    bool isOutgoing,
    std::function<void(bool)> onStateChanged,
    std::function<void()> onTerminated,
    std::function<void(std::string const &)> onMessageReceived,
    std::shared_ptr<Threads> threads
) :
_weakFactory(this),
_threads(std::move(threads)),
_onStateChanged(onStateChanged),
_onTerminated(onTerminated),
_onMessageReceived(onMessageReceived),
_environment(webrtc::CreateEnvironment()) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _sctpTransportFactory.reset(new webrtc::SctpTransportFactory(_threads->getNetworkThread()));
    _dtlsTransportAdapter.reset(new TgcallDtlsTransportAdapter(transportChannel));

    _sctpTransport = _sctpTransportFactory->CreateSctpTransport(_environment, _dtlsTransportAdapter.get());
    _sctpTransport->SetDataChannelSink(this);

    // TODO: should we disconnect the data channel sink?

    webrtc::InternalDataChannelInit dataChannelInit;
    dataChannelInit.id = 0;
    dataChannelInit.open_handshake_role = isOutgoing ? webrtc::InternalDataChannelInit::kOpener : webrtc::InternalDataChannelInit::kAcker;
    
    _dataChannel = webrtc::SctpDataChannel::Create(
        _weakFactory.GetWeakPtr(),
        "data",
        true,
        dataChannelInit,
        std::optional<int>(262144),
        webrtc::PendingTaskSafetyFlag::Create(),
        _threads->getNetworkThread(),
        _threads->getNetworkThread()
    );

    _dataChannel->RegisterObserver(this);
    
    AddSctpDataStream(webrtc::StreamId(0), webrtc::PriorityValue(webrtc::Priority::kMedium));
}

SctpDataChannelProviderInterfaceImpl::~SctpDataChannelProviderInterfaceImpl() {
    assert(_threads->getNetworkThread()->IsCurrent());
    
    _weakFactory.InvalidateWeakPtrs();

    _dataChannel->UnregisterObserver();
    _dataChannel->Close();
    _dataChannel = nullptr;

    _sctpTransport = nullptr;
    _sctpTransportFactory.reset();
}

bool SctpDataChannelProviderInterfaceImpl::IsOkToCallOnTheNetworkThread() {
    return true;
}

void SctpDataChannelProviderInterfaceImpl::sendDataChannelMessage(std::string const &message) {
    assert(_threads->getNetworkThread()->IsCurrent());

    if (_isDataChannelOpen) {
        RTC_LOG(LS_INFO) << "Outgoing DataChannel message: " << message;

        webrtc::DataBuffer buffer(message);
        _dataChannel->Send(buffer);
    } else {
        RTC_LOG(LS_INFO) << "Could not send an outgoing DataChannel message: the channel is not open";
    }
}

void SctpDataChannelProviderInterfaceImpl::OnStateChange() {
    assert(_threads->getNetworkThread()->IsCurrent());

    auto state = _dataChannel->state();
    bool isDataChannelOpen = state == webrtc::DataChannelInterface::DataState::kOpen;
    if (_isDataChannelOpen != isDataChannelOpen) {
        _isDataChannelOpen = isDataChannelOpen;
        _onStateChanged(_isDataChannelOpen);
    }
}

void SctpDataChannelProviderInterfaceImpl::OnMessage(const webrtc::DataBuffer& buffer) {
    assert(_threads->getNetworkThread()->IsCurrent());

    if (!buffer.binary) {
        std::string messageText(buffer.data.data(), buffer.data.data() + buffer.data.size());
        RTC_LOG(LS_INFO) << "Incoming DataChannel message: " << messageText;

        _onMessageReceived(messageText);
    }
}

void SctpDataChannelProviderInterfaceImpl::updateIsConnected(bool isConnected) {
    assert(_threads->getNetworkThread()->IsCurrent());

    if (isConnected) {
        if (!_isSctpTransportStarted) {
            _isSctpTransportStarted = true;
            webrtc::SctpOptions sctpOptions;
            sctpOptions.local_port = 5000;
            sctpOptions.remote_port = 5000;
            sctpOptions.max_message_size = 262144;
            _sctpTransport->Start(sctpOptions);
        }
    }
}

void SctpDataChannelProviderInterfaceImpl::OnReadyToSend() {
    assert(_threads->getNetworkThread()->IsCurrent());

    _dataChannel->OnTransportReady();
}

void SctpDataChannelProviderInterfaceImpl::OnTransportClosed(webrtc::RTCError error) {
    assert(_threads->getNetworkThread()->IsCurrent());

    if (_onTerminated) {
        _onTerminated();
    }
}

void SctpDataChannelProviderInterfaceImpl::OnTransportConnected() {
    assert(_threads->getNetworkThread()->IsCurrent());
}

void SctpDataChannelProviderInterfaceImpl::OnDataReceived(int channel_id, webrtc::DataMessageType type, const webrtc::CopyOnWriteBuffer& buffer) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _dataChannel->OnDataReceived(type, buffer);
}

webrtc::RTCError SctpDataChannelProviderInterfaceImpl::SendData(
    webrtc::StreamId sid,
    const webrtc::SendDataParams& params,
    const webrtc::CopyOnWriteBuffer& payload
) {
    assert(_threads->getNetworkThread()->IsCurrent());

    return _sctpTransport->SendData(sid.stream_id_int(), params, payload);
}

webrtc::RTCError SctpDataChannelProviderInterfaceImpl::AddSctpDataStream(webrtc::StreamId sid, webrtc::PriorityValue priority) {
  assert(_threads->getNetworkThread()->IsCurrent());

    _sctpTransport->OpenStream(sid.stream_id_int(), priority);
    return webrtc::RTCError::OK();
}

void SctpDataChannelProviderInterfaceImpl::RemoveSctpDataStream(webrtc::StreamId sid) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _threads->getNetworkThread()->BlockingCall([this, sid]() {
        _sctpTransport->ResetStream(sid.stream_id_int());
    });
}

void SctpDataChannelProviderInterfaceImpl::OnChannelStateChanged(webrtc::SctpDataChannel *data_channel, webrtc::DataChannelInterface::DataState state) {
    
}

}
