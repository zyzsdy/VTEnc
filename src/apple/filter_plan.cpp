#include "gpu_filter.hpp"
#include "output_metadata.hpp"
#include "encoder_session.hpp"
#include "vtenc/colorspace.hpp"

#include <algorithm>
#include <array>

namespace vtenc {
namespace {
struct PixelFormat { int depth; bool full; };
PixelFormat describe(OSType type) {
    switch (type) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: return {8, false};
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: return {8, true};
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange: return {10, false};
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: return {10, true};
    default: throw std::runtime_error("Metal filter requires NV12/P010; pixel format=" + std::to_string(type));
    }
}
CFTypeRef metadata(CMFormatDescriptionRef format, CFDictionaryRef pixel, CFStringRef key) {
    if (const auto value = CFDictionaryGetValue(pixel, key)) return value;
    return CMFormatDescriptionGetExtension(format, key);
}
ColorMatrix matrix_value(CFTypeRef value) {
    if (value && CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) return ColorMatrix::bt601;
    if (value && CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) return ColorMatrix::bt709;
    if (value && CFEqual(value, kCVImageBufferYCbCrMatrix_ITU_R_2020)) return ColorMatrix::bt2020nc;
    if (value && CFEqual(value, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995)) return ColorMatrix::smpte240m;
    return ColorMatrix::automatic;
}
CFStringRef matrix_name(ColorMatrix matrix) {
    if (matrix == ColorMatrix::bt2020nc) return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    if (matrix == ColorMatrix::smpte240m) return kCVImageBufferYCbCrMatrix_SMPTE_240M_1995;
    return matrix == ColorMatrix::bt601 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4 : kCVImageBufferYCbCrMatrix_ITU_R_709_2;
}
ColorTransfer transfer_value(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return ColorTransfer::automatic;
    switch (CVTransferFunctionGetIntegerCodePointForString(static_cast<CFStringRef>(value))) {
    case 1: case 6: case 14: case 15: return ColorTransfer::bt709;
    case 8: return ColorTransfer::linear;
    case 13: return ColorTransfer::srgb;
    case 16: return ColorTransfer::pq;
    case 18: return ColorTransfer::hlg;
    default: return ColorTransfer::automatic;
    }
}
ColorPrimaries primaries_value(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return ColorPrimaries::automatic;
    switch (CVColorPrimariesGetIntegerCodePointForString(static_cast<CFStringRef>(value))) {
    case 1: return ColorPrimaries::bt709;
    case 5: return ColorPrimaries::bt470bg;
    case 6: case 7: return ColorPrimaries::smpte170m;
    case 9: return ColorPrimaries::bt2020;
    case 11: return ColorPrimaries::dci_p3;
    case 12: return ColorPrimaries::display_p3;
    default: return ColorPrimaries::automatic;
    }
}

}

bool needs_gpu_filter(const EncodeConfig& config, CVPixelBufferRef source) {
    return !config.subtitles.empty() || config.filter.bob || config.filter.ivtc.enabled || config.filter.requested || describe(CVPixelBufferGetPixelFormatType(source)).depth != config.video.bit_depth;
}

