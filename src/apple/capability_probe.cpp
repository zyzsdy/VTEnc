#include "vtenc/capabilities.hpp"
#include "encoder_session.hpp"

#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vtenc {
namespace {

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
    CompressionSession session;
    create_hardware_session(session, config,
        [](void*, void*, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef) noexcept {}, nullptr);
    check_status(VTCompressionSessionPrepareToEncodeFrames(session.value),
        "VTCompressionSessionPrepareToEncodeFrames");
    verify_hardware(session.value);

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
