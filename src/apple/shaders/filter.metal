#include <metal_stdlib>
using namespace metal;
struct Parameters {
    float crop_x, crop_y, crop_width, crop_height;
    float output_width, output_height;
    float chroma_x, chroma_y;
    float input_storage, input_y_offset, input_y_scale, input_c_offset, input_c_scale;
    float output_storage, output_y_offset, output_y_scale, output_c_offset, output_c_scale, output_max;
    float input_kr, input_kb, output_kr, output_kb;
    uint convert_matrix;
    uint bob, field;
};
// Explicit float interpolation avoids hardware sampler fractional-weight quantization
// when converting 8-bit textures to 10-bit output.
float4 bilinear(texture2d<float, access::read> tex, float2 position, float2 minimum, float2 maximum) {
    float2 coord = clamp(position - 0.5f, minimum, maximum);
    uint2 low = uint2(floor(coord));
    uint2 high = min(low + 1, uint2(maximum));
    float2 weight = fract(coord);
    return mix(mix(tex.read(low), tex.read(uint2(high.x, low.y)), weight.x),
               mix(tex.read(uint2(low.x, high.y)), tex.read(high), weight.x), weight.y);
}
// A field has half the rows in each plane. Never interpolate across field parity.
float4 field_bilinear(texture2d<float, access::read> tex, float2 coord,
                      float2 minimum, float2 maximum, uint parity) {
    coord = clamp(coord, minimum, maximum);
    uint2 low = uint2(floor(coord)), high = min(low + 1, uint2(maximum));
    float2 weight = fract(coord);
    low.y = low.y * 2 + parity; high.y = high.y * 2 + parity;
    return mix(mix(tex.read(low), tex.read(uint2(high.x, low.y)), weight.x),
               mix(tex.read(uint2(low.x, high.y)), tex.read(high), weight.x), weight.y);
}
float3 sample_yuv(texture2d<float, access::read> y, texture2d<float, access::read> uv,
                  float2 position, constant Parameters& p) {
    float2 start(p.crop_x, p.crop_y), extent(p.crop_width, p.crop_height);
    // Clamp to the cropped rectangle, not to the original texture's edges.
    float yy = bilinear(y, position, start, start + extent - 1.0f).r;
    float2 chroma_position = (position - float2(p.chroma_x, p.chroma_y)) * 0.5f + 0.5f;
    float2 cc = bilinear(uv, chroma_position, start * 0.5f, (start + extent) * 0.5f - 1.0f).rg;
    if (p.bob) {
        yy = field_bilinear(y, float2(position.x - 0.5f, (position.y - 0.5f - p.field) * 0.5f),
            float2(start.x, start.y * 0.5f), float2(start.x + extent.x - 1.0f, (start.y + extent.y) * 0.5f - 1.0f), p.field).r;
        // Chroma siting is expressed in field-local luma coordinates. For left
        // siting, top/bottom field chroma centers are full-frame y=1.5/2.5.
        float2 field_chroma((position.x - p.chroma_x) * 0.5f,
            (position.y - (2.0f * p.chroma_y - 0.5f + p.field)) * 0.25f);
        cc = field_bilinear(uv, field_chroma, float2(start.x * 0.5f, start.y * 0.25f),
            float2((start.x + extent.x) * 0.5f - 1.0f, (start.y + extent.y) * 0.25f - 1.0f), p.field).rg;
    }
    float3 value((yy * p.input_storage - p.input_y_offset) / p.input_y_scale,
                 (cc * p.input_storage - p.input_c_offset) / p.input_c_scale);
    if (!p.convert_matrix) return value;
    // Nonlinear R'G'B': matrix conversion only, no gamut or transfer conversion.
    float r = value.x + 2.0f * (1.0f - p.input_kr) * value.z;
    float b = value.x + 2.0f * (1.0f - p.input_kb) * value.y;
    float g = (value.x - p.input_kr * r - p.input_kb * b) / (1.0f - p.input_kr - p.input_kb);
    float luma = p.output_kr * r + (1.0f - p.output_kr - p.output_kb) * g + p.output_kb * b;
    return float3(luma, (b - luma) / (2.0f * (1.0f - p.output_kb)), (r - luma) / (2.0f * (1.0f - p.output_kr)));
}
kernel void filter_frame(texture2d<float, access::read> input_y [[texture(0)]],
                         texture2d<float, access::read> input_uv [[texture(1)]],
                         texture2d<float, access::write> output_y [[texture(2)]],
                         texture2d<float, access::write> output_uv [[texture(3)]],
                         constant Parameters& p [[buffer(0)]], uint2 index [[thread_position_in_grid]]) {
    if (index.x >= output_y.get_width() || index.y >= output_y.get_height()) return;
    float2 scale = float2(p.crop_width, p.crop_height) / float2(p.output_width, p.output_height);
    float2 start(p.crop_x, p.crop_y);
    float yy = sample_yuv(input_y, input_uv, start + (float2(index) + 0.5f) * scale, p).x;
    yy = round(clamp(yy * p.output_y_scale + p.output_y_offset, 0.0f, p.output_max)) * p.output_storage;
    output_y.write(float4(yy), index);
    if (index.x < output_uv.get_width() && index.y < output_uv.get_height()) {
        float2 cc = sample_yuv(input_y, input_uv,
            start + (float2(index) * 2.0f + float2(0.5f, 1.0f)) * scale, p).yz;
        cc = round(clamp(cc * p.output_c_scale + p.output_c_offset, 0.0f, p.output_max)) * p.output_storage;
        output_uv.write(float4(cc, 0.0f, 1.0f), index);
    }
}
