#pragma once

#include "cf_ref.hpp"
#include "media_io.hpp"
#include <CoreVideo/CoreVideo.h>

namespace vtenc {

struct DecodedFrame {
    CFRef<CVPixelBufferRef> pixel;
    CMTime pts;
    CMTime duration;
};

class DecodedFrames {
public:
    virtual ~DecodedFrames() = default;
    [[nodiscard]] virtual std::unique_ptr<DecodedFrame> next() = 0;
};

// Software-only libavcodec, fed by libavformat; owns its demux/decode state.
class SoftwareFrames final : public DecodedFrames {
public:
    SoftwareFrames(const EncodeConfig& config, ContainerInput& input,
                   const volatile std::sig_atomic_t& cancelled);
    ~SoftwareFrames() override;
    [[nodiscard]] std::unique_ptr<DecodedFrame> next() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtenc
