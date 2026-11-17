// Metal port of NVEnc's transfer-function path (rigaya, MIT), whose gamma
// functions derive from zimg (WTFPL). See THIRD_PARTY_NOTICES.md.
#include <metal_stdlib>
using namespace metal;

constant float rec709_alpha = 1.09929682680944f;
constant float rec709_beta = 0.018053968510807f;
constant float srgb_alpha = 1.055010718947587f;
constant float srgb_beta = 0.003041282560128f;
constant float pq_m1 = 0.1593017578125f, pq_m2 = 78.84375f;
constant float pq_c1 = 0.8359375f, pq_c2 = 18.8515625f, pq_c3 = 18.6875f;

float rec709_decode(float x) {
    return x < 4.5f * rec709_beta ? x / 4.5f : pow((x + rec709_alpha - 1.0f) / rec709_alpha, 1.0f / 0.45f);
}
float rec709_encode(float x) {
    return x < rec709_beta ? x * 4.5f : rec709_alpha * pow(x, 0.45f) - (rec709_alpha - 1.0f);
}
float pq_decode(float x) {
    // Bound invalid PQ excursions before the rational function's singularity.
    if (x <= 0.0f) return 0.0f;
    float v = pow(min(x, 1.0f), 1.0f / pq_m2);
    return pow(max(v - pq_c1, 0.0f) / (pq_c2 - pq_c3 * v), 1.0f / pq_m1);
}
float pq_encode(float x) {
    if (x <= 0.0f) return 0.0f;
    float v = pow(x, pq_m1);
    // Stable arrangement from NVEnc/zimg; explicitly preserve exact black.
    return pow(1.0f + ((pq_c1 - 1.0f) + (pq_c2 - pq_c3) * v) / (1.0f + pq_c3 * v), pq_m2);
}
float hlg_decode(float x) {
    x = max(x, 0.0f);
    return x <= 0.5f ? x * x / 3.0f : (exp((x - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
}
float hlg_encode(float x) {
    x = max(x, 0.0f);
    return x <= 1.0f / 12.0f ? sqrt(3.0f * x) : 0.17883277f * log(12.0f * x - 0.28466892f) + 0.55991073f;
}
float decode_channel(float x, uint transfer, bool scene) {
    switch (transfer) {
    case 1: return scene ? rec709_decode(x) : pow(max(x, 0.0f), 2.4f);
    case 8: return x;
    case 13: return x < 12.92f * srgb_beta ? x / 12.92f : pow((x + srgb_alpha - 1.0f) / srgb_alpha, 2.4f);
    case 16: {
        float v = pq_decode(x);
        return scene ? rec709_decode(pow(v * 100.0f, 1.0f / 2.4f)) / 59.49080238715383f : v;
    }
    case 18: return hlg_decode(x);
    }
    return x; // Host validates all transfer identifiers before dispatch.
}
float encode_channel(float x, uint transfer, bool scene) {
    switch (transfer) {
    case 1: return scene ? rec709_encode(x) : pow(max(x, 0.0f), 1.0f / 2.4f);
    case 8: return x;
    case 13: return x < srgb_beta ? x * 12.92f : srgb_alpha * pow(x, 1.0f / 2.4f) - (srgb_alpha - 1.0f);
    case 16: return pq_encode(scene ? pow(max(rec709_encode(x * 59.49080238715383f), 0.0f), 2.4f) / 100.0f : x);
    case 18: return hlg_encode(x);
    }
    return x;
}
float3 to_linear(float3 rgb, uint transfer, bool scene, float peak, float3 luma) {
    float3 v(decode_channel(rgb.r, transfer, scene), decode_channel(rgb.g, transfer, scene), decode_channel(rgb.b, transfer, scene));
    if (transfer == 16) v *= 10000.0f / peak;
    if (transfer == 18) {
        // Rec.2100 OOTF uses scene-linear luminance, never encoded RGB.
        // This also makes the luminance-dependent forward/inverse paths reciprocal.
        if (!scene) v *= pow(max(dot(luma, v), 1.175494351e-38f), 0.2f);
        v *= scene ? 12.0f : 1000.0f / peak;
    }
    return v;
}
float3 from_linear(float3 v, uint transfer, bool scene, float peak, float3 luma) {
    if (transfer == 16) v *= peak / 10000.0f;
    if (transfer == 18) {
        v *= scene ? 1.0f / 12.0f : peak / 1000.0f;
        if (!scene) v *= pow(max(dot(luma, v), 1.175494351e-38f), -1.0f / 6.0f);
    }
    return float3(encode_channel(v.r, transfer, scene), encode_channel(v.g, transfer, scene), encode_channel(v.b, transfer, scene));
}
