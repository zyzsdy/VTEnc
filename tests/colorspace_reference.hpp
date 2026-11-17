#pragma once
// Independent double-precision reference for the GPU integration test. Fixed
// RGB->XYZ matrices keep verification independent of the production planner.
#include "vtenc/command_line.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace color_reference {
using V = std::array<double, 3>;
using M = std::array<V, 3>;
inline V mul(const M& m, const V& v) {
    return {m[0][0]*v[0]+m[0][1]*v[1]+m[0][2]*v[2],
            m[1][0]*v[0]+m[1][1]*v[1]+m[1][2]*v[2],
            m[2][0]*v[0]+m[2][1]*v[1]+m[2][2]*v[2]};
}
inline M xyz(vtenc::ColorPrimaries p) {
    using enum vtenc::ColorPrimaries;
    if (p == bt709) return {{{.4123907993,.3575843394,.1804807884}, {.2126390059,.7151686788,.0721923154}, {.0193308187,.1191947798,.9505321522}}};
    if (p == bt2020) return {{{.6369580483,.1446169036,.1688809752}, {.2627002120,.6779980715,.0593017165}, {0,.0280726930,1.0609850577}}};
    if (p == display_p3) return {{{.4865709486,.2656676932,.1982172852}, {.2289745641,.6917385218,.0792869141}, {0,.0451133819,1.0439443689}}};
    throw std::runtime_error("Unsupported reference primaries");
}
inline M rgb(vtenc::ColorPrimaries p) {
    using enum vtenc::ColorPrimaries;
    if (p == bt709) return {{{3.2409699419,-1.5373831776,-.4986107603}, {-.9692436363,1.8759675015,.0415550574}, {.0556300797,-.2039769589,1.0569715142}}};
    if (p == bt2020) return {{{1.7166511880,-.3556707838,-.2533662814}, {-.6666843518,1.6164812366,.0157685458}, {.0176398574,-.0427706133,.9421031212}}};
    if (p == display_p3) return {{{2.4934969119,-.9313836179,-.4027107845}, {-.8294889696,1.7626640603,.0236246858}, {.0358458302,-.0761723893,.9568845240}}};
    throw std::runtime_error("Unsupported reference primaries");
}
inline std::array<double, 2> coefficients(vtenc::ColorMatrix m) {
    using enum vtenc::ColorMatrix;
    if (m == bt601) return {.299, .114};
    if (m == bt709) return {.2126, .0722};
    if (m == bt2020nc) return {.2627, .0593};
    if (m == smpte240m) return {.212, .087};
    throw std::runtime_error("Unknown test matrix");
}
inline double rec_decode(double x) {
    return x < .0812428582986315 ? x/4.5 : std::pow((x+.09929682680944)/1.09929682680944, 1/.45);
}
inline double rec_encode(double x) {
    return x < .018053968510807 ? x*4.5 : 1.09929682680944*std::pow(x,.45)-.09929682680944;
}
inline double decode(double x, vtenc::ColorTransfer t, bool scene) {
    using enum vtenc::ColorTransfer;
    switch (t) {
    case linear: return x;
    case bt709: return scene ? rec_decode(x) : std::pow(std::max(x,0.0),2.4);
    case srgb: return x < .039293370676847 ? x/12.92 : std::pow((x+.055010718947587)/1.055010718947587,2.4);
    case pq: {
        const auto n = std::pow(std::clamp(x,0.0,1.0), 32.0/2523.0);
        const auto v = std::pow(std::max(n-3424.0/4096,0.0)/(2413.0/128-2392.0/128*n),4096.0*4/2610);
        return scene ? rec_decode(std::pow(100*v,1/2.4))/59.49080238715383 : v;
    }
    case hlg:
        x = std::max(x, 0.0);
        return x <= .5 ? x*x/3 : (std::exp((x-.55991073)/.17883277)+.28466892)/12;
    default: throw std::runtime_error("Unknown test transfer");
    }
}
inline double encode(double x, vtenc::ColorTransfer t, bool scene) {
    using enum vtenc::ColorTransfer;
    switch (t) {
    case linear: return x;
    case bt709: return scene ? rec_encode(x) : std::pow(std::max(x,0.0),1/2.4);
    case srgb: return x < .003041282560128 ? x*12.92 : 1.055010718947587*std::pow(x,1/2.4)-.055010718947587;
    case pq: {
        if (scene) x = std::pow(std::max(rec_encode(x*59.49080238715383),0.0),2.4)/100;
        if (x <= 0) return 0;
        const auto n = std::pow(x,2610.0/(4096*4));
        return std::pow((3424.0/4096+2413.0/128*n)/(1+2392.0/128*n),2523.0/32);
    }
    case hlg:
        x = std::max(x,0.0);
        return x <= 1.0/12 ? std::sqrt(3*x) : .17883277*std::log(12*x-.28466892)+.55991073;
    default: throw std::runtime_error("Unknown test transfer");
    }
}
// Reference uses the original NVEnc rational forms in double precision;
// production Metal uses cancellation-resistant algebraic reductions.
inline double tone_channel(double x, const vtenc::ToneMapConfig& t) {
    using enum vtenc::ToneMap;
    const double ratio=static_cast<double>(t.source_peak)/t.ldr_nits;
    if (t.mode == hable) {
        const auto h = [&](double v) {
            const auto& p=t.hable;
            return (v*(p[0]*v+p[2]*p[1])+p[3]*p[4])/(v*(p[0]*v+p[1])+p[3]*p[5])-static_cast<double>(p[4])/p[5];
        };
        return x == 0 ? 0 : h(x)/h(ratio);
    }
    if (t.mode == mobius) {
        const double j=t.transition, peak=t.peak*ratio;
        if (x <= j || peak == 1) return x;
        const auto a=-j*j*(peak-1)/(j*j-2*j+peak);
        const auto b=(j*j-2*j*peak+peak)/(peak-1);
        return (b*b+2*b*j+j*j)/(b-a)*(x+a)/(x+b);
    }
    if (t.mode == reinhard) {
        const double offset=(1.0-t.contrast)/t.contrast, peak=t.peak*ratio;
        return x/(x+offset)*(peak+offset)/peak;
    }
    if (t.mode == bt2390) {
        if (t.source_peak == t.ldr_nits) return x;
        const auto pq = [](double v) { return encode(v*203/10000,vtenc::ColorTransfer::pq,false); };
        const auto src=pq(t.source_peak), max_lum=pq(t.ldr_nits)/src;
        auto v=pq(std::min(x*t.ldr_nits,static_cast<double>(t.source_peak)))/src;
        const auto knee=1.5*max_lum-.5;
        if (v >= knee) {
            const auto z=(v-knee)/(1-knee);
            v=(2*z*z*z-3*z*z+1)*knee+(z*z*z-2*z*z+z)*(1-knee)+(-2*z*z*z+3*z*z)*max_lum;
        }
        if (v <= 0) return 0;
        const auto n=std::pow(v*src,32.0/2523);
        return std::pow(std::max(n-3424.0/4096,0.0)/(2413.0/128-2392.0/128*n),4096.0*4/2610)*10000/203/t.ldr_nits;
    }
    return x;
}
inline V tonemap(V x, const vtenc::ToneMapConfig& t) {
    if (t.mode == vtenc::ToneMap::none) return x;
    V y{};
    for (unsigned i=0; i<3; ++i) y[i]=tone_channel(x[i],t);
    const double epsilon=t.mode == vtenc::ToneMap::bt2390 ? 1e-6/t.ldr_nits : 1e-6;
    const auto in_max=std::max({x[0],x[1],x[2],epsilon});
    const auto out_max=std::max({y[0],y[1],y[2],epsilon});
    const auto coeff=std::max(out_max-t.desat_base,1e-6)/std::max(out_max,1.0);
    const auto amount=std::clamp(t.desat_strength*std::pow(coeff,t.desat_exp),0.0,1.0);
    for (unsigned i=0; i<3; ++i) x[i]=std::lerp(x[i]*out_max/in_max,y[i],amount);
    return x;
}
inline V transform(V v, const vtenc::ColorspaceConfig& c) {
    using enum vtenc::ColorTransfer;
    const double linear_peak = c.tonemap.mode == vtenc::ToneMap::none ? c.sdr_source_peak : c.tonemap.ldr_nits;
    for (auto& x : v) x = decode(x,c.input_transfer,c.scene_ref);
    double scale = 1;
    if (c.input_transfer == pq) scale = 10000/linear_peak;
    if (c.input_transfer == hlg) {
        scale = c.scene_ref ? 12 : 1000/linear_peak;
        if (!c.scene_ref) scale *= std::pow(std::max(mul(xyz(c.input_primaries),v)[1],1.175494351e-38),.2);
    }
    for (auto& x : v) x *= scale;
    v = tonemap(v,c.tonemap);
    if (c.input_primaries != c.output_primaries) v = mul(rgb(c.output_primaries),mul(xyz(c.input_primaries),v));
    scale = 1;
    if (c.output_transfer == pq) scale = linear_peak/10000;
    if (c.output_transfer == hlg) {
        scale = c.scene_ref ? 1.0/12 : linear_peak/1000;
        if (!c.scene_ref) scale *= std::pow(std::max(mul(xyz(c.output_primaries),v)[1]*scale,1.175494351e-38),-1.0/6);
    }
    for (auto& x : v) x = encode(x*scale,c.output_transfer,c.scene_ref);
    return v;
}
inline bool nonlinear(const vtenc::ColorspaceConfig& c) {
    return c.enabled && (c.tonemap.mode != vtenc::ToneMap::none || c.input_transfer != c.output_transfer || c.input_primaries != c.output_primaries);
}
} // namespace color_reference