FilterPlan make_filter_plan(const EncodeConfig& config, CMFormatDescriptionRef format, CVPixelBufferRef source) {
    auto raw_attachments = CVBufferCopyAttachments(source, kCVAttachmentMode_ShouldPropagate);
    if (!raw_attachments) raw_attachments = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRef source_attachments(raw_attachments);
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(format);
    const auto width = dimensions.width;
    const auto height = dimensions.height;
    const auto& crop = config.filter.crop;
    const auto cropped_width = static_cast<std::int64_t>(width) - crop[0] - crop[2];
    const auto cropped_height = static_cast<std::int64_t>(height) - crop[1] - crop[3];
    if (width <= 0 || height <= 0 || width % 2 || height % 2 || cropped_width <= 0 || cropped_height <= 0 ||
        std::ranges::any_of(crop, [](int x) { return x < 0 || x % 2; })) {
        throw std::runtime_error("GPU crop must leave a positive even 4:2:0 image; source=" +
            std::to_string(width) + "x" + std::to_string(height));
    }
    if (config.filter.bob && (height % 4 || crop[1] % 4 || crop[3] % 4 || cropped_height < 4)) {
        throw std::runtime_error("Bob requires source height and vertical crop margins divisible by four, leaving at least four rows");
    }
    const auto size = config.filter.output_size.value_or(std::array<int, 2>{static_cast<int>(cropped_width), static_cast<int>(cropped_height)});
    if (size[0] <= 0 || size[1] <= 0 || size[0] % 2 || size[1] % 2) throw std::runtime_error("GPU output dimensions must be positive and even");
    const auto input_type = CVPixelBufferGetPixelFormatType(source);
    auto input = describe(input_type);
    if (config.filter.input_range != ColorRange::automatic) input.full = config.filter.input_range == ColorRange::full;
    const bool full = config.filter.range == ColorRange::automatic ? input.full : config.filter.range == ColorRange::full;
    if (config.video.bit_depth != 8 && config.video.bit_depth != 10) throw std::runtime_error("GPU output depth must be 8 or 10");
    const OSType output_type = config.video.bit_depth == 10
        ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
        : (full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFRef attachments(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    // Carry semantic metadata only. The scheduler validates static HDR applicability before encoding.
    for (const auto key : {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey,
                          kCVImageBufferYCbCrMatrixKey, kCVImageBufferPixelAspectRatioKey}) {
        if (auto value = metadata(format, source_attachments.get(), key)) CFDictionarySetValue(attachments.get(), key, value);
    }
    // Keep original HDR evidence separately, never attach stale HDR labels to
    // transformed pixels. The scheduler decides which evidence is applicable.
    for (const auto pair : {std::pair{kCMFormatDescriptionExtension_MasteringDisplayColorVolume,source_master_key()},
                            std::pair{kCMFormatDescriptionExtension_ContentLightLevelInfo,source_light_key()}}) {
        auto value=metadata(format,source_attachments.get(),pair.second);
        if (!value) value=metadata(format,source_attachments.get(),pair.first);
        if (value) CFDictionarySetValue(attachments.get(),pair.second,value);
    }
    const auto transfer = metadata(format, source_attachments.get(), kCVImageBufferTransferFunctionKey);
    if (!config.filter.colorspace.enabled && transfer && (CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) ||
                     CFEqual(transfer, kCVImageBufferTransferFunction_ITU_R_2100_HLG))) {
        throw std::runtime_error("HDR filtering requires explicit --vpp-colorspace conversion");
    }
    const auto& color = config.filter.colorspace;
    const bool tone = color.tonemap.mode != ToneMap::none;
    validate_tonemap(color.tonemap);
    auto input_matrix = config.filter.input_matrix;
    if (input_matrix == ColorMatrix::automatic) input_matrix = matrix_value(metadata(format, source_attachments.get(), kCVImageBufferYCbCrMatrixKey));
    auto output_matrix = config.filter.output_matrix;
    if (output_matrix == ColorMatrix::automatic) output_matrix = tone ? ColorMatrix::bt709 : input_matrix;
    if (output_matrix != ColorMatrix::automatic && input_matrix == ColorMatrix::automatic) {
        throw std::runtime_error("Missing or unsupported source YCbCr matrix; specify --vpp-colorspace matrix=FROM:TO or --input-matrix");
    }
    if (output_matrix != ColorMatrix::automatic) CFDictionarySetValue(attachments.get(), kCVImageBufferYCbCrMatrixKey, matrix_name(output_matrix));
    auto in_transfer = color.input_transfer;
    if (in_transfer == ColorTransfer::automatic) in_transfer = transfer_value(transfer);
    const auto out_transfer = color.output_transfer == ColorTransfer::automatic ? (tone ? ColorTransfer::bt709 : in_transfer) : color.output_transfer;
    auto in_primaries = color.input_primaries;
    if (in_primaries == ColorPrimaries::automatic)
        in_primaries = primaries_value(metadata(format, source_attachments.get(), kCVImageBufferColorPrimariesKey));
    const auto out_primaries = color.output_primaries == ColorPrimaries::automatic ? (tone ? ColorPrimaries::bt709 : in_primaries) : color.output_primaries;
    const bool linear = tone || in_transfer != out_transfer || in_primaries != out_primaries;
    if (tone && color.scene_ref) throw std::runtime_error("hdr2sdr requires scene_ref=false (display luminance)");
    if (tone && in_transfer != ColorTransfer::pq && in_transfer != ColorTransfer::hlg)
        throw std::runtime_error("hdr2sdr requires known PQ or HLG input transfer; specify transfer=FROM:TO");
    if (tone && (out_transfer == ColorTransfer::pq || out_transfer == ColorTransfer::hlg))
        throw std::runtime_error("hdr2sdr requires an SDR output transfer");
    if (linear && (in_transfer == ColorTransfer::automatic || out_transfer == ColorTransfer::automatic))
        throw std::runtime_error("Missing or unsupported source transfer; specify --vpp-colorspace transfer=FROM:TO");
    if (linear && (input_matrix == ColorMatrix::automatic || output_matrix == ColorMatrix::automatic))
        throw std::runtime_error("Missing or unsupported source matrix; specify --vpp-colorspace matrix=FROM:TO");
    if (in_primaries != out_primaries && (in_primaries == ColorPrimaries::automatic || out_primaries == ColorPrimaries::automatic))
        throw std::runtime_error("Missing or unsupported source primaries; specify --vpp-colorspace colorprim=FROM:TO");
    if (color.enabled) {
        if (out_transfer != ColorTransfer::automatic && (tone || color.input_transfer != ColorTransfer::automatic || color.output_transfer != ColorTransfer::automatic)) {
            const auto name = CVTransferFunctionGetStringForIntegerCodePoint(static_cast<int>(out_transfer));
            if (!name) throw std::runtime_error("CoreVideo cannot represent output transfer");
            CFDictionarySetValue(attachments.get(), kCVImageBufferTransferFunctionKey, name);
        }
        if (out_primaries != ColorPrimaries::automatic && (tone || color.input_primaries != ColorPrimaries::automatic || color.output_primaries != ColorPrimaries::automatic)) {
            const auto name = CVColorPrimariesGetStringForIntegerCodePoint(static_cast<int>(out_primaries));
            if (!name) throw std::runtime_error("CoreVideo cannot represent output primaries");
            CFDictionarySetValue(attachments.get(), kCVImageBufferColorPrimariesKey, name);
        }
    }
    auto chroma = config.filter.input_chroma;
    if (chroma == ChromaLocation::automatic) {
        const auto value = metadata(format, source_attachments.get(), kCVImageBufferChromaLocationTopFieldKey);
        if (value && CFEqual(value, kCVImageBufferChromaLocation_Left)) chroma = ChromaLocation::left;
        else if (value && CFEqual(value, kCVImageBufferChromaLocation_Center)) chroma = ChromaLocation::center;
        else if (value && CFEqual(value, kCVImageBufferChromaLocation_TopLeft)) chroma = ChromaLocation::top_left;
        else throw std::runtime_error("Missing or unsupported source chroma location; specify --input-chroma-loc left|center|top-left");
    }
    auto bottom_chroma = chroma;
    if (config.filter.bob && config.filter.input_chroma == ChromaLocation::automatic) {
        const auto value = metadata(format, source_attachments.get(), kCVImageBufferChromaLocationBottomFieldKey);
        if (value && CFEqual(value, kCVImageBufferChromaLocation_Left)) bottom_chroma = ChromaLocation::left;
        else if (value && CFEqual(value, kCVImageBufferChromaLocation_Center)) bottom_chroma = ChromaLocation::center;
        else if (value && CFEqual(value, kCVImageBufferChromaLocation_TopLeft)) bottom_chroma = ChromaLocation::top_left;
        else throw std::runtime_error("Missing or unsupported bottom-field chroma location; specify --input-chroma-loc");
    }
    if (config.filter.bob || config.filter.ivtc.enabled) {
        const int count = 1;
        CFRef number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &count));
        CFDictionarySetValue(attachments.get(), kCVImageBufferFieldCountKey, number.get());
    }
    // H.264/HEVC output uses left-sited 4:2:0. Resample other input locations explicitly.
    CFDictionarySetValue(attachments.get(), kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocation_Left);
    CFDictionarySetValue(attachments.get(), kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferChromaLocation_Left);
    const bool geometry = crop != std::array<int, 4>{} || size != std::array<int, 2>{width, height};
    if (!geometry) {
        if (const auto aperture = metadata(format, source_attachments.get(), kCVImageBufferCleanApertureKey)) {
            CFDictionarySetValue(attachments.get(), kCVImageBufferCleanApertureKey, aperture);
        }
    }
    FilterParameters p{};
    p.crop_x = static_cast<float>(crop[0]); p.crop_y = static_cast<float>(crop[1]);
    p.crop_width = static_cast<float>(cropped_width); p.crop_height = static_cast<float>(cropped_height);
    p.output_width = static_cast<float>(size[0]); p.output_height = static_cast<float>(size[1]);
    p.chroma_x = chroma == ChromaLocation::center ? 1.0F : 0.5F;
    p.chroma_y = chroma == ChromaLocation::top_left ? 0.5F : 1.0F;
    const float in_factor = input.depth == 10 ? 4.0F : 1.0F;
    const float out_factor = config.video.bit_depth == 10 ? 4.0F : 1.0F;
    const float in_max = input.depth == 10 ? 1023.0F : 255.0F;
    p.input_storage = input.depth == 10 ? 65535.0F / 64.0F : 255.0F;
    p.input_y_offset = input.full ? 0 : 16 * in_factor;
    p.input_y_scale = input.full ? in_max : 219 * in_factor;
    p.input_c_offset = 128 * in_factor;
    p.input_c_scale = input.full ? in_max : 224 * in_factor;
    p.output_max = config.video.bit_depth == 10 ? 1023.0F : 255.0F;
    p.output_storage = config.video.bit_depth == 10 ? 64.0F / 65535.0F : 1.0F / 255.0F;
    p.output_y_offset = full ? 0 : 16 * out_factor;
    p.output_y_scale = full ? p.output_max : 219 * out_factor;
    p.output_c_offset = 128 * out_factor;
    p.output_c_scale = full ? p.output_max : 224 * out_factor;
    if (input_matrix != ColorMatrix::automatic) {
        const auto in = matrix_coefficients(input_matrix), out = matrix_coefficients(output_matrix);
        p.input_kr = in[0]; p.input_kb = in[1]; p.output_kr = out[0]; p.output_kb = out[1];
    }
    p.convert_matrix = input_matrix != output_matrix || linear;
    p.convert_transfer = linear;
    p.input_transfer = static_cast<unsigned>(in_transfer); p.output_transfer = static_cast<unsigned>(out_transfer);
    p.scene_ref = color.scene_ref; p.linear_peak = tone ? color.tonemap.ldr_nits : color.sdr_source_peak;
    p.tonemap = color.tonemap;
    if (in_primaries != out_primaries) {
        const auto gamut = gamut_matrix(in_primaries, out_primaries);
        std::copy(gamut.begin(), gamut.end(), p.gamut);
    }
    if (linear && !color.scene_ref && (in_transfer == ColorTransfer::hlg || out_transfer == ColorTransfer::hlg)) {
        if (in_primaries == ColorPrimaries::automatic || out_primaries == ColorPrimaries::automatic)
            throw std::runtime_error("Display-referred HLG requires known primaries; specify --vpp-colorspace colorprim=FROM:TO");
        const auto in = primaries_luma(in_primaries), out = primaries_luma(out_primaries);
        std::copy(in.begin(), in.end(), p.input_luma); std::copy(out.begin(), out.end(), p.output_luma);
    }
    p.bob = config.filter.bob;
    CFRetain(attachments.get());
    FilterPlan plan{width, height, size[0], size[1], input_type, output_type, p, CFRef<CFDictionaryRef>(attachments.get()), std::move(source_attachments)};
    plan.bottom_chroma_x = bottom_chroma == ChromaLocation::center ? 1.0F : 0.5F;
    plan.bottom_chroma_y = bottom_chroma == ChromaLocation::top_left ? 0.5F : 1.0F;
    validate_filter_frame(plan, source);
    return plan;
}

