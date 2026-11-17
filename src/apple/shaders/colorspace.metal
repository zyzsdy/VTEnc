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

// NVEnc HDR2SDR curves and highlight desaturation (rigaya, MIT).
// See THIRD_PARTY_NOTICES.md. Keep layout synchronized with ToneMapConfig.
struct ToneMapParameters {
    uint mode;
    float source_peak, ldr_nits;
    float desat_base, desat_strength, desat_exp;
    float hable[6];
    float transition, peak, contrast;
};
float hable_curve(float x, constant ToneMapParameters& t) {
    float a=t.hable[0], b=t.hable[1], c=t.hable[2], d=t.hable[3], e=t.hable[4], f=t.hable[5];
    // Algebraically identical to NVEnc's rational expression minus e/f,
    // without cancellation at black. Host rejects nonmonotonic/zero curves.
    return x * (a * (1.0f-e/f) * x + b * (c-e/f)) / (x*(a*x+b)+d*f);
}
float bt2390_pq_decode(float x) {
    if (x <= 0.0f) return 0.0f;
    // NVEnc's BT.2390 helper uses a 203 cd/m2 reference-white multiplier;
    // its PQ values can exceed 1. Do not use the input-signal-clamping decoder.
    float v=pow(x,1.0f/pq_m2);
    return pow(max(v-pq_c1,0.0f)/(pq_c2-pq_c3*v),1.0f/pq_m1)*10000.0f/203.0f;
}
float tone_channel(float x, constant ToneMapParameters& t) {
    float ratio=t.source_peak/t.ldr_nits;
    if (t.mode == 1) return hable_curve(x,t)/hable_curve(ratio,t);
    if (t.mode == 2) {
        float transition=t.transition;
        if (x <= transition) return x;
        float peak=t.peak*ratio, q=peak-1.0f, u=1.0f-transition, h=x-transition;
        float base=u*(peak-transition), d=u*u+q;
        // Reduced NVEnc Mobius rational form: avoids division by peak-1,
        // remains continuous (identity) at peak=1, and has no pole for x>t.
        return (transition*base+d*h)/(base+q*h);
    }
    if (t.mode == 3) {
        float offset=(1.0f-t.contrast)/t.contrast, peak=t.peak*ratio;
        return x/(x+offset)*(peak+offset)/peak;
    }
    if (t.mode == 4) {
        if (t.source_peak == t.ldr_nits) return x;
        float src=pq_encode(t.source_peak*203.0f/10000.0f);
        float dst=pq_encode(t.ldr_nits*203.0f/10000.0f)/src;
        if (dst >= 1.0f) return min(x,ratio); // Vanishing shoulder; retain the source-peak clamp.
        // Clamp at the declared source peak: Hermite extrapolation outside
        // [0,1] can reverse the shoulder and turn over-range highlights dark.
        float v=pq_encode(min(x*t.ldr_nits,t.source_peak)*203.0f/10000.0f)/src;
        float knee=1.5f*dst-0.5f;
        if (v >= knee) {
            float z=(v-knee)/(1.0f-knee), z2=z*z, z3=z2*z;
            v=(2.0f*z3-3.0f*z2+1.0f)*knee + (z3-2.0f*z2+z)*(1.0f-knee) + (-2.0f*z3+3.0f*z2)*dst;
        }
        return bt2390_pq_decode(v*src)/t.ldr_nits;
    }
    return x;
}
float3 tone_map(float3 x, constant ToneMapParameters& t) {
    if (!t.mode) return x;
    // PQ/HLG EOTFs produce nonnegative source-linear RGB; gamut conversion
    // follows this operation, so negative out-of-gamut values cannot hit poles.
    float3 y(tone_channel(x.r,t),tone_channel(x.g,t),tone_channel(x.b,t));
    float epsilon=t.mode == 4 ? 1e-6f/t.ldr_nits : 1e-6f;
    float in_max=max(max(x.r,x.g),max(x.b,epsilon));
    float out_max=max(max(y.r,y.g),max(y.b,epsilon));
    float coeff=max(out_max-t.desat_base,1e-6f)/max(out_max,1.0f);
    float amount=clamp(t.desat_strength*pow(coeff,t.desat_exp),0.0f,1.0f);
    return mix(x*(out_max/in_max),y,amount);
}
