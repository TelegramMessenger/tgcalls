#include "ChannelManager.h"
#include <utility>
#include "absl/algorithm/container.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "api/media_types.h"
#include "api/rtp_packet_infos.h"
#include "api/sequence_checker.h"
#include "api/units/timestamp.h"
#include "media/base/media_constants.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/trace_event.h"
namespace tgcalls {
// static
namespace {

// The receive channels call this for every delivered frame, unguarded (RtpTransceiver updates its receivers' sources
// from it); the custom engines keep no receivers.
absl::AnyInvocable<void(uint32_t, const webrtc::RtpPacketInfos&, webrtc::Timestamp) const> ignoreDeliveredFrame() {
  return [](uint32_t, const webrtc::RtpPacketInfos&, webrtc::Timestamp) {};
}

}  // namespace

void disablePayloadTypeDemuxing(webrtc::BaseChannel* channel) {
  if (webrtc::RtpTransportInternal* rtp_transport = channel->rtp_transport()) {
    rtp_transport->SetActivePayloadTypeDemuxing(false);
  }
  channel->media_receive_channel()->ResetUnsignaledRecvStream();
}

void setLocalContent(webrtc::BaseChannel* channel, const webrtc::MediaContentDescription* content, webrtc::SdpType type) {
  const webrtc::RTCError error = channel->SetLocalContent(content, type);
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to set the local content of " << channel->mid() << ": " << error.message();
  }
}

void setRemoteContent(webrtc::BaseChannel* channel, const webrtc::MediaContentDescription* content, webrtc::SdpType type) {
  const webrtc::RTCError error = channel->SetRemoteContent(content, type);
  if (!error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to set the remote content of " << channel->mid() << ": " << error.message();
  }
}

