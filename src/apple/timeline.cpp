#include "timeline.hpp"
#include "encoder_session.hpp"
#include <cmath>
#include <limits>
#include <numeric>

namespace vtenc {
namespace {
void exact(CMTime time) {
    if (!CMTIME_IS_NUMERIC(time) || time.epoch || CMTIME_HAS_BEEN_ROUNDED(time))
        throw std::runtime_error("Invalid, overflowing or inexact CFR timestamp");
}
void validate(const DecodedFrame& frame) {
    exact(frame.pts); exact(frame.duration);
    if (frame.duration.value <= 0) throw std::runtime_error("CFR requires positive source duration");
}
}
int timeline_timescale(int source, int rate) {
    if (source <= 0 || rate <= 0) throw std::runtime_error("Invalid CFR time base");
    const auto scale = std::lcm(static_cast<std::int64_t>(source), static_cast<std::int64_t>(rate));
    if (scale > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("Exact CFR time base exceeds 32-bit range");
    return static_cast<int>(scale);
}
CMTime cfr_timestamp(CMTime origin, std::int64_t index, int numerator, int denominator) {
    exact(origin);
    if (index < 0 || numerator <= 0 || denominator <= 0 || index > INT64_MAX / denominator)
        throw std::runtime_error("Invalid or overflowing CFR frame index/rate");
    (void)timeline_timescale(origin.timescale, numerator);
    const auto result = CMTimeAdd(origin, CMTimeMake(index * denominator, numerator));
    exact(result);
    return result;
}
std::array<int, 2> display_sar(std::array<int, 2> dar, int width, int height,
                             const std::array<double, 6>& t) {
    const bool straight = std::abs(t[0]) == 1 && std::abs(t[3]) == 1 && t[1] == 0 && t[2] == 0;
    const bool swapped = std::abs(t[1]) == 1 && std::abs(t[2]) == 1 && t[0] == 0 && t[3] == 0;
    if (!straight && !swapped) throw std::runtime_error("--dar requires orthogonal unit rotation/reflection; scaled/sheared display transform is unsupported");
    if (dar[0] <= 0 || dar[1] <= 0 || width <= 0 || height <= 0) throw std::runtime_error("Invalid DAR or output dimensions");
    std::int64_t n = static_cast<std::int64_t>(dar[swapped ? 1 : 0]) * height;
    std::int64_t d = static_cast<std::int64_t>(dar[swapped ? 0 : 1]) * width;
    const auto gcd = std::gcd(n, d); n /= gcd; d /= gcd;
    // H.264 and HEVC extended SAR fields are unsigned 16-bit.
    if (n > 65535 || d > 65535) throw std::runtime_error("Derived SAR exceeds H.264/HEVC 16-bit spacing; choose a representable --dar");
    return {static_cast<int>(n), static_cast<int>(d)};
}
void set_display_sar(VTCompressionSessionRef session, std::array<int, 2> sar) {
    CFRef n(CFNumberCreate(nullptr, kCFNumberIntType, &sar[0]));
    CFRef d(CFNumberCreate(nullptr, kCFNumberIntType, &sar[1]));
    const void* keys[]{kCVImageBufferPixelAspectRatioHorizontalSpacingKey, kCVImageBufferPixelAspectRatioVerticalSpacingKey};
    const void* values[]{n.get(), d.get()};
    CFRef ratio(CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    check_status(VTSessionSetProperty(session, kVTCompressionPropertyKey_PixelAspectRatio, ratio.get()), "Set DAR-derived PixelAspectRatio");
}
void verify_display_sar(CMFormatDescriptionRef format, std::array<int, 2> sar) {
    const auto ratio = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_PixelAspectRatio);
    std::array<int, 2> actual{1, 1};
    if (ratio) {
        if (CFGetTypeID(ratio) != CFDictionaryGetTypeID()) throw std::runtime_error("Invalid encoded SAR dictionary");
        const CFStringRef keys[]{kCVImageBufferPixelAspectRatioHorizontalSpacingKey, kCVImageBufferPixelAspectRatioVerticalSpacingKey};
        for (unsigned i = 0; i < 2; ++i) {
            const auto value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(ratio), keys[i]);
            if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
                !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &actual[i]) || actual[i] <= 0)
                throw std::runtime_error("Invalid encoded SAR value");
        }
    }
    if (static_cast<std::int64_t>(actual[0]) * sar[1] != static_cast<std::int64_t>(sar[0]) * actual[1])
        throw std::runtime_error("Hardware encoder changed requested DAR/SAR");
}
CfrFrames::CfrFrames(std::unique_ptr<DecodedFrames> source, SyncMode mode, int numerator, int denominator,
                     const volatile std::sig_atomic_t& cancelled)
    : source_(std::move(source)), mode_(mode), num_(numerator), den_(denominator), cancelled_(cancelled) {
    if (mode == SyncMode::vfr || num_ <= 0 || den_ <= 0) throw std::runtime_error("Invalid CFR configuration");
}
void CfrFrames::advance() {
    following_ = source_->next();
    if (following_) {
        validate(*following_);
        if (CMTimeCompare(following_->pts, current_->pts) <= 0) throw std::runtime_error("CFR source PTS must strictly increase");
    }
}
std::unique_ptr<DecodedFrame> CfrFrames::next() {
    if (cancelled_) return nullptr;
    if (!CMTIME_IS_VALID(origin_)) {
        current_ = source_->next();
        if (!current_) return nullptr;
        validate(*current_); origin_ = current_->pts;
        advance();
    }
    const auto pts = cfr_timestamp(origin_, index_, num_, den_);
    while (following_ && CMTimeCompare(following_->pts, pts) <= 0 && !cancelled_) {
        current_ = std::move(following_); advance();
    }
    if (cancelled_) return nullptr;
    const auto end = CMTimeAdd(current_->pts, current_->duration); exact(end);
    if (!following_ && CMTimeCompare(pts, end) >= 0) return nullptr;
    if (index_ == INT64_MAX) throw std::runtime_error("CFR frame index overflow");
    auto next = cfr_timestamp(origin_, ++index_, num_, den_);
    CFRetain(current_->pixel.get());
    auto result = std::make_unique<DecodedFrame>(DecodedFrame{CFRef(current_->pixel.get()), pts, kCMTimeInvalid, current_->cadence});
    // Look through frames dropped within this period, so even a very short final
    // source interval clips the current CFR packet instead of extending it.
    while (following_ && CMTimeCompare(following_->pts, next) < 0 && !cancelled_) {
        current_ = std::move(following_); advance();
    }
    if (cancelled_) return nullptr;
    const auto final_end = CMTimeAdd(current_->pts, current_->duration); exact(final_end);
    if (!following_ && mode_ == SyncMode::cfr && CMTimeCompare(next, final_end) > 0) next = final_end;
    result->duration = CMTimeSubtract(next, pts); exact(result->duration);
    return result;
}
} // namespace vtenc