void validate_filter_frame(const FilterPlan& plan, CVPixelBufferRef frame) {
    if (CVPixelBufferGetPixelFormatType(frame) != plan.input_format ||
        CVPixelBufferGetWidth(frame) != static_cast<std::size_t>(plan.input_width) ||
        CVPixelBufferGetHeight(frame) != static_cast<std::size_t>(plan.input_height) || CVPixelBufferGetPlaneCount(frame) != 2) {
        throw std::runtime_error("Decoded pixel layout changed during GPU processing");
    }
    for (const auto key : {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey,
                          kCVImageBufferYCbCrMatrixKey, kCVImageBufferChromaLocationTopFieldKey,
                          kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferFieldCountKey, kCVImageBufferFieldDetailKey,
                          kCVImageBufferPixelAspectRatioKey, kCVImageBufferCleanApertureKey,
                          kCMFormatDescriptionExtension_MasteringDisplayColorVolume, kCMFormatDescriptionExtension_ContentLightLevelInfo, source_master_key(), source_light_key()}) {
        const auto expected = CFDictionaryGetValue(plan.source_attachments.get(), key);
        auto raw = CVBufferCopyAttachment(frame, key, nullptr);
        if (!raw) {
            if (expected) throw std::runtime_error("Decoded image metadata disappeared during GPU processing");
        } else {
            CFRef actual(raw);
            if (!expected || !CFEqual(expected, actual.get())) throw std::runtime_error("Decoded image metadata changed during GPU processing");
        }
    }
}

