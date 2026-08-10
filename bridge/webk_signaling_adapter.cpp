#include "webk_signaling_adapter.h"

#include <utility>

namespace {

tgcalls::signaling::DtlsFingerprint MapFingerprint(
    const tgcalls::signaling_4_0_0::DtlsFingerprint &fingerprint) {
  tgcalls::signaling::DtlsFingerprint mapped;
  mapped.hash = fingerprint.hash;
  mapped.setup = fingerprint.setup;
  mapped.fingerprint = fingerprint.fingerprint;
  return mapped;
}

tgcalls::signaling::SsrcGroup MapSsrcGroup(
    const tgcalls::signaling_4_0_0::SsrcGroup &ssrcGroup) {
  tgcalls::signaling::SsrcGroup mapped;
  mapped.ssrcs = ssrcGroup.ssrcs;
  mapped.semantics = ssrcGroup.semantics;
  return mapped;
}

tgcalls::signaling::FeedbackType MapFeedbackType(
    const tgcalls::signaling_4_0_0::FeedbackType &feedbackType) {
  tgcalls::signaling::FeedbackType mapped;
  mapped.type = feedbackType.type;
  mapped.subtype = feedbackType.subtype;
  return mapped;
}

tgcalls::signaling::PayloadType MapPayloadType(
    const tgcalls::signaling_4_0_0::PayloadType &payloadType) {
  tgcalls::signaling::PayloadType mapped;
  mapped.id = payloadType.id;
  mapped.name = payloadType.name;
  mapped.clockrate = payloadType.clockrate;
  mapped.channels = payloadType.channels;
  mapped.parameters = payloadType.parameters;
  mapped.feedbackTypes.reserve(payloadType.feedbackTypes.size());
  for (const tgcalls::signaling_4_0_0::FeedbackType &feedbackType : payloadType.feedbackTypes) {
    mapped.feedbackTypes.push_back(MapFeedbackType(feedbackType));
  }
  return mapped;
}

tgcalls::signaling::MediaContent MapMediaContent(
    const tgcalls::signaling_4_0_0::MediaContent &content,
    tgcalls::signaling::MediaContent::Type type) {
  tgcalls::signaling::MediaContent mapped;
  mapped.type = type;
  mapped.ssrc = content.ssrc;
  mapped.rtpExtensions = content.rtpExtensions;
  mapped.ssrcGroups.reserve(content.ssrcGroups.size());
  for (const tgcalls::signaling_4_0_0::SsrcGroup &ssrcGroup : content.ssrcGroups) {
    mapped.ssrcGroups.push_back(MapSsrcGroup(ssrcGroup));
  }
  mapped.payloadTypes.reserve(content.payloadTypes.size());
  for (const tgcalls::signaling_4_0_0::PayloadType &payloadType : content.payloadTypes) {
    mapped.payloadTypes.push_back(MapPayloadType(payloadType));
  }
  return mapped;
}

tgcalls::signaling_4_0_0::DtlsFingerprint MapFingerprint(
    const tgcalls::signaling::DtlsFingerprint &fingerprint) {
  tgcalls::signaling_4_0_0::DtlsFingerprint mapped;
  mapped.hash = fingerprint.hash;
  mapped.setup = fingerprint.setup;
  mapped.fingerprint = fingerprint.fingerprint;
  return mapped;
}

tgcalls::signaling_4_0_0::SsrcGroup MapSsrcGroup(
    const tgcalls::signaling::SsrcGroup &ssrcGroup) {
  tgcalls::signaling_4_0_0::SsrcGroup mapped;
  mapped.ssrcs = ssrcGroup.ssrcs;
  mapped.semantics = ssrcGroup.semantics;
  return mapped;
}

tgcalls::signaling_4_0_0::FeedbackType MapFeedbackType(
    const tgcalls::signaling::FeedbackType &feedbackType) {
  tgcalls::signaling_4_0_0::FeedbackType mapped;
  mapped.type = feedbackType.type;
  mapped.subtype = feedbackType.subtype;
  return mapped;
}

tgcalls::signaling_4_0_0::PayloadType MapPayloadType(
    const tgcalls::signaling::PayloadType &payloadType) {
  tgcalls::signaling_4_0_0::PayloadType mapped;
  mapped.id = payloadType.id;
  mapped.name = payloadType.name;
  mapped.clockrate = payloadType.clockrate;
  mapped.channels = payloadType.channels;
  mapped.parameters = payloadType.parameters;
  mapped.feedbackTypes.reserve(payloadType.feedbackTypes.size());
  for (const tgcalls::signaling::FeedbackType &feedbackType : payloadType.feedbackTypes) {
    mapped.feedbackTypes.push_back(MapFeedbackType(feedbackType));
  }
  return mapped;
}

tgcalls::signaling_4_0_0::MediaContent MapMediaContent(
    const tgcalls::signaling::MediaContent &content) {
  tgcalls::signaling_4_0_0::MediaContent mapped;
  mapped.ssrc = content.ssrc;
  mapped.rtpExtensions = content.rtpExtensions;
  mapped.ssrcGroups.reserve(content.ssrcGroups.size());
  for (const tgcalls::signaling::SsrcGroup &ssrcGroup : content.ssrcGroups) {
    mapped.ssrcGroups.push_back(MapSsrcGroup(ssrcGroup));
  }
  mapped.payloadTypes.reserve(content.payloadTypes.size());
  for (const tgcalls::signaling::PayloadType &payloadType : content.payloadTypes) {
    mapped.payloadTypes.push_back(MapPayloadType(payloadType));
  }
  return mapped;
}

tgcalls::signaling_4_0_0::MediaStateMessage::VideoState MapVideoState(
    tgcalls::signaling::MediaStateMessage::VideoState videoState) {
  switch (videoState) {
    case tgcalls::signaling::MediaStateMessage::VideoState::Active:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Active;
    case tgcalls::signaling::MediaStateMessage::VideoState::Suspended:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Suspended;
    case tgcalls::signaling::MediaStateMessage::VideoState::Inactive:
    default:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoState::Inactive;
  }
}

tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation MapVideoRotation(
    tgcalls::signaling::MediaStateMessage::VideoRotation videoRotation) {
  switch (videoRotation) {
    case tgcalls::signaling::MediaStateMessage::VideoRotation::Rotation90:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation::Rotation90;
    case tgcalls::signaling::MediaStateMessage::VideoRotation::Rotation180:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation::Rotation180;
    case tgcalls::signaling::MediaStateMessage::VideoRotation::Rotation270:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation::Rotation270;
    case tgcalls::signaling::MediaStateMessage::VideoRotation::Rotation0:
    default:
      return tgcalls::signaling_4_0_0::MediaStateMessage::VideoRotation::Rotation0;
  }
}

}  // namespace

