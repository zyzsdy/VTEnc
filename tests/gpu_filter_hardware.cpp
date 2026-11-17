// Explicit GPU integration test. CPU code below is an independent double-precision
// reference used only for verification, never linked into VTEnc's processing path.
#include "gpu_filter.hpp"
#include "encoder_session.hpp"
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
double interpolate(const Image& input, int plane, double x, double y, const std::array<int, 4>& crop) {
    const int div = plane ? 2 : 1, width = input.width / div;
    const int min_x = crop[0] / div, min_y = crop[1] / div;
    const int max_x = (input.width - crop[2]) / div - 1, max_y = (input.height - crop[3]) / div - 1;
    x = std::clamp(x, static_cast<double>(min_x), static_cast<double>(max_x));
    y = std::clamp(y, static_cast<double>(min_y), static_cast<double>(max_y));
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, max_x), y1 = std::min(y0 + 1, max_y);
    const auto at = [&](int xx, int yy) { return input.planes[static_cast<std::size_t>(plane)][static_cast<std::size_t>(yy * width + xx)]; };
    return std::lerp(std::lerp(at(x0, y0), at(x1, y0), x - x0),
                     std::lerp(at(x0, y1), at(x1, y1), x - x0), y - y0);
}
std::array<double, 3> reference(const Image& input, double x, double y, const EncodeConfig& config, bool convert) {
    double cx = config.filter.input_chroma == ChromaLocation::center ? 1.0 : 0.5;
    double cy = config.filter.input_chroma == ChromaLocation::top_left ? 0.5 : 1.0;
    double yy = interpolate(input, 0, x - 0.5, y - 0.5, config.filter.crop);
    double u = interpolate(input, 1, (x - cx) / 2, (y - cy) / 2, config.filter.crop);
    double v = interpolate(input, 2, (x - cx) / 2, (y - cy) / 2, config.filter.crop);
    const double factor = input.depth == 10 ? 4 : 1, max = input.depth == 10 ? 1023 : 255;
    yy = (yy - (input.full ? 0 : 16 * factor)) / (input.full ? max : 219 * factor);
    u = (u - 128 * factor) / (input.full ? max : 224 * factor);
    v = (v - 128 * factor) / (input.full ? max : 224 * factor);
    if (convert) {
        const bool from601 = config.filter.input_matrix == ColorMatrix::bt601;
        const double kr = from601 ? 0.299 : 0.2126, kb = from601 ? 0.114 : 0.0722;
        const double r = yy + (2 - 2 * kr) * v, b = yy + (2 - 2 * kb) * u;
        const double g = (yy - kr * r - kb * b) / (1 - kr - kb);
        const double out_kr = from601 ? 0.2126 : 0.299, out_kb = from601 ? 0.0722 : 0.114;
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
int verify(const Image& image, CVPixelBufferRef output, const EncodeConfig& config, bool convert) {
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
            const auto expected = reference(image, xx, yy, config, convert)[static_cast<std::size_t>(plane)];
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
    std::cout << "PASS " << cases << " Metal pixel cases; maximum error=" << largest << " code value; P010 low bits zero\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
