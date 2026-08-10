#ifndef TGCALLS_FIELD_TRIALS_CONFIG_H
#define TGCALLS_FIELD_TRIALS_CONFIG_H

#include "api/field_trials_view.h"

namespace tgcalls {

// Empty FieldTrialsView: returns no configured values. Aligns with the
// FieldTrialsView interface used by the prebuilt WebRTC (main branch).
class EmptyFieldTrialsView : public webrtc::FieldTrialsView {
public:
    std::string Lookup(absl::string_view key) const override {
        return std::string();
    }
};

extern EmptyFieldTrialsView fieldTrialsBasedConfig;

} // namespace tgcalls

#endif
