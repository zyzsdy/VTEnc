#pragma once

#include "vtenc/command_line.hpp"
#include "cf_ref.hpp"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <memory>

namespace vtenc {
// Scalar-only layout shared with Metal. Coordinates are luma pixel edge coordinates.
struct FilterParameters {
    float crop_x, crop_y, crop_width, crop_height;
    float output_width, output_height;
    float chroma_x, chroma_y;
    float input_storage, input_y_offset, input_y_scale, input_c_offset, input_c_scale;
    float output_storage, output_y_offset, output_y_scale, output_c_offset, output_c_scale, output_max;
    float input_kr, input_kb, output_kr, output_kb;
    unsigned convert_matrix;
};
struct FilterPlan {
    int input_width, input_height, width, height;
    OSType input_format, output_format;
    FilterParameters parameters{};
    CFRef<CFDictionaryRef> attachments;
    CFRef<CFDictionaryRef> source_attachments;
};
[[nodiscard]] bool needs_gpu_filter(const EncodeConfig& config, CVPixelBufferRef source);
[[nodiscard]] FilterPlan make_filter_plan(const EncodeConfig& config, CMFormatDescriptionRef format, CVPixelBufferRef source);
[[nodiscard]] CFRef<CVPixelBufferPoolRef> make_filter_pool(const FilterPlan& plan);
[[nodiscard]] CFRef<CVPixelBufferRef> make_filter_buffer(CVPixelBufferPoolRef pool, const FilterPlan& plan);
void validate_filter_frame(const FilterPlan& plan, CVPixelBufferRef frame);

class GpuFilter {
public:
    explicit GpuFilter(FilterPlan plan);
    ~GpuFilter();
    // Returns a completed GPU buffer. No CPU pixel access, one command buffer in flight.
    [[nodiscard]] CFRef<CVPixelBufferRef> process(CVPixelBufferRef source);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vtenc
