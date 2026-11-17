// Explicit GPU integration test. CPU code below is an independent double-precision
// reference used only for verification, never linked into VTEnc's processing path.
#include "gpu_filter.hpp"
#include "encoder_session.hpp"
#include "colorspace_reference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using namespace vtenc;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Image {
    int width, height, depth;
    bool full;
    std::array<std::vector<double>, 3> planes;
};
CFRef<CVPixelBufferRef> pixel_buffer(const Image& image) {
    const int width = image.width, height = image.height;
    const OSType type = image.depth == 10
        ? (image.full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
        : (image.full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFRef surface(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const void* keys[] = {kCVPixelBufferMetalCompatibilityKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {kCFBooleanTrue, surface.get()};
    CFRef attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CVPixelBufferRef raw = nullptr;
    check_status(CVPixelBufferCreate(kCFAllocatorDefault, static_cast<std::size_t>(width), static_cast<std::size_t>(height), type, attributes.get(), &raw), "Create test pixel buffer");
    CFRef result(raw);
    check_status(CVPixelBufferLockBaseAddress(raw, 0), "Lock fixture");
    for (int plane = 0; plane < 3; ++plane) {
        const int w = plane ? width / 2 : width, h = plane ? height / 2 : height;
        const std::size_t cv_plane = plane ? 1 : 0;
        auto base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(raw, cv_plane));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(raw, cv_plane);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const int index = plane ? x * 2 + plane - 1 : x;
            const auto value = static_cast<unsigned>(image.planes[static_cast<std::size_t>(plane)][static_cast<std::size_t>(y * w + x)]);
            auto row = base + static_cast<std::size_t>(y) * stride;
            if (image.depth == 10) reinterpret_cast<std::uint16_t*>(row)[index] = static_cast<std::uint16_t>(value << 6);
            else row[index] = static_cast<std::uint8_t>(value);
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(raw, 0), "Unlock fixture");
    return result;
}
// Bilinear interpolation of code values with crop-local edge extension.
double interpolate(const Image& input, int plane, double x, double y, const std::array<int, 4>& crop, int field = -1) {
    const int div = plane ? 2 : 1, width = input.width / div;
    const int step = field >= 0 ? 2 : 1;
    const int min_x = crop[0] / div, min_y = crop[1] / div / step;
    const int max_x = (input.width - crop[2]) / div - 1, max_y = (input.height - crop[3]) / div / step - 1;
    x = std::clamp(x, static_cast<double>(min_x), static_cast<double>(max_x));
    y = std::clamp(y, static_cast<double>(min_y), static_cast<double>(max_y));
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, max_x), y1 = std::min(y0 + 1, max_y);
    const auto at = [&](int xx, int yy) { return input.planes[static_cast<std::size_t>(plane)][static_cast<std::size_t>((yy * step + (field >= 0 ? field : 0)) * width + xx)]; };
    return std::lerp(std::lerp(at(x0, y0), at(x1, y0), x - x0),
                     std::lerp(at(x0, y1), at(x1, y1), x - x0), y - y0);
}
std::array<double, 3> reference(const Image& input, double x, double y, const EncodeConfig& config, bool convert, int field = -1) {
    double cx = config.filter.input_chroma == ChromaLocation::center ? 1.0 : 0.5;
    double cy = config.filter.input_chroma == ChromaLocation::top_left ? 0.5 : 1.0;
    double yy = interpolate(input, 0, x - 0.5, y - 0.5, config.filter.crop);
    double u = interpolate(input, 1, (x - cx) / 2, (y - cy) / 2, config.filter.crop);
    double v = interpolate(input, 2, (x - cx) / 2, (y - cy) / 2, config.filter.crop);
    if (field >= 0) {
        yy = interpolate(input, 0, x - 0.5, (y - 0.5 - field) / 2, config.filter.crop, field);
        const auto field_y = (y - (2 * cy - 0.5 + field)) / 4;
        u = interpolate(input, 1, (x - cx) / 2, field_y, config.filter.crop, field);
        v = interpolate(input, 2, (x - cx) / 2, field_y, config.filter.crop, field);
    }
    const double factor = input.depth == 10 ? 4 : 1, max = input.depth == 10 ? 1023 : 255;
    const bool input_full = config.filter.input_range == ColorRange::automatic ? input.full : config.filter.input_range == ColorRange::full;
    yy = (yy - (input_full ? 0 : 16 * factor)) / (input_full ? max : 219 * factor);
    u = (u - 128 * factor) / (input_full ? max : 224 * factor);
    v = (v - 128 * factor) / (input_full ? max : 224 * factor);
    if (convert) {
        const auto [kr, kb] = color_reference::coefficients(config.filter.input_matrix);
        double r = yy + (2 - 2 * kr) * v, b = yy + (2 - 2 * kb) * u;
        double g = (yy - kr * r - kb * b) / (1 - kr - kb);
        if (color_reference::nonlinear(config.filter.colorspace)) {
            const auto rgb = color_reference::transform({r,g,b}, config.filter.colorspace);
            r = rgb[0]; g = rgb[1]; b = rgb[2];
        }
        const auto [out_kr, out_kb] = color_reference::coefficients(config.filter.output_matrix);
        yy = out_kr * r + (1 - out_kr - out_kb) * g + out_kb * b;
        u = (b - yy) / (2 - 2 * out_kb); v = (r - yy) / (2 - 2 * out_kr);
    }
    const double out_factor = config.video.bit_depth == 10 ? 4 : 1;
    const double out_max = config.video.bit_depth == 10 ? 1023 : 255;
    const bool full = config.filter.range == ColorRange::full;
    return {yy * (full ? out_max : 219 * out_factor) + (full ? 0 : 16 * out_factor),
            u * (full ? out_max : 224 * out_factor) + 128 * out_factor,
            v * (full ? out_max : 224 * out_factor) + 128 * out_factor};
}
int verify(const Image& image, CVPixelBufferRef output, const EncodeConfig& config, bool convert, int field = -1) {
    const int width = static_cast<int>(CVPixelBufferGetWidth(output)), height = static_cast<int>(CVPixelBufferGetHeight(output));
    const auto crop = config.filter.crop;
    const double scale_x = static_cast<double>(image.width - crop[0] - crop[2]) / width;
    const double scale_y = static_cast<double>(image.height - crop[1] - crop[3]) / height;
    int largest = 0;
    check_status(CVPixelBufferLockBaseAddress(output, kCVPixelBufferLock_ReadOnly), "Read GPU output");
    for (int plane = 0; plane < 3; ++plane) {
        const int w = plane ? width / 2 : width, h = plane ? height / 2 : height;
        const std::size_t cv_plane = plane ? 1 : 0;
        auto base = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(output, cv_plane));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(output, cv_plane);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const double xx = crop[0] + (plane ? 2 * x + 0.5 : x + 0.5) * scale_x;
            const double yy = crop[1] + (plane ? 2 * y + 1.0 : y + 0.5) * scale_y;
            auto expected = reference(image, xx, yy, config, convert, field)[static_cast<std::size_t>(plane)];
            if (plane && color_reference::nonlinear(config.filter.colorspace)) {
                expected = (reference(image, xx, yy - scale_y * .5, config, convert, field)[static_cast<std::size_t>(plane)] +
                            reference(image, xx, yy + scale_y * .5, config, convert, field)[static_cast<std::size_t>(plane)]) / 2;
            }
            require(std::isfinite(expected), "Nonfinite colorspace reference");
            const int max = config.video.bit_depth == 10 ? 1023 : 255;
            const int code = static_cast<int>(std::round(std::clamp(expected, 0.0, static_cast<double>(max))));
            const auto row = base + static_cast<std::size_t>(y) * stride;
            const int index = plane ? 2 * x + plane - 1 : x;
            int actual = row[index];
            if (config.video.bit_depth == 10) {
                const auto stored = reinterpret_cast<const std::uint16_t*>(row)[index];
                require((stored & 63) == 0, "P010 low bits are nonzero"); actual = stored >> 6;
            }
            largest = std::max(largest, std::abs(actual - code));
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(output, kCVPixelBufferLock_ReadOnly), "Unlock GPU output");
    return largest;
}
}
int main() try {
    int cases = 0, largest = 0;
    for (const int depth : {8, 10}) for (const bool full : {false, true}) {
        Image fixture{18, 14, depth, full, {}};
        const unsigned max = depth == 10 ? 1023 : 255;
        for (unsigned plane = 0; plane < 3; ++plane) {
            const int width = plane ? fixture.width / 2 : fixture.width, height = plane ? fixture.height / 2 : fixture.height;
            auto& data = fixture.planes[plane];
            // Saturated checkerboard/ramp including boundary pixels; padded CV rows are ignored.
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                data.push_back((static_cast<unsigned>(x * 29 + y * 67) + plane * 101) % (max + 1));
            }
        }
        auto pixel = pixel_buffer(fixture);
        CMVideoFormatDescriptionRef raw = nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel.get(), &raw), "Describe test image");
        CFRef format(raw);
        for (int out_depth : {8, 10}) for (bool out_full : {false, true})
        for (auto chroma : {ChromaLocation::left, ChromaLocation::center, ChromaLocation::top_left})
        for (int mode = 0; mode < 5; ++mode) {
            EncodeConfig config;
            config.video.bit_depth = out_depth;
            config.filter.range = out_full ? ColorRange::full : ColorRange::limited;
            config.filter.input_chroma = chroma;
            config.filter.input_matrix = mode == 4 ? ColorMatrix::bt601 : ColorMatrix::bt709;
            config.filter.output_matrix = mode == 4 ? ColorMatrix::bt709 : mode == 3 ? ColorMatrix::bt601 : ColorMatrix::bt709;
            if (mode) config.filter.crop = {2, 4, 4, 2};
            if (mode == 1) config.filter.output_size = {26, 18}; // upscale, noninteger ratio, crop clamp
            if (mode == 2) config.filter.output_size = {4, 2}; // downscale
            if (mode >= 3) config.filter.output_size = {10, 6}; // matrix + geometry
            GpuFilter filter(make_filter_plan(config, format.get(), pixel.get()));
            auto output = filter.process(pixel.get());
            const int error = verify(fixture, output.get(), config, mode >= 3);
            if (error > 1) throw std::runtime_error("GPU reference error exceeds one code value: " + std::to_string(error) +
                " depth=" + std::to_string(depth) + " output=" + std::to_string(out_depth) + " mode=" + std::to_string(mode));
            largest = std::max(largest, error);
            ++cases;
        }
        EncodeConfig invalid;
        invalid.filter.input_chroma = ChromaLocation::left;
        invalid.filter.crop = {2147483646, 0, 2147483646, 0};
        bool rejected = false;
        try { (void)make_filter_plan(invalid, format.get(), pixel.get()); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Overflowing crop was not rejected");
        invalid.filter.crop = {};
        invalid.filter.output_matrix = ColorMatrix::bt601;
        rejected = false;
        try { (void)make_filter_plan(invalid, format.get(), pixel.get()); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Unknown matrix was silently guessed");
        invalid.filter.output_matrix = ColorMatrix::automatic;
        invalid.filter.input_chroma = ChromaLocation::automatic;
        rejected = false;
        try { (void)make_filter_plan(invalid, format.get(), pixel.get()); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Unknown chroma location was silently guessed");
        invalid.filter.input_chroma = ChromaLocation::left;
        GpuFilter bounded(make_filter_plan(invalid, format.get(), pixel.get()));
        std::vector<CFRef<CVPixelBufferRef>> held;
        for (int n = 0; n < 3; ++n) held.push_back(bounded.process(pixel.get()));
        rejected = false;
        try { (void)bounded.process(pixel.get()); } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("Allocate bounded Metal output buffer") != std::string::npos;
        }
        require(rejected, "GPU output pool grew beyond three buffers");
        held.clear();
        for (int n = 0; n < 64; ++n) { auto recycled = bounded.process(pixel.get()); }
        CVBufferSetAttachment(pixel.get(), kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4,
                              kCVAttachmentMode_ShouldPropagate);
        rejected = false;
        try { (void)bounded.process(pixel.get()); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Midstream decoded color change was not rejected");
    }
    int color_cases = 0, color_error = 0;
    for (int depth : {8, 10}) for (bool full : {false, true}) {
        Image fixture{18, 16, depth, full, {}};
        const int max = depth == 10 ? 1023 : 255;
        for (int plane = 0; plane < 3; ++plane) {
            const int w = plane ? 9 : 18, h = plane ? 8 : 16;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                fixture.planes[static_cast<std::size_t>(plane)].push_back((x*67+y*29+plane*101) % (max+1));
        }
        auto pixel = pixel_buffer(fixture);
        CMVideoFormatDescriptionRef raw = nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, pixel.get(), &raw), "Describe colorspace fixture");
        CFRef format(raw);
        for (auto from : {ColorTransfer::linear, ColorTransfer::bt709, ColorTransfer::srgb, ColorTransfer::pq, ColorTransfer::hlg})
        for (auto to : {ColorTransfer::linear, ColorTransfer::bt709, ColorTransfer::srgb, ColorTransfer::pq, ColorTransfer::hlg})
        for (bool scene : {false, true}) {
            EncodeConfig config;
            config.video.bit_depth = depth == 8 ? 10 : 8;
            config.filter.range = full ? ColorRange::limited : ColorRange::full;
            config.filter.input_range = full ? ColorRange::limited : ColorRange::full; // Explicit reinterpretation.
            config.filter.input_chroma = ChromaLocation::center;
            config.filter.input_matrix = ColorMatrix::bt2020nc;
            config.filter.output_matrix = ColorMatrix::bt709;
            config.filter.crop = {2,4,4,4}; config.filter.output_size = {10,6};
            auto& color = config.filter.colorspace;
            color.enabled = true; color.input_transfer = from; color.output_transfer = to; color.scene_ref = scene;
            color.sdr_source_peak = scene ? 203.0F : 100.0F;
            color.input_primaries = ColorPrimaries::bt2020; color.output_primaries = ColorPrimaries::bt709;
            GpuFilter filter(make_filter_plan(config, format.get(), pixel.get()));
            auto output = filter.process(pixel.get());
            const auto error = verify(fixture, output.get(), config, true);
            if (error > 1) throw std::runtime_error("Colorspace GPU mismatch: " + std::to_string(error) +
                " from=" + std::to_string(static_cast<int>(from)) + " to=" + std::to_string(static_cast<int>(to)) + " scene=" + std::to_string(scene));
            color_error = std::max(color_error,error); ++color_cases;
            const auto check_attachment = [&](CFStringRef key, CFStringRef expected) {
                auto raw_value = CVBufferCopyAttachment(output.get(), key, nullptr);
                require(raw_value, "Missing colorspace output attachment"); CFRef value(raw_value);
                require(CFEqual(value.get(),expected), "Wrong colorspace output attachment");
            };
            check_attachment(kCVImageBufferTransferFunctionKey, CVTransferFunctionGetStringForIntegerCodePoint(static_cast<int>(to)));
            check_attachment(kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2);
            check_attachment(kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
        }
        // Identity must preserve code values, including PQ superwhite/negative RGB
        // excursions, instead of forcing a lossy gamma round trip.
        for (auto transfer : {ColorTransfer::linear, ColorTransfer::bt709, ColorTransfer::srgb, ColorTransfer::pq, ColorTransfer::hlg}) {
            EncodeConfig config;
            config.video.bit_depth = depth;
            config.filter.range = full ? ColorRange::full : ColorRange::limited;
            config.filter.input_chroma = ChromaLocation::left;
            config.filter.input_matrix = config.filter.output_matrix = ColorMatrix::bt2020nc;
            auto& color = config.filter.colorspace;
            color.enabled = true; color.input_transfer = color.output_transfer = transfer;
            color.input_primaries = color.output_primaries = ColorPrimaries::bt2020;
            GpuFilter filter(make_filter_plan(config,format.get(),pixel.get()));
            auto output = filter.process(pixel.get());
            require(verify(fixture,output.get(),config,false) == 0, "Identity colorspace changed code values");
            ++color_cases;
        }
        // P3 gamut conversion, all supported matrices, and nonlinear Bob sampling.
        for (auto matrix : {ColorMatrix::bt601, ColorMatrix::bt709, ColorMatrix::smpte240m, ColorMatrix::bt2020nc}) {
            EncodeConfig config;
            config.video.bit_depth = depth; config.filter.bob = true;
            config.filter.range = full ? ColorRange::full : ColorRange::limited;
            config.filter.input_matrix = matrix; config.filter.output_matrix = ColorMatrix::bt709;
            config.filter.input_chroma = ChromaLocation::left;
            auto& color = config.filter.colorspace;
            color.enabled = true; color.input_transfer = ColorTransfer::srgb; color.output_transfer = ColorTransfer::bt709;
            color.input_primaries = ColorPrimaries::display_p3; color.output_primaries = ColorPrimaries::bt709;
            GpuFilter filter(make_filter_plan(config,format.get(),pixel.get()));
            for (int field : {0,1}) {
                auto output = filter.process(pixel.get(),field);
                auto error = verify(fixture,output.get(),config,true,field);
                require(error <= 1, "P3/Bob colorspace mismatch");
                color_error = std::max(color_error,error); ++color_cases;
            }
        }
    }
    std::cout << "PASS " << color_cases << " colorspace pixel cases; maximum error=" << color_error << " code value\n";
    int tone_cases=0, tone_error=0;
    for (int depth : {8,10}) for (bool full : {false,true}) {
        Image fixture{18,16,depth,full,{}};
        const int max=depth == 10 ? 1023 : 255;
        for (int plane=0; plane<3; ++plane) {
            const int w=plane ? 9 : 18, h=plane ? 8 : 16;
            for (int y=0; y<h; ++y) for (int x=0; x<w; ++x)
                fixture.planes[static_cast<std::size_t>(plane)].push_back((x*67+y*29+plane*101) % (max+1));
        }
        auto pixel=pixel_buffer(fixture);
        CMVideoFormatDescriptionRef raw=nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,pixel.get(),&raw), "Describe tone mapping fixture");
        CFRef format(raw);
        for (auto mode : {ToneMap::hable,ToneMap::mobius,ToneMap::reinhard,ToneMap::bt2390})
        for (auto transfer : {ColorTransfer::pq,ColorTransfer::hlg}) for (int variant=0; variant<5; ++variant) {
            EncodeConfig config;
            config.video.bit_depth=variant == 1 ? (depth == 8 ? 10 : 8) : depth;
            config.filter.range=full ? ColorRange::full : ColorRange::limited;
            config.filter.input_chroma=ChromaLocation::left;
            config.filter.input_matrix=ColorMatrix::bt2020nc; config.filter.output_matrix=ColorMatrix::bt709;
            auto& color=config.filter.colorspace;
            color.enabled=true; color.input_transfer=transfer; color.output_transfer=ColorTransfer::bt709;
            color.input_primaries=ColorPrimaries::bt2020; color.output_primaries=ColorPrimaries::bt709;
            auto& tone=color.tonemap; tone.mode=mode;
            if (variant == 1) {
                tone.source_peak=4000; tone.ldr_nits=203; tone.desat_strength=1;
                tone.desat_base=.1F; tone.desat_exp=.5F;
                tone.hable={.3F,.4F,.2F,.3F,.02F,.4F}; tone.transition=.6F; tone.peak=2; tone.contrast=.7F;
                config.filter.crop={2,4,4,4}; config.filter.output_size={10,6};
            } else if (variant == 2) {
                tone.source_peak=tone.ldr_nits=100; tone.desat_strength=0;
            } else if (variant == 3) {
                tone.source_peak=10000; tone.ldr_nits=1;
                config.filter.bob=true; config.filter.crop={2,4,4,4}; config.filter.output_size={26,18};
            } else if (variant == 4) {
                tone.source_peak=100.001F; tone.ldr_nits=100;
                tone.transition=.999F; tone.contrast=.999F;
                tone.hable={0,1,1,1,0,1};
            }
            GpuFilter filter(make_filter_plan(config,format.get(),pixel.get()));
            for (int field=0; field<(config.filter.bob ? 2 : 1); ++field) {
                const int parity=config.filter.bob ? field : -1;
                auto output=filter.process(pixel.get(),parity);
                const auto error=verify(fixture,output.get(),config,true,parity);
                if (error > 1) throw std::runtime_error("Tone mapping mismatch: " + std::to_string(error) +
                    " mode=" + std::to_string(static_cast<int>(mode)) + " variant=" + std::to_string(variant) +
                    " transfer=" + std::to_string(static_cast<int>(transfer)) + " depth=" + std::to_string(depth));
                tone_error=std::max(tone_error,error); ++tone_cases;
            }
        }
    }
    // Neutral PQ luminance ramp checks actual GPU black, monotonic highlight
    // compression and near-black/100-nit/1000-nit anchors after quantization.
    Image gray{18,16,10,false,{}};
    constexpr double nits[] = {0,.001,.01,.1,1,5,18,50,100,200,400,600,800,1000,2000,4000,8000,10000};
    for (int y=0; y<16; ++y) for (auto luminance : nits)
        gray.planes[0].push_back(std::round(64+876*color_reference::encode(luminance/10000,ColorTransfer::pq,false)));
    gray.planes[1].assign(72,512); gray.planes[2].assign(72,512);
    auto gray_pixel=pixel_buffer(gray);
    CMVideoFormatDescriptionRef gray_raw=nullptr;
    check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,gray_pixel.get(),&gray_raw), "Describe PQ ramp");
    CFRef gray_format(gray_raw);
    for (auto mode : {ToneMap::hable,ToneMap::mobius,ToneMap::reinhard,ToneMap::bt2390}) for (int depth : {8,10}) {
        EncodeConfig config;
        config.video.bit_depth=depth; config.filter.range=ColorRange::limited;
        config.filter.input_chroma=ChromaLocation::left;
        config.filter.input_matrix=ColorMatrix::bt2020nc; config.filter.output_matrix=ColorMatrix::bt709;
        auto& color=config.filter.colorspace;
        color.enabled=true; color.tonemap.mode=mode;
        color.input_transfer=ColorTransfer::pq; color.output_transfer=ColorTransfer::bt709;
        color.input_primaries=ColorPrimaries::bt2020; color.output_primaries=ColorPrimaries::bt709;
        GpuFilter filter(make_filter_plan(config,gray_format.get(),gray_pixel.get()));
        auto output=filter.process(gray_pixel.get());
        const auto error=verify(gray,output.get(),config,true);
        require(error <= 1, "Tone mapping luminance anchors differ from reference");
        check_status(CVPixelBufferLockBaseAddress(output.get(),kCVPixelBufferLock_ReadOnly), "Read tone ramp");
        const auto row=static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(output.get(),0));
        int previous=0;
        for (int x=0; x<18; ++x) {
            const int code=depth == 8 ? row[x] : reinterpret_cast<const std::uint16_t*>(row)[x] >> 6;
            require(x != 0 || code == (depth == 8 ? 16 : 64), "Tone mapping raised GPU black");
            require(code >= previous, "GPU tone curve reversed highlight luminance"); previous=code;
        }
        check_status(CVPixelBufferUnlockBaseAddress(output.get(),kCVPixelBufferLock_ReadOnly), "Unlock tone ramp");
        tone_error=std::max(tone_error,error); ++tone_cases;
    }
    std::cout << "PASS " << tone_cases << " tone mapping pixel cases; maximum error=" << tone_error << " code value\n";
    int bob_cases = 0, bob_error = 0;
    for (int depth : {8, 10}) for (bool full : {false, true}) {
        Image fixture{18, 16, depth, full, {}};
        const int factor = depth == 10 ? 4 : 1;
        for (int plane = 0; plane < 3; ++plane) {
            const int w = plane ? 9 : 18, h = plane ? 8 : 16;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                // Opposite fields have deliberately different brightness/chroma.
                fixture.planes[static_cast<std::size_t>(plane)].push_back((16 + (y % 2) * 140 + x * 2 + y + plane * 5) * factor);
            }
        }
        auto pixel = pixel_buffer(fixture);
        CMVideoFormatDescriptionRef raw = nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, pixel.get(), &raw), "Describe Bob fixture");
        CFRef format(raw);
        for (int output_depth : {8, 10}) for (bool output_full : {false, true})
        for (auto chroma : {ChromaLocation::left, ChromaLocation::center, ChromaLocation::top_left})
        for (int geometry = 0; geometry < 3; ++geometry) {
            EncodeConfig config;
            config.filter.bob = true;
            config.video.bit_depth = output_depth;
            config.filter.input_chroma = chroma;
            config.filter.input_matrix = ColorMatrix::bt709;
            config.filter.output_matrix = geometry == 2 ? ColorMatrix::bt601 : ColorMatrix::bt709;
            config.filter.range = output_full ? ColorRange::full : ColorRange::limited;
            if (geometry) {
                config.filter.crop = {2, 4, 4, 4};
                config.filter.output_size = geometry == 1 ? std::array<int, 2>{26, 18} : std::array<int, 2>{6, 4};
            }
            GpuFilter filter(make_filter_plan(config, format.get(), pixel.get()));
            for (int field : {0, 1}) {
                auto output = filter.process(pixel.get(), field);
                const auto error = verify(fixture, output.get(), config, geometry == 2, field);
                require(error <= 1, "Bob mixed fields or exceeded one target code value");
                bob_error = std::max(bob_error, error); ++bob_cases;
                auto raw_count = CVBufferCopyAttachment(output.get(), kCVImageBufferFieldCountKey, nullptr);
                require(raw_count, "Bob output lacks progressive field count");
                CFRef count(raw_count); int number = 0;
                require(CFNumberGetValue(static_cast<CFNumberRef>(count.get()), kCFNumberIntType, &number) && number == 1, "Bob output is not progressive");
            }
        }
    }
    std::cout << "PASS " << bob_cases << " Bob field pixel cases; maximum error=" << bob_error << " code value\n";
    std::cout << "PASS " << cases << " Metal pixel cases; maximum error=" << largest << " code value; P010 low bits zero\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