CFRef<CVPixelBufferPoolRef> make_filter_pool(const FilterPlan& plan) {
    return make_metal_pixel_pool(plan.width, plan.height, plan.output_format);
}
CFRef<CVPixelBufferPoolRef> make_metal_pixel_pool(int width, int height, OSType format) {
    CFRef dictionary(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto set_int = [&](CFStringRef key, int value) {
        CFRef number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value));
        CFDictionarySetValue(dictionary.get(), key, number.get());
    };
    set_int(kCVPixelBufferWidthKey, width);
    set_int(kCVPixelBufferHeightKey, height);
    set_int(kCVPixelBufferPixelFormatTypeKey, static_cast<int>(format));
    CFDictionarySetValue(dictionary.get(), kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    CFRef surface(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(dictionary.get(), kCVPixelBufferIOSurfacePropertiesKey, surface.get());
    CVPixelBufferPoolRef raw = nullptr;
    check_status(CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, dictionary.get(), &raw), "Create Metal output pixel pool");
    return CFRef(raw);
}
CFRef<CVPixelBufferRef> make_filter_buffer(CVPixelBufferPoolRef pool, const FilterPlan& plan) {
    const int limit = 3;
    CFRef number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &limit));
    const void* keys[] = {kCVPixelBufferPoolAllocationThresholdKey};
    const void* values[] = {number.get()};
    CFRef options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CVPixelBufferRef raw = nullptr;
    check_status(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool, options.get(), &raw), "Allocate bounded Metal output buffer");
    CFRef result(raw);
    CVBufferRemoveAllAttachments(raw);
    CVBufferSetAttachments(raw, plan.attachments.get(), kCVAttachmentMode_ShouldPropagate);
    return result;
}
} // namespace vtenc
