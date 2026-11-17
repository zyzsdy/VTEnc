#include "bob.hpp"
#include "cf_ref.hpp"
#include <limits>
#include <stdexcept>

namespace vtenc {
namespace {
void exact(CMTime value) {
    if (!CMTIME_IS_NUMERIC(value) || value.epoch != 0 || CMTIME_HAS_BEEN_ROUNDED(value)) {
        throw std::runtime_error("Bob requires finite exact times with epoch zero");
    }
}
}
std::int32_t bob_timescale(std::int32_t source) {
    if (source <= 0 || source > std::numeric_limits<std::int32_t>::max() / 2) {
        throw std::runtime_error("Bob doubled video timescale exceeds CoreMedia's positive 32-bit range");
    }
    return source * 2;
}
BobTime bob_time(CMTime pts, CMTime duration, unsigned field_index) {
    exact(pts); exact(duration);
    if (duration.value <= 0 || field_index > 1) throw std::runtime_error("Bob needs a positive frame duration and field index 0 or 1");
    const auto half = duration.value % 2 == 0 ? CMTimeMake(duration.value / 2, duration.timescale)
                                             : CMTimeMake(duration.value, bob_timescale(duration.timescale));
    const auto time = field_index ? CMTimeAdd(pts, half) : pts;
    const auto end = CMTimeAdd(pts, duration);
    const auto split_end = CMTimeAdd(CMTimeAdd(pts, half), half);
    exact(time); exact(end); exact(split_end);
    if (CMTimeCompare(end, split_end) != 0) throw std::runtime_error("Bob field timeline cannot be represented exactly");
    return {time, half};
}
unsigned bob_first_field(FieldOrder requested, CMFormatDescriptionRef format, CVPixelBufferRef pixel) {
    if (requested == FieldOrder::tff) return 0;
    if (requested == FieldOrder::bff) return 1;
    const auto parse = [](CFTypeRef value) -> int {
        if (!value) return -1;
        if (CFEqual(value, kCVImageBufferFieldDetailTemporalTopFirst) || CFEqual(value, kCVImageBufferFieldDetailSpatialFirstLineEarly)) return 0;
        if (CFEqual(value, kCVImageBufferFieldDetailTemporalBottomFirst) || CFEqual(value, kCVImageBufferFieldDetailSpatialFirstLineLate)) return 1;
        throw std::runtime_error("Unsupported Bob field detail; specify --field-order tff|bff");
    };
    int from_pixel = -1;
    if (auto raw = CVBufferCopyAttachment(pixel, kCVImageBufferFieldDetailKey, nullptr)) {
        CFRef detail(raw); from_pixel = parse(detail.get());
    }
    const auto from_format = parse(CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FieldDetail));
    if (from_pixel >= 0 && from_format >= 0 && from_pixel != from_format) throw std::runtime_error("Conflicting Bob field order metadata; specify --field-order tff|bff");
    const auto order = from_pixel >= 0 ? from_pixel : from_format;
    if (order < 0) throw std::runtime_error("Missing Bob field order metadata; specify --field-order tff|bff");
    return static_cast<unsigned>(order);
}
EncodeConfig raw_bob_output_config(const EncodeConfig& source) {
    if (!source.filter.bob || source.filter.field_order == FieldOrder::automatic || source.video.height % 4 || source.input.empty()) {
        throw std::invalid_argument("Raw Bob requires file input, explicit field order and height divisible by four");
    }
    auto result = source;
    if (source.frames > std::numeric_limits<std::int32_t>::max() / 2) throw std::invalid_argument("Bob output frame count exceeds 32-bit configuration range");
    result.frames *= 2;
    if (result.fps_den % 2 == 0) result.fps_den /= 2;
    else result.fps_num = bob_timescale(result.fps_num);
    return result;
}
}
