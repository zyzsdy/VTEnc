#include "encoder_controls.hpp"
#include "encoder_session.hpp"
#include "vtenc/status.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>

namespace vtenc {
namespace {
std::string string_value(CFStringRef value) {
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!CFStringGetCString(value, result.data(), size, kCFStringEncodingUTF8)) throw std::runtime_error("Cannot read encoder property string");
    result.resize(std::strlen(result.c_str()));
    return result;
}
bool requested(const EncodeConfig& config, std::int64_t index) {
    return std::ranges::binary_search(config.force_keyframes, index);
}
}
EncoderRateVerifier::EncoderRateVerifier(const EncodeConfig& c) {
    if (c.max_bitrate) {
        window_ = CMTimeMakeWithSeconds(c.bitrate_window, 1'000'000'000);
        if (!CMTIME_IS_NUMERIC(window_) || window_.value <= 0) throw std::invalid_argument("Invalid bitrate window precision");
        limit_bytes_ = static_cast<std::uint64_t>(std::floor(static_cast<double>(*c.max_bitrate) * c.bitrate_window / 8.0));
    }
    if (c.vbv_maxrate) {
        vbv_rate_ = *c.vbv_maxrate;
        // Public API default buffer duration is 2.5 seconds.
        vbv_bits_ = c.vbv_buffer_bits.value_or(static_cast<std::int64_t>(static_cast<double>(vbv_rate_) * c.vbv_duration.value_or(2.5)));
    }
}
void EncoderRateVerifier::append(CMSampleBufferRef sample) {
    if (!limit_bytes_ && !vbv_rate_) return;
    const auto pts = CMSampleBufferGetPresentationTimeStamp(sample);
    if (!CMTIME_IS_NUMERIC(pts) || (CMTIME_IS_VALID(previous_) && CMTimeCompare(pts, previous_) <= 0))
        throw std::runtime_error("Rate control verification requires increasing finite output PTS");
    const auto size = CMSampleBufferGetTotalSampleSize(sample);
    if (limit_bytes_) {
        while (!packets_.empty() && CMTimeCompare(CMTimeSubtract(pts, packets_.front().pts), window_) >= 0) {
            bytes_ -= packets_.front().bytes; packets_.pop_front();
        }
        bytes_ += size;
        packets_.push_back({pts, size});
        if (bytes_ > limit_bytes_)
            throw std::runtime_error("Hardware DataRateLimits violated at PTS=" + std::to_string(pts.value) + "/" + std::to_string(pts.timescale) +
                ": sliding-window bytes=" + std::to_string(bytes_) + ", limit=" + std::to_string(limit_bytes_) + "; output rejected");
    }
    if (vbv_rate_) {
        const auto elapsed = CMTIME_IS_VALID(previous_) ? CMTimeSubtract(pts, previous_) : kCMTimeZero;
        if (!CMTIME_IS_NUMERIC(elapsed)) throw std::runtime_error("Invalid VBV verification interval");
        // Rational leaky bucket, with no floating point timestamp accumulation.
        const auto factor = elapsed.timescale / std::gcd(denominator_, static_cast<std::int64_t>(elapsed.timescale));
        if (denominator_ > std::numeric_limits<std::int64_t>::max() / factor)
            throw std::runtime_error("VBV verification timebase overflow");
        debt_ *= factor; denominator_ *= factor;
        debt_ -= static_cast<__int128>(vbv_rate_) * elapsed.value * (denominator_ / elapsed.timescale);
        debt_ = std::max<__int128>(0, debt_);
        debt_ += static_cast<__int128>(size) * 8 * denominator_;
        if (debt_ > static_cast<__int128>(vbv_bits_) * denominator_)
            throw std::runtime_error("Hardware VBR/VBV envelope violated at PTS=" + std::to_string(pts.value) + "/" + std::to_string(pts.timescale) +
                ": buffer capacity bits=" + std::to_string(vbv_bits_) + "; output rejected");
    }
    previous_ = pts;
}
std::string property_json(CFTypeRef value) {
    if (!value) return "null";
    const auto type = CFGetTypeID(value);
    if (type == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(value)) ? "true" : "false";
    if (type == CFStringGetTypeID()) return json_string(string_value(static_cast<CFStringRef>(value)));
    if (type == CFNumberGetTypeID()) {
        double number = 0;
        if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &number) || !std::isfinite(number))
            throw std::runtime_error("Invalid encoder numeric property");
        std::ostringstream text; text.imbue(std::locale::classic()); text << std::setprecision(17) << number;
        return text.str();
    }
    if (type == CFArrayGetTypeID()) {
        const auto array = static_cast<CFArrayRef>(value);
        std::string result = "[";
        for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
            if (i) result += ',';
            result += property_json(CFArrayGetValueAtIndex(array, i));
        }
        return result + ']';
    }
    if (type == CFDictionaryGetTypeID()) {
        const auto dictionary = static_cast<CFDictionaryRef>(value);
        std::vector<const void*> keys(static_cast<std::size_t>(CFDictionaryGetCount(dictionary))), values(keys.size());
        CFDictionaryGetKeysAndValues(dictionary, keys.data(), values.data());
        std::map<std::string, std::string> sorted;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (CFGetTypeID(keys[i]) != CFStringGetTypeID()) throw std::runtime_error("Non-string encoder dictionary key");
            sorted.emplace(string_value(static_cast<CFStringRef>(keys[i])), property_json(values[i]));
        }
        std::string result = "{";
        for (const auto& [key, item] : sorted) { if (result.size() > 1) result += ','; result += json_string(key) + ':' + item; }
        return result + '}';
    }
    throw std::runtime_error("Unsupported encoder property value type");
}
CFRef<CFMutableDictionaryRef> encoder_control_properties(const EncodeConfig& c) {
    if (c.bitrate <= 0 || c.gop <= 0 || c.fps_num <= 0 || c.fps_den <= 0 ||
        (c.rate_control != RateControl::average && c.rate_control != RateControl::constant && c.rate_control != RateControl::variable) ||
        (c.quality && (!std::isfinite(*c.quality) || *c.quality < 0 || *c.quality > 1 || c.rate_control != RateControl::average)) ||
        (c.max_bitrate && (c.quality || c.rate_control != RateControl::average || *c.max_bitrate < c.bitrate)) ||
        !std::isfinite(c.bitrate_window) || c.bitrate_window <= 0 || c.bitrate_window > 60 ||
        ((c.vbv_maxrate || c.vbv_duration || c.vbv_buffer_bits) && c.rate_control != RateControl::variable) ||
        (c.vbv_buffer_bits && (*c.vbv_buffer_bits <= 0 || !c.vbv_maxrate || !c.vbv_duration)) ||
        (c.vbv_maxrate && *c.vbv_maxrate < c.bitrate) ||
        (c.vbv_duration && (!std::isfinite(*c.vbv_duration) || *c.vbv_duration <= 0 || *c.vbv_duration > 60)))
        throw std::invalid_argument("Invalid encoder rate control combination");
    CFRef result(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto number = [&](CFStringRef key, double value) {
        CFRef n(CFNumberCreate(nullptr, kCFNumberDoubleType, &value));
        CFDictionarySetValue(result.get(), key, n.get());
    };
    CFDictionarySetValue(result.get(), kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    CFDictionarySetValue(result.get(), kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    number(kVTCompressionPropertyKey_MaxKeyFrameInterval, c.gop);
    if (c.quality) number(kVTCompressionPropertyKey_Quality, *c.quality);
    else if (c.rate_control == RateControl::average) number(kVTCompressionPropertyKey_AverageBitRate, c.bitrate);
    else if (c.rate_control == RateControl::constant) {
        number(kVTCompressionPropertyKey_ExpectedFrameRate, static_cast<double>(c.fps_num) / c.fps_den);
        number(kVTCompressionPropertyKey_ConstantBitRate, c.bitrate);
    } else {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (__builtin_available(macOS 26.0, *)) {
            number(kVTCompressionPropertyKey_VariableBitRate, c.bitrate);
            if (c.vbv_maxrate) number(kVTCompressionPropertyKey_VBVMaxBitRate, *c.vbv_maxrate);
            if (c.vbv_maxrate || c.vbv_duration) number(kVTCompressionPropertyKey_VBVBufferDuration, c.vbv_duration.value_or(2.5));
        } else
#endif
        { throw std::runtime_error("Native VBR/VBV requires macOS 26 and an SDK with its public APIs"); }
    }
    if (c.max_bitrate) {
        // Bytes in the whole sliding window, not bytes per second.
        const double bytes = std::floor(static_cast<double>(*c.max_bitrate) * c.bitrate_window / 8.0);
        if (bytes < 1) throw std::invalid_argument("DataRateLimits window must contain at least one byte");
        CFRef byte_count(CFNumberCreate(nullptr, kCFNumberDoubleType, &bytes));
        CFRef seconds(CFNumberCreate(nullptr, kCFNumberDoubleType, &c.bitrate_window));
        const void* entries[] = {byte_count.get(), seconds.get()};
        CFRef limit(CFArrayCreate(nullptr, entries, 2, &kCFTypeArrayCallBacks));
        CFDictionarySetValue(result.get(), kVTCompressionPropertyKey_DataRateLimits, limit.get());
    }
    if (c.prioritize_speed) CFDictionarySetValue(result.get(), kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
        *c.prioritize_speed ? kCFBooleanTrue : kCFBooleanFalse);
    return result;
}
void apply_encoder_controls(VTCompressionSessionRef session, const EncodeConfig& config) {
    const auto properties = encoder_control_properties(config);
    std::vector<const void*> keys(static_cast<std::size_t>(CFDictionaryGetCount(properties.get()))), values(keys.size());
    CFDictionaryGetKeysAndValues(properties.get(), keys.data(), values.data());
    // Set dependencies before constraints; dictionary order is deliberately not relied upon.
    const auto apply = [&](CFStringRef key) {
        if (const auto value = CFDictionaryGetValue(properties.get(), key))
            check_status(VTSessionSetProperty(session, key, value), "Set encoder control " + string_value(key) + "=" + property_json(value));
    };
    apply(kVTCompressionPropertyKey_ExpectedFrameRate);
    apply(kVTCompressionPropertyKey_AverageBitRate);
    apply(kVTCompressionPropertyKey_ConstantBitRate);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (__builtin_available(macOS 26.0, *)) apply(kVTCompressionPropertyKey_VariableBitRate);
#endif
    for (const auto key : keys) {
        const auto name = static_cast<CFStringRef>(key);
        if (CFEqual(name, kVTCompressionPropertyKey_ExpectedFrameRate) || CFEqual(name, kVTCompressionPropertyKey_AverageBitRate) ||
            CFEqual(name, kVTCompressionPropertyKey_ConstantBitRate)) continue;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (__builtin_available(macOS 26.0, *)) { if (CFEqual(name, kVTCompressionPropertyKey_VariableBitRate)) continue; }
#endif
        apply(name);
    }
}
std::string verify_encoder_controls(VTCompressionSessionRef session, const EncodeConfig& config) {
    const auto properties = encoder_control_properties(config);
    std::vector<const void*> keys(static_cast<std::size_t>(CFDictionaryGetCount(properties.get()))), values(keys.size());
    CFDictionaryGetKeysAndValues(properties.get(), keys.data(), values.data());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto key = static_cast<CFStringRef>(keys[i]);
        CFTypeRef raw = nullptr;
        check_status(VTSessionCopyProperty(session, key, nullptr, &raw), "Read encoder control " + string_value(key));
        CFRef value(raw);
        // ExpectedFrameRate is a hint. Apple hardware may return an integer even for
        // a fractional request. Preserve the real CMTime timeline and report the readback.
        if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) {
            double fps = 0;
            if (CFGetTypeID(raw) != CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(raw), kCFNumberDoubleType, &fps) ||
                !std::isfinite(fps) || fps <= 0) throw std::runtime_error("Invalid ExpectedFrameRate readback for native CBR");
            CFDictionarySetValue(properties.get(), key, raw);
            continue;
        }
        if (!CFEqual(raw, values[i])) throw std::runtime_error("Encoder changed " + string_value(key) + ": requested " +
            property_json(values[i]) + ", effective " + property_json(raw));
    }
    return property_json(properties.get());
}
void report_encoder_controls(VTCompressionSessionRef session, const EncodeConfig& config) {
    const auto settings = verify_encoder_controls(session, config);
    const auto requested_properties = encoder_control_properties(config);
    const auto requested_settings = property_json(requested_properties.get());
    if (config.status) {
        if (settings != requested_settings) config.status->diagnostic("VideoToolbox normalized ExpectedFrameRate hint; requested " +
            requested_settings + "; effective " + settings + "; submitted rational timestamps remain unchanged");
        config.status->event("encoder_settings", "\"probe_level\":\"session_prepared\",\"requested_settings\":" +
            requested_settings + ",\"settings\":" + settings);
    }
}
CFRef<CFDictionaryRef> keyframe_options(const EncodeConfig& config, std::int64_t index) {
    const void* key[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
    const void* value[] = {kCFBooleanTrue};
    return CFRef(CFDictionaryCreate(nullptr, key, value, requested(config, index) ? 1 : 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}
void verify_keyframe(const EncodeConfig& config, std::int64_t index, CMSampleBufferRef sample) {
    if (!requested(config, index)) return;
    auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments)) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        auto not_sync = CFDictionaryGetValue(entry, kCMSampleAttachmentKey_NotSync);
        if (not_sync && CFEqual(not_sync, kCFBooleanTrue))
            throw std::runtime_error("Encoder ignored keyframe request at output frame=" + std::to_string(index));
    }
}
void verify_keyframe_count(const EncodeConfig& config, std::int64_t count) {
    if (!config.force_keyframes.empty() && config.force_keyframes.back() >= count)
        throw std::runtime_error("Requested keyframe index=" + std::to_string(config.force_keyframes.back()) +
            " does not exist: output frame count=" + std::to_string(count));
}
}
