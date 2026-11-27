/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#ifndef TGCALLS_CUSTOM_SIMULCAST_ENCODER_ADAPTER_H_
#define TGCALLS_CUSTOM_SIMULCAST_ENCODER_ADAPTER_H_

#include <cstdint>
#include <functional>
#include <atomic>
#include <list>
#include <memory>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include <optional>
#include "api/fec_controller_override.h"
#include "api/sequence_checker.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "common_video/framerate_controller.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/experiments/encoder_info_settings.h"
#include "rtc_base/system/no_unique_address.h"
#include "rtc_base/system/rtc_export.h"
#include "api/environment/environment.h"
#include "api/field_trials_view.h"

namespace webrtc {

// CustomSimulcastEncoderAdapter implements simulcast support by creating multiple
// webrtc::VideoEncoder instances with the given VideoEncoderFactory.
// The object is created and destroyed on the worker thread, but all public
// interfaces should be called from the encoder task queue.
class RTC_EXPORT CustomSimulcastEncoderAdapter : public VideoEncoder {
 public:
  // `primary_factory` produces the first-choice encoders to use.
  // `fallback_factory`, if non-null, is used to create fallback encoder that
  // will be used if InitEncode() fails for the primary encoder.
  // `top_stream_encoder_factory`, if set, makes the encoder of the
  // highest-quality stream whenever there are two or more streams (tgcalls:
  // the group screen share's AV1 top layer beside H264 streams); the other
  // streams, and every stream of a single-stream setup, come from the
  // factories as before. With `top_stream_encoder_for_every_stream` it makes
  // the encoder of every stream, whatever their number (an AV1 sender's
  // simulcast: `format` is AV1, which the factories need not make).
  CustomSimulcastEncoderAdapter(const Environment& env,
                          VideoEncoderFactory* primary_factory,
                          VideoEncoderFactory* hardware_factory,
                          const SdpVideoFormat& format,
                          std::function<std::unique_ptr<VideoEncoder>()>
                              top_stream_encoder_factory = nullptr,
                          bool top_stream_encoder_for_every_stream = false);
  ~CustomSimulcastEncoderAdapter() override;

  // Implements VideoEncoder.
  void SetFecControllerOverride(
      FecControllerOverride* fec_controller_override) override;
  int Release() override;
  int InitEncode(const VideoCodec* codec_settings,
                 const VideoEncoder::Settings& settings) override;
  int Encode(const VideoFrame& input_image,
             const std::vector<VideoFrameType>* frame_types) override;
  int RegisterEncodeCompleteCallback(EncodedImageCallback* callback) override;
  void SetRates(const RateControlParameters& parameters) override;
  void OnPacketLossRateUpdate(float packet_loss_rate) override;
  void OnRttUpdate(int64_t rtt_ms) override;
  void OnLossNotification(const LossNotification& loss_notification) override;

  EncoderInfo GetEncoderInfo() const override;

 private:
  class EncoderContext {
   public:
    EncoderContext(std::unique_ptr<VideoEncoder> encoder,
                   bool prefer_temporal_support,
                   VideoEncoder::EncoderInfo primary_info,
                   VideoEncoder::EncoderInfo fallback_info,
                   bool is_top_stream = false);
    EncoderContext& operator=(EncoderContext&&) = delete;

    VideoEncoder& encoder() { return *encoder_; }
    bool prefer_temporal_support() { return prefer_temporal_support_; }
    // Made by the top-stream encoder factory: reused only for the top stream.
    bool is_top_stream() { return is_top_stream_; }
    void Release();

    const VideoEncoder::EncoderInfo& PrimaryInfo() { return primary_info_; }

    const VideoEncoder::EncoderInfo& FallbackInfo() { return fallback_info_; }

   private:
    std::unique_ptr<VideoEncoder> encoder_;
    bool prefer_temporal_support_;
    bool is_top_stream_;
    const VideoEncoder::EncoderInfo primary_info_;
    const VideoEncoder::EncoderInfo fallback_info_;
  };

  class StreamContext : public EncodedImageCallback {
   public:
    StreamContext(CustomSimulcastEncoderAdapter* parent,
                  std::unique_ptr<EncoderContext> encoder_context,
                  std::unique_ptr<FramerateController> framerate_controller,
                  int stream_idx,
                  uint16_t width,
                  uint16_t height,
                  bool send_stream);
    StreamContext(StreamContext&& rhs);
    StreamContext& operator=(StreamContext&&) = delete;
    ~StreamContext() override;

