#pragma once

#include "vtenc/command_line.hpp"
#include "cf_ref.hpp"

#include <VideoToolbox/VideoToolbox.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace vtenc {

inline void check_status(OSStatus status, const std::string& operation) {
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

inline void create_hardware_session(CompressionSession& session, const ProbeConfig& config,
    VTCompressionOutputCallback callback, void* context, OSType source_pixel_format = 0) {
    if ((config.codec != Codec::h264 && config.codec != Codec::hevc) ||
        (config.bit_depth != 8 && config.bit_depth != 10) ||
        (config.codec == Codec::h264 && config.bit_depth != 8) ||
        config.width <= 0 || config.height <= 0 || config.width % 2 || config.height % 2) {
        throw std::invalid_argument("Invalid hardware encoder configuration");
    }
    const void* spec_keys[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
    const void* spec_values[] = {kCFBooleanTrue};
    CFRef specification(CFDictionaryCreate(kCFAllocatorDefault, spec_keys, spec_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto pixel_format = static_cast<std::int32_t>(source_pixel_format ? source_pixel_format : config.bit_depth == 10
        ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFRef pixel_number(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format));
    const void* pixel_keys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void* pixel_values[] = {pixel_number.get()};
    CFRef attributes(CFDictionaryCreate(kCFAllocatorDefault, pixel_keys, pixel_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto description = std::string(codec_name(config.codec)) + " " + std::to_string(config.bit_depth)
        + "-bit " + std::to_string(config.width) + "x" + std::to_string(config.height);
    check_status(VTCompressionSessionCreate(kCFAllocatorDefault, config.width, config.height,
        config.codec == Codec::h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC,
        specification.get(), attributes.get(), nullptr, callback, context, &session.value),
        "VTCompressionSessionCreate [hardware required; " + description + "]");
    if (!session.value) throw std::runtime_error("VTCompressionSessionCreate returned no session");
    const auto profile = config.codec == Codec::h264 ? kVTProfileLevel_H264_High_AutoLevel
        : config.bit_depth == 10 ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
    check_status(VTSessionSetProperty(session.value, kVTCompressionPropertyKey_ProfileLevel, profile),
        "VTSessionSetProperty(ProfileLevel) [" + description + "]");
}

inline void verify_hardware(VTCompressionSessionRef session) {
    CFTypeRef raw = nullptr;
    check_status(VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
        kCFAllocatorDefault, &raw), "VTSessionCopyProperty(UsingHardwareAcceleratedVideoEncoder)");
    CFRef hardware(raw);
    if (CFGetTypeID(hardware.get()) != CFBooleanGetTypeID() || !CFEqual(hardware.get(), kCFBooleanTrue)) {
        throw std::runtime_error("VideoToolbox did not confirm hardware encoding; software fallback is forbidden");
    }
}

inline void configure_hardware_encoder(VTCompressionSessionRef session, const EncodeConfig& config, CMFormatDescriptionRef source_format = nullptr) {
    const auto set = [&](CFStringRef key, CFTypeRef value, const char* name) {
        check_status(VTSessionSetProperty(session, key, value), std::string("VTSessionSetProperty(") + name + ")");
    };
    const auto set_integer = [&](CFStringRef key, std::int32_t value, const char* name) {
        CFRef number(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value));
        set(key, number.get(), name);
    };
    set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, "AllowFrameReordering");
    set(kVTCompressionPropertyKey_RealTime, kCFBooleanFalse, "RealTime");
    set_integer(kVTCompressionPropertyKey_MaxKeyFrameInterval, config.gop, "MaxKeyFrameInterval");
    if (config.filter.bob) set_integer(kVTCompressionPropertyKey_FieldCount, 1, "FieldCount [Bob progressive output]");
    if (!source_format) {
        // Floating point is only an encoder rate hint. All submitted times use CMTime integers.
        const double fps = static_cast<double>(config.fps_num) / config.fps_den;
        CFRef rate(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &fps));
        set(kVTCompressionPropertyKey_ExpectedFrameRate, rate.get(), "ExpectedFrameRate");
    }
    if (config.quality) {
        CFRef quality(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &*config.quality));
        set(kVTCompressionPropertyKey_Quality, quality.get(), "Quality");
    } else set_integer(kVTCompressionPropertyKey_AverageBitRate, config.bitrate, "AverageBitRate");
    if (!source_format) {
        set(kVTCompressionPropertyKey_ColorPrimaries, kCMFormatDescriptionColorPrimaries_ITU_R_709_2, "ColorPrimaries");
        set(kVTCompressionPropertyKey_TransferFunction, kCMFormatDescriptionTransferFunction_ITU_R_709_2, "TransferFunction");
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2, "YCbCrMatrix");
    } else {
        const struct { CFStringRef source; CFStringRef destination; const char* name; } keys[] = {
            {kCMFormatDescriptionExtension_ColorPrimaries, kVTCompressionPropertyKey_ColorPrimaries, "ColorPrimaries"},
            {kCMFormatDescriptionExtension_TransferFunction, kVTCompressionPropertyKey_TransferFunction, "TransferFunction"},
            {kCMFormatDescriptionExtension_YCbCrMatrix, kVTCompressionPropertyKey_YCbCrMatrix, "YCbCrMatrix"},
            {kCMFormatDescriptionExtension_PixelAspectRatio, kVTCompressionPropertyKey_PixelAspectRatio, "PixelAspectRatio"},
            {kCMFormatDescriptionExtension_CleanAperture, kVTCompressionPropertyKey_CleanAperture, "CleanAperture"},
        };
        for (const auto& key : keys) {
            if (const auto value = CMFormatDescriptionGetExtension(source_format, key.source)) {
                set(key.destination, value, key.name);
            }
        }
    }
}

} // namespace vtenc
