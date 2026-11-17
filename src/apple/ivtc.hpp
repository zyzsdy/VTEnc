#pragma once
#include "decoded_frames.hpp"
#include "vtenc/command_line.hpp"
#include <array>
#include <cstdint>

namespace vtenc {
// Port of NVEnc's hard-telecine primitives; see THIRD_PARTY_NOTICES.md.
struct IvtcScore {
    std::uint64_t match = 0, comb = 0, maximum = 0, blocks = 0;
};
using IvtcScores = std::array<IvtcScore, 3>; // C, P, N
[[nodiscard]] unsigned ivtc_select(const IvtcScores& scores);
[[nodiscard]] std::int32_t ivtc_timescale(std::int32_t source);
[[nodiscard]] CMTime ivtc_quarter(CMTime duration);

class GpuIvtc {
public:
    GpuIvtc(CVPixelBufferRef source, IvtcConfig config);
    ~GpuIvtc();
    [[nodiscard]] IvtcScores score(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef next, unsigned first);
    [[nodiscard]] CFRef<CVPixelBufferRef> synthesize(CVPixelBufferRef prev, CVPixelBufferRef cur,
        CVPixelBufferRef next, unsigned first, unsigned match, bool repair);
    [[nodiscard]] std::uint64_t difference(CVPixelBufferRef a, CVPixelBufferRef b);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Bounded temporal stage: three source frames, five matched frames, one cycle tail.
// Does not wrap errors as HardwareDecodeError: only the underlying decoder may retry.
class IvtcFrames final : public DecodedFrames {
public:
    IvtcFrames(std::unique_ptr<DecodedFrames> source, CMFormatDescriptionRef format,
               const FilterConfig& config, const volatile std::sig_atomic_t& cancelled);
    ~IvtcFrames() override;
    [[nodiscard]] std::unique_ptr<DecodedFrame> next() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