    Result OnEncodedImage(
        const EncodedImage& encoded_image,
        const CodecSpecificInfo* codec_specific_info) override;
    void OnFrameDropped(uint32_t rtp_timestamp,
                        int spatial_id,
                        bool is_end_of_temporal_unit) override;

    VideoEncoder& encoder() { return encoder_context_->encoder(); }
    const VideoEncoder& encoder() const { return encoder_context_->encoder(); }
    int stream_idx() const { return stream_idx_; }
    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    bool is_keyframe_needed() const {
      return !is_paused_ && is_keyframe_needed_;
    }
    void set_is_keyframe_needed() { is_keyframe_needed_ = true; }
    bool is_paused() const { return is_paused_; }
    void set_is_paused(bool is_paused) { is_paused_ = is_paused; }
    std::optional<double> target_fps() const {
      return framerate_controller_ == nullptr
                 ? std::nullopt
                 : std::optional<double>(
                       framerate_controller_->GetMaxFramerate());
    }

    std::unique_ptr<EncoderContext> ReleaseEncoderContext() &&;
    void OnKeyframe(Timestamp timestamp);
    bool ShouldDropFrame(Timestamp timestamp);

   private:
    CustomSimulcastEncoderAdapter* const parent_;
    std::unique_ptr<EncoderContext> encoder_context_;
    std::unique_ptr<FramerateController> framerate_controller_;
    const int stream_idx_;
    const uint16_t width_;
    const uint16_t height_;
    bool is_keyframe_needed_;
    bool is_paused_;
  };

  bool Initialized() const;

  void DestroyStoredEncoders();

  // This method creates encoder. May reuse previously created encoders from
  // `cached_encoder_contexts_`. It's const because it's used from
  // const GetEncoderInfo().
  std::unique_ptr<EncoderContext> FetchOrCreateEncoderContext(
      bool is_lowest_quality_stream, bool is_highest_quality_stream) const;

  webrtc::VideoCodec MakeStreamCodec(const webrtc::VideoCodec& codec,
                                     int stream_idx,
                                     uint32_t start_bitrate_kbps,
                                     bool is_lowest_quality_stream,
                                     bool is_highest_quality_stream);

  EncodedImageCallback::Result OnEncodedImage(
      size_t stream_idx,
      const EncodedImage& encoded_image,
      const CodecSpecificInfo* codec_specific_info);

  void OnDroppedFrame(size_t stream_idx);

  void OverrideFromFieldTrial(VideoEncoder::EncoderInfo* info) const;

  // Whether the stream's encoder comes from `top_stream_encoder_factory_`.
  bool UsesTopStreamEncoder(bool is_highest_quality_stream) const;

  std::atomic<int> inited_;
  const Environment env_;
  VideoEncoderFactory* const primary_encoder_factory_;
  VideoEncoderFactory* const hardware_encoder_factory_;
  const SdpVideoFormat video_format_;
  const std::function<std::unique_ptr<VideoEncoder>()> top_stream_encoder_factory_;
  const bool top_stream_encoder_for_every_stream_;
  VideoCodec codec_;
  int total_streams_count_;
  bool bypass_mode_;
  std::vector<StreamContext> stream_contexts_;
  EncodedImageCallback* encoded_complete_callback_;

  // Used for checking the single-threaded access of the encoder interface.
  RTC_NO_UNIQUE_ADDRESS SequenceChecker encoder_queue_;

  // Store previously created and released encoders , so they don't have to be
  // recreated. Remaining encoders are destroyed by the destructor.
  // Marked as `mutable` becuase we may need to temporarily create encoder in
  // GetEncoderInfo(), which is const.
  mutable std::list<std::unique_ptr<EncoderContext>> cached_encoder_contexts_;

  const std::optional<unsigned int> experimental_boosted_screenshare_qp_;
  const bool boost_base_layer_quality_;
  const bool prefer_temporal_support_on_base_layer_;

  const SimulcastEncoderAdapterEncoderInfoSettings encoder_info_override_;
};

}  // namespace webrtc

#endif  // TGCALLS_CUSTOM_SIMULCAST_ENCODER_ADAPTER_H_