namespace tgcall {

WebKToV2InitialSetupMapping MapWebKInitialSetupToV2(
    const tgcalls::signaling_4_0_0::InitialSetupMessage &initialSetup,
    uint32_t exchangeId) {
  WebKToV2InitialSetupMapping mapped;
  mapped.initialSetup.ufrag = initialSetup.ufrag;
  mapped.initialSetup.pwd = initialSetup.pwd;
  mapped.initialSetup.supportsRenomination = false;
  mapped.initialSetup.fingerprints.reserve(initialSetup.fingerprints.size());
  for (const tgcalls::signaling_4_0_0::DtlsFingerprint &fingerprint : initialSetup.fingerprints) {
    mapped.initialSetup.fingerprints.push_back(MapFingerprint(fingerprint));
  }

  mapped.negotiation.exchangeId = exchangeId;
  if (initialSetup.audio.has_value()) {
    mapped.hasAudio = true;
    mapped.negotiation.contents.push_back(
        MapMediaContent(initialSetup.audio.value(), tgcalls::signaling::MediaContent::Type::Audio));
  }
  if (initialSetup.video.has_value()) {
    mapped.hasVideo = true;
    mapped.negotiation.contents.push_back(
        MapMediaContent(initialSetup.video.value(), tgcalls::signaling::MediaContent::Type::Video));
  }
  if (initialSetup.screencast.has_value()) {
    mapped.hasScreencast = true;
    mapped.negotiation.contents.push_back(
        MapMediaContent(initialSetup.screencast.value(), tgcalls::signaling::MediaContent::Type::Video));
  }

  return mapped;
}

tgcalls::signaling::CandidatesMessage MapWebKCandidatesToV2(
    const tgcalls::signaling_4_0_0::CandidatesMessage &candidates) {
  tgcalls::signaling::CandidatesMessage mapped;
  mapped.iceCandidates.reserve(candidates.iceCandidates.size());
  for (const tgcalls::signaling_4_0_0::IceCandidate &candidate : candidates.iceCandidates) {
    tgcalls::signaling::IceCandidate mappedCandidate;
    mappedCandidate.sdpString = candidate.sdpString;
    mapped.iceCandidates.push_back(std::move(mappedCandidate));
  }
  return mapped;
}

tgcalls::signaling_4_0_0::InitialSetupMessage MapV2InitialSetupToWebK(
    const tgcalls::signaling::InitialSetupMessage &initialSetup,
    const tgcalls::signaling::NegotiateChannelsMessage &negotiation) {
  tgcalls::signaling_4_0_0::InitialSetupMessage mapped;
  mapped.ufrag = initialSetup.ufrag;
  mapped.pwd = initialSetup.pwd;
  mapped.fingerprints.reserve(initialSetup.fingerprints.size());
  for (const tgcalls::signaling::DtlsFingerprint &fingerprint : initialSetup.fingerprints) {
    mapped.fingerprints.push_back(MapFingerprint(fingerprint));
  }

  bool hasVideo = false;
  for (const tgcalls::signaling::MediaContent &content : negotiation.contents) {
    if (content.type == tgcalls::signaling::MediaContent::Type::Audio) {
      if (!mapped.audio.has_value()) {
        mapped.audio = MapMediaContent(content);
      }
      continue;
    }
    if (!hasVideo) {
      mapped.video = MapMediaContent(content);
      hasVideo = true;
      continue;
    }
    if (!mapped.screencast.has_value()) {
      mapped.screencast = MapMediaContent(content);
    }
  }

  return mapped;
}

tgcalls::signaling_4_0_0::CandidatesMessage MapV2CandidatesToWebK(
    const tgcalls::signaling::CandidatesMessage &candidates) {
  tgcalls::signaling_4_0_0::CandidatesMessage mapped;
  mapped.iceCandidates.reserve(candidates.iceCandidates.size());
  for (const tgcalls::signaling::IceCandidate &candidate : candidates.iceCandidates) {
    tgcalls::signaling_4_0_0::IceCandidate mappedCandidate;
    mappedCandidate.sdpString = candidate.sdpString;
    mapped.iceCandidates.push_back(std::move(mappedCandidate));
  }
  return mapped;
}

tgcalls::signaling_4_0_0::MediaStateMessage MapV2MediaStateToWebK(
    const tgcalls::signaling::MediaStateMessage &mediaState) {
  tgcalls::signaling_4_0_0::MediaStateMessage mapped;
  mapped.isMuted = mediaState.isMuted;
  mapped.videoState = MapVideoState(mediaState.videoState);
  mapped.videoRotation = MapVideoRotation(mediaState.videoRotation);
  mapped.screencastState = MapVideoState(mediaState.screencastState);
  mapped.isBatteryLow = mediaState.isBatteryLow;
  return mapped;
}

}  // namespace tgcall
