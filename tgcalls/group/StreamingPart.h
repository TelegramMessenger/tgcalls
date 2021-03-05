#ifndef TGCALLS_STREAMING_PART_H
#define TGCALLS_STREAMING_PART_H

#include "absl/types/optional.h"
#include <vector>

namespace tgcalls {

class StreamingPart {
public:
    struct StreamingPartChannel {
        uint32_t ssrc = 0;
        std::vector<uint8_t> pcmData;
    };
    
    static absl::optional<StreamingPart> parse(std::vector<uint8_t> const &data);
    
public:
    std::vector<StreamingPartChannel> channels;
};

}

#endif
