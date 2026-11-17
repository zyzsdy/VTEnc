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
[[nodiscard]] CMTime ivtc_quarter(CMTime duration);
[[nodiscard]] CMTime ivtc_divide(CMTime duration, int divisor);
[[nodiscard]] std::int32_t ivtc_output_timescale(std::int32_t source, const IvtcConfig& config);

// Packed single-field HEVC pictures -> full-height display rasters, in display order.
class FieldFrames final : public DecodedFrames {
public:
    FieldFrames(std::unique_ptr<DecodedFrames> source, const FilterConfig& config,
                const volatile std::sig_atomic_t& cancelled);
    ~FieldFrames() override;
    [[nodiscard]] std::unique_ptr<DecodedFrame> next() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class GpuFieldPair {
public:
    explicit GpuFieldPair(CVPixelBufferRef source);
    ~GpuFieldPair();
    // Null second field reconstructs missing lines spatially; no temporal fabrication.
    [[nodiscard]] CFRef<CVPixelBufferRef> process(CVPixelBufferRef first, CVPixelBufferRef second, unsigned parity);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Streaming equivalent of the upstream pending-top/pending-bottom RFF schedule.
// Materializes field pairs in Metal; never duplicates a whole interval's time.
class RffFrames final : public DecodedFrames {
public:
    RffFrames(std::unique_ptr<DecodedFrames> source, const FilterConfig& config,
              const volatile std::sig_atomic_t& cancelled);
    ~RffFrames() override;
    [[nodiscard]] std::unique_ptr<DecodedFrame> next() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

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
