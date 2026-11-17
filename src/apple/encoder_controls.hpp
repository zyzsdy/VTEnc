#pragma once
#include "cf_ref.hpp"
#include "vtenc/command_line.hpp"
#include <VideoToolbox/VideoToolbox.h>
#include <deque>

namespace vtenc {
// Validate the actual compressed samples before writing/publishing constrained output.
class EncoderRateVerifier {
public:
    explicit EncoderRateVerifier(const EncodeConfig& config);
    void append(CMSampleBufferRef sample);
private:
    struct Packet { CMTime pts; std::uint64_t bytes; };
    std::deque<Packet> packets_;
    CMTime window_ = kCMTimeInvalid, previous_ = kCMTimeInvalid;
    std::uint64_t bytes_ = 0, limit_bytes_ = 0;
    std::int64_t vbv_rate_ = 0, vbv_bits_ = 0, denominator_ = 1;
    __int128 debt_ = 0;
};
// Public API properties only. Readback is evidence of configuration, not bitrate verification.
[[nodiscard]] CFRef<CFMutableDictionaryRef> encoder_control_properties(const EncodeConfig& config);
void apply_encoder_controls(VTCompressionSessionRef session, const EncodeConfig& config);
[[nodiscard]] std::string verify_encoder_controls(VTCompressionSessionRef session, const EncodeConfig& config);
void report_encoder_controls(VTCompressionSessionRef session, const EncodeConfig& config);
[[nodiscard]] std::string property_json(CFTypeRef value);
[[nodiscard]] CFRef<CFDictionaryRef> keyframe_options(const EncodeConfig& config, std::int64_t index);
void verify_keyframe(const EncodeConfig& config, std::int64_t index, CMSampleBufferRef sample);
void verify_keyframe_count(const EncodeConfig& config, std::int64_t count);
}
