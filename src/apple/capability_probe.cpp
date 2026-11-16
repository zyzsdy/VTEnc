#include "vtenc/capabilities.hpp"
#include "cf_ref.hpp"

#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vtenc {
namespace {

void check_status(OSStatus status, const std::string& operation) {
    if (status != noErr) {
        throw std::runtime_error(operation + " failed (OSStatus=" + std::to_string(status) + ")");
    }
}

class CompressionSession {
public:
    VTCompressionSessionRef value = nullptr;
    CompressionSession() = default;
    CompressionSession(const CompressionSession&) = delete;
    CompressionSession& operator=(const CompressionSession&) = delete;
    ~CompressionSession() {
        if (value) {
            VTCompressionSessionInvalidate(value);
            CFRelease(value);
        }
    }
};

std::string utf8(CFStringRef value) {
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    if (!CFStringGetCString(value, buffer.data(), size, kCFStringEncodingUTF8)) {
        throw std::runtime_error("Cannot convert VideoToolbox property name to UTF-8");
    }
    return buffer.data();
}

} // namespace

EncoderCapabilities probe_encoder(const ProbeConfig& config) {
    if ((config.codec != Codec::h264 && config.codec != Codec::hevc) ||
        (config.bit_depth != 8 && config.bit_depth != 10) ||
        (config.codec == Codec::h264 && config.bit_depth != 8) ||
        config.width <= 0 || config.height <= 0 || config.width % 2 || config.height % 2) {
        throw std::invalid_argument("Invalid hardware probe configuration");
    }

    const void* spec_keys[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
    const void* spec_values[] = {kCFBooleanTrue};
    CFRef specification(CFDictionaryCreate(kCFAllocatorDefault, spec_keys, spec_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    const auto pixel_format = static_cast<std::int32_t>(config.bit_depth == 10
        ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFRef pixel_number(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format));
    const void* pixel_keys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void* pixel_values[] = {pixel_number.get()};
    CFRef attributes(CFDictionaryCreate(kCFAllocatorDefault, pixel_keys, pixel_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CompressionSession session;
    const auto context = std::string(codec_name(config.codec)) + " " + std::to_string(config.bit_depth)
        + "-bit " + std::to_string(config.width) + "x" + std::to_string(config.height);
    check_status(VTCompressionSessionCreate(kCFAllocatorDefault, config.width, config.height,
        config.codec == Codec::h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC,
        specification.get(), attributes.get(), nullptr, nullptr, nullptr, &session.value),
        "VTCompressionSessionCreate [hardware required; " + context + "]");
    if (!session.value) throw std::runtime_error("VTCompressionSessionCreate returned no session");

    const auto profile = config.codec == Codec::h264 ? kVTProfileLevel_H264_High_AutoLevel
        : config.bit_depth == 10 ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
    check_status(VTSessionSetProperty(session.value, kVTCompressionPropertyKey_ProfileLevel, profile),
        "VTSessionSetProperty(ProfileLevel) [" + context + "]");
    check_status(VTCompressionSessionPrepareToEncodeFrames(session.value),
        "VTCompressionSessionPrepareToEncodeFrames [" + context + "]");

    CFTypeRef hardware_raw = nullptr;
    check_status(VTSessionCopyProperty(session.value,
        kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
        kCFAllocatorDefault, &hardware_raw), "VTSessionCopyProperty(UsingHardwareAcceleratedVideoEncoder)");
    CFRef hardware(hardware_raw);
    if (CFGetTypeID(hardware.get()) != CFBooleanGetTypeID() || !CFEqual(hardware.get(), kCFBooleanTrue)) {
        throw std::runtime_error("VideoToolbox did not confirm hardware encoding; software fallback is forbidden");
    }

    CFDictionaryRef properties_raw = nullptr;
    check_status(VTSessionCopySupportedPropertyDictionary(session.value, &properties_raw),
        "VTSessionCopySupportedPropertyDictionary");
    CFRef properties(properties_raw);
    const auto count = CFDictionaryGetCount(properties.get());
    std::vector<const void*> keys(static_cast<std::size_t>(count));
    CFDictionaryGetKeysAndValues(properties.get(), keys.data(), nullptr);
    EncoderCapabilities result{config, true, {}};
    for (const auto key : keys) {
        if (CFGetTypeID(key) != CFStringGetTypeID()) {
            throw std::runtime_error("VideoToolbox returned a non-string property key");
        }
        result.property_names.push_back(utf8(static_cast<CFStringRef>(key)));
    }
    std::ranges::sort(result.property_names);
    return result;
}

} // namespace vtenc
