#pragma once
#include "gpu_filter.hpp"
#include "decoded_frames.hpp"
#include <span>
#include <vector>
namespace vtenc {
struct SubtitleBitmap {
    const unsigned char* data;
    int width, height, stride, x, y;
    std::uint32_t color; // libass RRGGBBAA (AA = transparency).
    float opacity, white_nits;
    std::array<int,4> clip;
    unsigned color_matrix = 0; // 0=None; 1/2=TV/PC.601, 3/4=709, 5/6=240M, 7/8=FCC.
};
struct SubtitleParameters {
    float storage, y_offset, y_scale, c_offset, c_scale, output_storage, maximum;
    float kr, kb;
    unsigned transfer;
    float luma[3], gamut[9];
};
class GpuSubtitles {
public:
    GpuSubtitles(CVPixelBufferRef source);
    ~GpuSubtitles();
    CFRef<CVPixelBufferRef> process(CVPixelBufferRef source, std::span<const SubtitleBitmap> images, bool changed);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class SubtitleRenderer {
public:
    SubtitleRenderer(const std::vector<SubtitleConfig>& config, int width, int height, double sar);
    ~SubtitleRenderer();
    std::span<const SubtitleBitmap> render(CMTime pts);
    bool changed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
