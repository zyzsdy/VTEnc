#pragma once
#include "decoded_frames.hpp"
#include <array>
#include <VideoToolbox/VideoToolbox.h>

namespace vtenc {
// Checked rational grid; never accumulate floating point or rounded CMTime.
CMTime cfr_timestamp(CMTime origin, std::int64_t index, int numerator, int denominator);
int timeline_timescale(int source, int rate);
std::array<int, 2> display_sar(std::array<int, 2> dar, int width, int height,
                             const std::array<double, 6>& transform);
void set_display_sar(VTCompressionSessionRef session, std::array<int, 2> sar);
void verify_display_sar(CMFormatDescriptionRef format, std::array<int, 2> sar);

class CfrFrames final : public DecodedFrames {
public:
    CfrFrames(std::unique_ptr<DecodedFrames> source, SyncMode mode, int numerator, int denominator,
              const volatile std::sig_atomic_t& cancelled);
    std::unique_ptr<DecodedFrame> next() override;
private:
    void advance();
    std::unique_ptr<DecodedFrames> source_;
    std::unique_ptr<DecodedFrame> current_, following_;
    SyncMode mode_;
    int num_, den_;
    std::int64_t index_ = 0;
    CMTime origin_ = kCMTimeInvalid;
    const volatile std::sig_atomic_t& cancelled_;
};
} // namespace vtenc