std::unique_ptr<ChannelManager> ChannelManager::Create(
    const webrtc::Environment& env,
    std::unique_ptr<webrtc::MediaEngineInterface> media_engine,
    webrtc::Thread* worker_thread,
    webrtc::Thread* network_thread) {
  RTC_DCHECK(network_thread);
  RTC_DCHECK(worker_thread);
  return absl::WrapUnique(new ChannelManager(
      env, std::move(media_engine), worker_thread, network_thread));
}
ChannelManager::ChannelManager(
    const webrtc::Environment& env,
    std::unique_ptr<webrtc::MediaEngineInterface> media_engine,
    webrtc::Thread* worker_thread,
    webrtc::Thread* network_thread)
    : env_(env),
      media_engine_(std::move(media_engine)),
      signaling_thread_(webrtc::Thread::Current()),
      worker_thread_(worker_thread),
      network_thread_(network_thread) {
  RTC_DCHECK_RUN_ON(signaling_thread_);
  RTC_DCHECK(worker_thread_);
  RTC_DCHECK(network_thread_);
  if (media_engine_) {
    // TODO(tommi): Change VoiceEngine to do ctor time initialization so that
    // this isn't necessary.
    worker_thread_->BlockingCall([&] { media_engine_->Init(); });
  }
}
ChannelManager::~ChannelManager() {
  RTC_DCHECK_RUN_ON(signaling_thread_);
  worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    RTC_DCHECK(voice_channels_.empty());
    RTC_DCHECK(video_channels_.empty());
    if (media_engine_) {
      // The pair of the constructor's Init: since M159 the voice engine's destructor no longer stops the audio device,
      // so without it playout and recording would go on with a callback into freed audio state.
      media_engine_->Terminate();
    }
    // While `media_engine_` is const throughout the ChannelManager's lifetime,
    // it requires destruction to happen on the worker thread. Instead of
    // marking the pointer as non-const, we live with this const_cast<> in the
    // destructor.
    const_cast<std::unique_ptr<webrtc::MediaEngineInterface>&>(media_engine_).reset();
  });
}
VoiceChannel* ChannelManager::CreateVoiceChannel(
    webrtc::Call* call,
    const webrtc::MediaConfig& media_config,
    const std::string& mid,
    bool srtp_required,
    const webrtc::CryptoOptions& crypto_options,
    const webrtc::AudioOptions& options) {
  RTC_DCHECK(call);
  RTC_DCHECK(media_engine_);
  // TODO(bugs.webrtc.org/11992): Remove this workaround after updates in
  // PeerConnection and add the expectation that we're already on the right
  // thread.
  if (!worker_thread_->IsCurrent()) {
    VoiceChannel* temp = nullptr;
    worker_thread_->BlockingCall([&] {
      temp = CreateVoiceChannel(call, media_config, mid, srtp_required,
                                crypto_options, options);
    });
    return temp;
  }
  RTC_DCHECK_RUN_ON(worker_thread_);
  // The channel's audio processing options (echo cancellation, noise suppression, AGC, high-pass filter) apply to the
  // engine's one audio processing module. M123's voice channel applied them itself when it was created; M159's never
  // does (only Init and ApplyGlobalOptions do, the latter from PeerConnectionFactory::CreateAudioSource, which the
  // custom engines do not use), so they are applied here, as M123 did. An option a device reports built in goes to the
  // device (the app's shared call audio device reports all three); ApplyOptions' platform adjustments run again too
  // (on iOS echo cancellation and AGC are forced off), which only repeats Init's state.
  media_engine_->voice().ApplyGlobalOptions(options);
  std::unique_ptr<webrtc::VoiceMediaSendChannelInterface> send_media_channel = media_engine_->voice().CreateSendChannel(
      env_, call, media_config, options, crypto_options);
  if (!send_media_channel) {
    return nullptr;
  }
  std::unique_ptr<webrtc::VoiceMediaReceiveChannelInterface> receive_media_channel = media_engine_->voice().CreateReceiveChannel(
        env_, call, media_config, options, crypto_options, nullptr, ignoreDeliveredFrame());
  if (!receive_media_channel) {
    return nullptr;
  }
  auto voice_channel = std::make_unique<VoiceChannel>(
      network_thread_, signaling_thread_,
      std::move(send_media_channel), std::move(receive_media_channel), mid, webrtc::MediaType::AUDIO, srtp_required,
      crypto_options, &ssrc_generator_);
  VoiceChannel* voice_channel_ptr = voice_channel.get();
  voice_channels_.push_back(std::move(voice_channel));
  return voice_channel_ptr;
}
void ChannelManager::DestroyVoiceChannel(VoiceChannel* channel) {
  TRACE_EVENT0("webrtc", "ChannelManager::DestroyVoiceChannel");
  RTC_DCHECK_RUN_ON(worker_thread_);
  voice_channels_.erase(absl::c_find_if(
      voice_channels_, [&](const auto& p) { return p.get() == channel; }));
}
VideoChannel* ChannelManager::CreateVideoChannel(
    webrtc::Call* call,
    const webrtc::MediaConfig& media_config,
    const std::string& mid,
    bool srtp_required,
    const webrtc::CryptoOptions& crypto_options,
    const webrtc::VideoOptions& options,
    webrtc::VideoBitrateAllocatorFactory* video_bitrate_allocator_factory) {
  RTC_DCHECK(call);
  RTC_DCHECK(media_engine_);
  // TODO(bugs.webrtc.org/11992): Remove this workaround after updates in
  // PeerConnection and add the expectation that we're already on the right
  // thread.
  if (!worker_thread_->IsCurrent()) {
    VideoChannel* temp = nullptr;
    worker_thread_->BlockingCall([&] {
      temp = CreateVideoChannel(call, media_config, mid, srtp_required,
                                crypto_options, options,
                                video_bitrate_allocator_factory);
    });
    return temp;
  }
  RTC_DCHECK_RUN_ON(worker_thread_);
  std::unique_ptr<webrtc::VideoMediaSendChannelInterface> send_media_channel = media_engine_->video().CreateSendChannel(
      env_, call, media_config, options, crypto_options,
      video_bitrate_allocator_factory, nullptr, nullptr);
  if (!send_media_channel) {
    return nullptr;
  }
  std::unique_ptr<webrtc::VideoMediaReceiveChannelInterface> receive_media_channel = media_engine_->video().CreateReceiveChannel(
    env_, call, media_config, crypto_options, nullptr, ignoreDeliveredFrame());
  if (!receive_media_channel) {
    return nullptr;
  }
  auto video_channel = std::make_unique<VideoChannel>(
      network_thread_, signaling_thread_,
      std::move(send_media_channel), std::move(receive_media_channel), mid, webrtc::MediaType::VIDEO, srtp_required,
      crypto_options, &ssrc_generator_);
  VideoChannel* video_channel_ptr = video_channel.get();
  video_channels_.push_back(std::move(video_channel));
  return video_channel_ptr;
}
void ChannelManager::DestroyVideoChannel(VideoChannel* channel) {
  TRACE_EVENT0("webrtc", "ChannelManager::DestroyVideoChannel");
  RTC_DCHECK_RUN_ON(worker_thread_);
  video_channels_.erase(absl::c_find_if(
      video_channels_, [&](const auto& p) { return p.get() == channel; }));
}
void ChannelManager::DestroyChannel(webrtc::ChannelInterface* channel) {
  RTC_DCHECK(channel);
  if (!worker_thread_->IsCurrent()) {
    // TODO(tommi): Do this asynchronously when we have a way to make sure that
    // the call to DestroyChannel runs before ~Call() runs, which today happens
    // inside an Invoke from the signaling thread in PeerConnectin::Close().
    worker_thread_->BlockingCall([&] { DestroyChannel(channel); });
    return;
  }
  if (channel->media_type() == webrtc::MediaType::AUDIO) {
    DestroyVoiceChannel(static_cast<VoiceChannel*>(channel));
  } else {
    RTC_DCHECK_EQ(channel->media_type(), webrtc::MediaType::VIDEO);
    DestroyVideoChannel(static_cast<VideoChannel*>(channel));
  }
}
}  // namespace tgcalls
