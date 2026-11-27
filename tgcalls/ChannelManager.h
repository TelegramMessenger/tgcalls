#ifndef TGCALLS_CHANNEL_MANAGER_H_
#define TGCALLS_CHANNEL_MANAGER_H_
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>
#include "api/audio_options.h"
#include "api/crypto/crypto_options.h"
#include "api/environment/environment.h"
#include "api/rtp_parameters.h"
#include "api/video/video_bitrate_allocator_factory.h"
#include "call/call.h"
#include "media/base/codec.h"
#include "media/base/media_channel.h"
#include "media/base/media_config.h"
#include "media/base/media_engine.h"
#include "pc/channel.h"
#include "pc/channel_interface.h"
#include "pc/rtp_transport_internal.h"
#include "pc/session_description.h"
#include "rtc_base/system/file_wrapper.h"
#include "rtc_base/thread.h"
#include "rtc_base/thread_annotations.h"
#include "rtc_base/unique_id_generator.h"
namespace tgcalls {

// WebRTC M159 has one channel class for both media types (webrtc::BaseChannel); these keep the custom engines' typed
// accessors, which M123's VoiceChannel and VideoChannel had.
class VoiceChannel final : public webrtc::BaseChannel {
 public:
  using webrtc::BaseChannel::BaseChannel;
  webrtc::VoiceMediaSendChannelInterface* send_channel() { return voice_media_send_channel(); }
  webrtc::VoiceMediaReceiveChannelInterface* receive_channel() { return voice_media_receive_channel(); }
};

class VideoChannel final : public webrtc::BaseChannel {
 public:
  using webrtc::BaseChannel::BaseChannel;
  webrtc::VideoMediaSendChannelInterface* send_channel() { return video_media_send_channel(); }
  webrtc::VideoMediaReceiveChannelInterface* receive_channel() { return video_media_receive_channel(); }
};

// M123's BaseChannel::SetPayloadTypeDemuxingEnabled(false): packets reach the channel by SSRC only, and its
// unsignaled receive streams go. M159 switches payload-type matching per RTP transport
// (RtpTransport::SetActivePayloadTypeDemuxing); every channel of the custom engines turns it off, so their transport
// does too (call this once the channel has its transport). Network thread.
void disablePayloadTypeDemuxing(webrtc::BaseChannel* channel);

// BaseChannel::SetLocalContent / SetRemoteContent, logging a failure (the custom engines never read M123's error
// string). Network thread.
void setLocalContent(webrtc::BaseChannel* channel, const webrtc::MediaContentDescription* content, webrtc::SdpType type);
void setRemoteContent(webrtc::BaseChannel* channel, const webrtc::MediaContentDescription* content, webrtc::SdpType type);

// Creates and owns the custom engines' media channels. Channels are created and destroyed on the worker thread, which
// is the network thread since WebRTC M159 (StaticThreads); Enable() belongs to the thread that created the
// ChannelManager.
class ChannelManager {
 public:
  static std::unique_ptr<ChannelManager> Create(
      const webrtc::Environment& env,
      std::unique_ptr<webrtc::MediaEngineInterface> media_engine,
      webrtc::Thread* worker_thread,
      webrtc::Thread* network_thread);
  ChannelManager() = delete;
  ~ChannelManager();
  webrtc::Thread* worker_thread() const { return worker_thread_; }
  webrtc::Thread* network_thread() const { return network_thread_; }
  webrtc::MediaEngineInterface* media_engine() { return media_engine_.get(); }
  webrtc::UniqueRandomIdGenerator& ssrc_generator() { return ssrc_generator_; }
  // ChannelManager retains ownership of the created channels, so clients should
  // call DestroyChannel when done.
  VoiceChannel* CreateVoiceChannel(webrtc::Call* call,
                                   const webrtc::MediaConfig& media_config,
                                   const std::string& mid,
                                   bool srtp_required,
                                   const webrtc::CryptoOptions& crypto_options,
                                   const webrtc::AudioOptions& options);
  VideoChannel* CreateVideoChannel(
      webrtc::Call* call,
      const webrtc::MediaConfig& media_config,
      const std::string& mid,
      bool srtp_required,
      const webrtc::CryptoOptions& crypto_options,
      const webrtc::VideoOptions& options,
      webrtc::VideoBitrateAllocatorFactory* video_bitrate_allocator_factory);
  void DestroyChannel(webrtc::ChannelInterface* channel);
 protected:
  ChannelManager(const webrtc::Environment& env,
                 std::unique_ptr<webrtc::MediaEngineInterface> media_engine,
                 webrtc::Thread* worker_thread,
                 webrtc::Thread* network_thread);
  void DestroyVoiceChannel(VoiceChannel* voice_channel);
  void DestroyVideoChannel(VideoChannel* video_channel);
 private:
  const webrtc::Environment env_;
  const std::unique_ptr<webrtc::MediaEngineInterface> media_engine_;  // Nullable.
  webrtc::Thread* const signaling_thread_;
  webrtc::Thread* const worker_thread_;
  webrtc::Thread* const network_thread_;
  // This object should be used to generate any SSRC that is not explicitly
  // specified by the user (or by the remote party).
  webrtc::UniqueRandomIdGenerator ssrc_generator_;
  // Vector contents are non-null.
  std::vector<std::unique_ptr<VoiceChannel>> voice_channels_
      RTC_GUARDED_BY(worker_thread_);
  std::vector<std::unique_ptr<VideoChannel>> video_channels_
      RTC_GUARDED_BY(worker_thread_);
};
}  // namespace tgcalls
#endif  // TGCALLS_CHANNEL_MANAGER_H_
