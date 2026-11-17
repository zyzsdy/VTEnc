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
    EncoderCapabilities result;
    result.config = config;
    result.hardware = true;
    result.encoder_id = hardware_encoder_id(session.value);
    for (const auto key : keys) {
        if (CFGetTypeID(key) != CFStringGetTypeID()) {
            throw std::runtime_error("VideoToolbox returned a non-string property key");
        }
        result.property_names.push_back(utf8(static_cast<CFStringRef>(key)));
    }
    std::ranges::sort(result.property_names);
    // Each candidate gets a fresh session. No rejected property can contaminate another probe.
    // These probes do not submit frames and are never cached as hardware support verdicts.
    for (const auto name : {"abr", "cbr", "abr_limit", "vbr", "vbr_vbv", "preset_performance", "preset_quality"}) {
        EncoderControlProbe probe;
        probe.name = name;
        EncodeConfig candidate;
        candidate.video = config;
        candidate.bitrate = 5'000'000;
        if (probe.name == "cbr") candidate.rate_control = RateControl::constant;
        if (probe.name == "abr_limit") candidate.max_bitrate = 8'000'000;
        if (probe.name.starts_with("vbr")) candidate.rate_control = RateControl::variable;
        if (probe.name == "vbr_vbv") { candidate.vbv_maxrate = 8'000'000; candidate.vbv_duration = 2.0; }
        if (probe.name.starts_with("preset_")) candidate.prioritize_speed = probe.name == "preset_performance";
        try {
            probe.stage = "api";
            const auto requested = encoder_control_properties(candidate);
            probe.settings_json = property_json(requested.get());
            CompressionSession control;
            probe.stage = "create";
            create_hardware_session(control, config, nullptr, nullptr);
            CFDictionaryRef raw = nullptr;
            check_status(VTSessionCopySupportedPropertyDictionary(control.value, &raw), "Enumerate control properties");
            CFRef dictionary(raw);
            std::vector<const void*> required(static_cast<std::size_t>(CFDictionaryGetCount(requested.get())));
            CFDictionaryGetKeysAndValues(requested.get(), required.data(), nullptr);
            probe.enumerated = std::ranges::all_of(required, [&](const void* key) { return CFDictionaryContainsKey(dictionary.get(), key); });
            probe.stage = "set";
            configure_hardware_encoder(control.value, candidate);
            probe.stage = "prepare";
            check_status(VTCompressionSessionPrepareToEncodeFrames(control.value), "Prepare control probe");
            probe.stage = "hardware";
            verify_hardware(control.value);
            probe.encoder_id = hardware_encoder_id(control.value);
            probe.stage = "readback";
            probe.settings_json = verify_encoder_controls(control.value, candidate);
            probe.stage = "session_prepared";
        } catch (const std::exception& error) {
            probe.error = error.what();
            probe.os_status = native_status(error);
        }
        result.controls.push_back(std::move(probe));
    }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (__builtin_available(macOS 26.0, *)) {
        CFTypeRef raw = nullptr;
        const auto status = VTSessionCopyProperty(session.value, kVTCompressionPropertyKey_SupportedPresetDictionaries, nullptr, &raw);
        result.native_presets_status = status;
        if (raw) { CFRef presets(raw); result.native_presets_json = property_json(raw); }
    }
#endif
    return result;
}

} // namespace vtenc
