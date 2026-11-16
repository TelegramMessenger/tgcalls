#ifndef TGCALLS_COMPAT_SIGNALING_FRAMING_H
#define TGCALLS_COMPAT_SIGNALING_FRAMING_H

#include <string>

namespace tgcalls {

// How InstanceV2CompatImpl frames signaling on the wire. Each end of a call
// picks its framing from its own version string, and nothing in-band tells it
// the peer's, so every version maps to the framing of the peer it is served
// against:
//   V1 (7.0.0): encryptRawPacket, no compression, no SCTP.
//   V2 (8.0.0, 9.0.0, and 14.0.0, this engine's own): EncryptedConnection's own
//       reliable messages (seq, acks, resends), no compression, no SCTP. 9.0.0
//       was V3 until 2023-04. The server serves 14.0.0 to one end of a call
//       whose other end it serves 8.0.0 (InstanceV2Impl), so 14.0.0 speaks
//       8.0.0's wire; two 14.0.0 ends speak it to each other.
//   V3 (12.0.0, 13.0.0, any other version): gzip, encryptRawPacket, over SCTP
//       unless the server sets network_signaling_nosctp.
// The legacy versions match InstanceV2Impl's map, except that an unknown
// version is V3 here and V2 there.
// network_kcp_experiment moves any version to V3, as in InstanceV2Impl (the
// peer gets the same custom parameters): it is how a 14.0.0 end talks to a
// 13.0.0 one.
// //submodules/TgVoipWebrtc:compat_signaling_framing_test pins this map.
enum class CompatSignalingFraming {
    V1,
    V2,
    V3
};

inline CompatSignalingFraming compatSignalingFraming(std::string const &version, bool kcpExperiment) {
    if (kcpExperiment) {
        return CompatSignalingFraming::V3;
    }
    if (version == "7.0.0") {
        return CompatSignalingFraming::V1;
    } else if (version == "8.0.0" || version == "9.0.0" || version == "14.0.0") {
        return CompatSignalingFraming::V2;
    } else {
        return CompatSignalingFraming::V3;
    }
}

} // namespace tgcalls

#endif
