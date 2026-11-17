#pragma once
#include "decoded_frames.hpp"

namespace vtenc {
// NVEnc YADIF adaptation; pinned source and MIT license in THIRD_PARTY_NOTICES.md.
class GpuYadif {
public:
    explicit GpuYadif(CVPixelBufferRef source);
    ~GpuYadif();
    // Retain spatial parity 'field'. second selects cur/next temporal prediction.
    // copy preserves progressive pixels while normalizing field metadata.
    [[nodiscard]] CFRef<CVPixelBufferRef> process(CVPixelBufferRef previous, CVPixelBufferRef current,
        CVPixelBufferRef following, unsigned field, bool second, bool copy = false, bool spatial_only = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::int32_t yadif_output_timescale(std::int32_t source, bool bob);

// Field pairing -> RFF expansion -> bounded three-frame YADIF. No decimation.
class YadifFrames final : public DecodedFrames {
public:
    YadifFrames(std::unique_ptr<DecodedFrames> source, CMFormatDescriptionRef format,
                const FilterConfig& config, const volatile std::sig_atomic_t& cancelled);
    ~YadifFrames() override;
    [[nodiscard]] std::unique_ptr<DecodedFrame> next() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
