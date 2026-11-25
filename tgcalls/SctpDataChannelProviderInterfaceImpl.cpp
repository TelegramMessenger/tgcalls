#include "SctpDataChannelProviderInterfaceImpl.h"

#include "api/priority.h"
#include "api/sctp_transport_interface.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "media/sctp/dcsctp_transport.h"
#include "api/environment/deprecated_global_field_trials.h"
#include "FieldTrialsConfig.h"

namespace tgcalls {

SctpDataChannelProviderInterfaceImpl::SctpDataChannelProviderInterfaceImpl(
    const webrtc::Environment &env,
    webrtc::DtlsTransportInternal *transportChannel,
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
_onMessageReceived(onMessageReceived) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _transportChannel = transportChannel;
    _sctpTransportFactory.reset(new webrtc::SctpTransportFactory(_threads->getNetworkThread()));
    _sctpTransport = _sctpTransportFactory->CreateSctpTransport(env, transportChannel);
    _sctpTransport->SetDataChannelSink(this);

    // TODO: should we disconnect the data channel sink?

    webrtc::InternalDataChannelInit dataChannelInit;
    dataChannelInit.id = 0;
    dataChannelInit.open_handshake_role = isOutgoing ? webrtc::InternalDataChannelInit::kOpener : webrtc::InternalDataChannelInit::kAcker;
    
    // As WebRTC's DataChannelController does: the stream is opened before the channel that uses it is created.
    AddSctpDataStream(webrtc::StreamId(0), webrtc::PriorityValue(webrtc::Priority::kLow));

    _dataChannel = webrtc::SctpDataChannel::Create(
        _weakFactory.GetWeakPtr(),
        0,
        "data",
        true,
        dataChannelInit,
        std::nullopt,
        _safety.flag(),
        _threads->getNetworkThread(),
        _threads->getNetworkThread()
    );

    _dataChannel->RegisterObserver(this);
}

SctpDataChannelProviderInterfaceImpl::~SctpDataChannelProviderInterfaceImpl() {
    assert(_threads->getNetworkThread()->IsCurrent());
    
    _weakFactory.InvalidateWeakPtrs();

    _dataChannel->UnregisterObserver();
    _dataChannel->Close();
    _dataChannel = nullptr;

    if (_sctpTransport) {
        // Nothing the transport does while it goes (its socket closes) may reach this half-destroyed provider.
        _sctpTransport->SetDataChannelSink(nullptr);
    }
    if (_sctpTransport && _transportChannel) {
        // DcSctpTransport subscribes to the DTLS transport under its own address and never unsubscribes (its
        // DisconnectTransportSignals has no caller in M159; M123's sigslot disconnected it on destruction), so its
        // callbacks would outlive it on a transport that stays. Its registrations, as DisconnectTransportSignals
        // removes them; SctpTransportFactory makes only DcSctpTransport. dcsctp_registrations_test keeps this list equal
        // to DcSctpTransport::ConnectTransportSignals; sctp_data_channel_provider_test fails if one is missing.
        void *tag = static_cast<webrtc::DcSctpTransport *>(_sctpTransport.get());
        _transportChannel->UnsubscribeWritableState(tag);
        _transportChannel->DeregisterReceivedPacketCallback(tag);
        _transportChannel->SetOnCloseCallback(nullptr);
        _transportChannel->UnsubscribeDtlsTransportState(tag);
    }
    _sctpTransport = nullptr;
    _sctpTransportFactory.reset();
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
            webrtc::SctpOptions options;
            options.local_port = 5000;
            options.remote_port = 5000;
            options.max_message_size = 262144;
            _sctpTransport->Start(options);
        }
    }
}

void SctpDataChannelProviderInterfaceImpl::OnReadyToSend() {
    assert(_threads->getNetworkThread()->IsCurrent());

    _dataChannel->OnTransportReady();
}

void SctpDataChannelProviderInterfaceImpl::OnTransportClosed(webrtc::RTCError error) {
    assert(_threads->getNetworkThread()->IsCurrent());

    // Later, outside the transport's notification: a restart that destroys this object and makes a new channel on
    // the same DTLS transport must not run inside the old SCTP transport's callbacks, nor inside the DTLS transport's
    // close callback, which the transport clears when it returns (the new channel's with it).
    _threads->getNetworkThread()->PostTask(webrtc::SafeTask(_safety.flag(), [this]() {
        if (_onTerminated) {
            _onTerminated();
        }
    }));
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

    if (!_sctpTransport->OpenStream(sid.stream_id_int(), priority)) {
        return webrtc::RTCError(webrtc::RTCErrorType::INTERNAL_ERROR, "Failed to open the SCTP stream");
    }
    return webrtc::RTCError::OK();
}

size_t SctpDataChannelProviderInterfaceImpl::buffered_amount(webrtc::StreamId sid) const {
    assert(_threads->getNetworkThread()->IsCurrent());

    return _sctpTransport->buffered_amount(sid.stream_id_int());
}

size_t SctpDataChannelProviderInterfaceImpl::buffered_amount_low_threshold(webrtc::StreamId sid) const {
    assert(_threads->getNetworkThread()->IsCurrent());

    return _sctpTransport->buffered_amount_low_threshold(sid.stream_id_int());
}

void SctpDataChannelProviderInterfaceImpl::SetBufferedAmountLowThreshold(webrtc::StreamId sid, size_t bytes) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _sctpTransport->SetBufferedAmountLowThreshold(sid.stream_id_int(), bytes);
}

void SctpDataChannelProviderInterfaceImpl::OnBufferedAmountLow(int channel_id) {
    assert(_threads->getNetworkThread()->IsCurrent());

    if (channel_id == 0) {
        _dataChannel->OnBufferedAmountLow();
    }
}

void SctpDataChannelProviderInterfaceImpl::OnMaxMessageSize(int max_message_size) {
    assert(_threads->getNetworkThread()->IsCurrent());

    _dataChannel->OnMaxMessageSize(max_message_size);
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
