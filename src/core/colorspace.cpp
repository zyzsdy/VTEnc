// Algorithm adapted from NVEncFilterColorspace.cpp by rigaya (MIT), including
// RGB/XYZ construction and Bradford adaptation. See THIRD_PARTY_NOTICES.md.
#include "vtenc/colorspace.hpp"

#include <locale>
#include <sstream>
#include <cmath>
#include <set>
#include <stdexcept>

namespace vtenc {
namespace {
using Vec = std::array<double, 3>;
using Mat = std::array<Vec, 3>;
constexpr Mat identity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
Vec mul(const Mat& a, const Vec& b) {
    Vec out{};
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) out[r] += a[r][c] * b[c];
    return out;
}
Mat mul(const Mat& a, const Mat& b) {
    Mat out{};
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c)
        for (unsigned k = 0; k < 3; ++k) out[r][c] += a[r][k] * b[k][c];
    return out;
}
Mat inverse(Mat a) {
    Mat out = identity;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 3; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (std::abs(a[pivot][c]) < 1e-12) throw std::runtime_error("Singular colorspace matrix");
        std::swap(a[c], a[pivot]); std::swap(out[c], out[pivot]);
        const auto scale = a[c][c];
        for (unsigned k = 0; k < 3; ++k) { a[c][k] /= scale; out[c][k] /= scale; }
        for (unsigned r = 0; r < 3; ++r) if (r != c) {
            const auto f = a[r][c];
            for (unsigned k = 0; k < 3; ++k) { a[r][k] -= f * a[c][k]; out[r][k] -= f * out[c][k]; }
        }
    }
    return out;
}
Vec xyz(double x, double y) { return {x / y, 1, (1 - x - y) / y}; }
Vec white(ColorPrimaries p) { return p == ColorPrimaries::dci_p3 ? xyz(0.314, 0.351) : xyz(0.3127, 0.3290); }
Mat rgb_xyz(ColorPrimaries p) {
    std::array<double, 6> xy;
    switch (p) {
    case ColorPrimaries::bt709: xy = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060}; break;
    case ColorPrimaries::bt470bg: xy = {0.640, 0.330, 0.290, 0.600, 0.150, 0.060}; break;
    case ColorPrimaries::smpte170m: xy = {0.630, 0.340, 0.310, 0.595, 0.155, 0.070}; break;
    case ColorPrimaries::bt2020: xy = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046}; break;
    case ColorPrimaries::dci_p3: case ColorPrimaries::display_p3: xy = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060}; break;
    default: throw std::runtime_error("Missing or unsupported color primaries for gamut conversion");
    }
    Mat m{};
    for (unsigned c = 0; c < 3; ++c) {
        const auto v = xyz(xy[c * 2], xy[c * 2 + 1]);
        for (unsigned r = 0; r < 3; ++r) m[r][c] = v[r];
    }
    const auto scale = mul(inverse(m), white(p));
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) m[r][c] *= scale[c];
    return m;
}
[[noreturn]] void invalid(std::string_view key, std::string_view value) {
    throw std::invalid_argument("Unsupported --vpp-colorspace " + std::string(key) + "=" + std::string(value));
}
ColorMatrix matrix(std::string_view s) {
    if (s == "auto") return ColorMatrix::automatic;
    if (s == "bt601" || s == "smpte170m" || s == "bt470bg") return ColorMatrix::bt601;
    if (s == "bt709") return ColorMatrix::bt709;
    if (s == "smpte240m") return ColorMatrix::smpte240m;
    if (s == "bt2020nc" || s == "bt2020") return ColorMatrix::bt2020nc;
    invalid("matrix", s);
}
ColorTransfer transfer(std::string_view s) {
    if (s == "auto") return ColorTransfer::automatic;
    if (s == "bt709" || s == "smpte170m" || s == "bt2020-10" || s == "bt2020-12") return ColorTransfer::bt709;
    if (s == "linear") return ColorTransfer::linear;
    if (s == "srgb" || s == "iec61966-2-1") return ColorTransfer::srgb;
    if (s == "smpte2084" || s == "pq") return ColorTransfer::pq;
    if (s == "arib-std-b67" || s == "hlg") return ColorTransfer::hlg;
    invalid("transfer", s);
}
ColorPrimaries primaries(std::string_view s) {
    if (s == "auto") return ColorPrimaries::automatic;
    if (s == "bt709") return ColorPrimaries::bt709;
    if (s == "smpte170m" || s == "smpte240m") return ColorPrimaries::smpte170m;
    if (s == "bt470bg") return ColorPrimaries::bt470bg;
    if (s == "bt2020") return ColorPrimaries::bt2020;
    if (s == "smpte431" || s == "dci-p3") return ColorPrimaries::dci_p3;
    if (s == "smpte432" || s == "display-p3") return ColorPrimaries::display_p3;
    invalid("colorprim", s);
}
ColorRange range(std::string_view s) {
    if (s == "auto") return ColorRange::automatic;
    if (s == "limited") return ColorRange::limited;
    if (s == "full") return ColorRange::full;
    invalid("range", s);
}
float number(std::string_view key, std::string_view value, float low, float high) {
    float result = 0;
    std::istringstream parser{std::string(value)};
    parser.imbue(std::locale::classic()); parser >> std::noskipws >> result;
    if (!parser || parser.peek() != std::char_traits<char>::eof() || !std::isfinite(result) || result < low || result > high)
        throw std::invalid_argument("--vpp-colorspace " + std::string(key) + " requires a finite number in [" + std::to_string(low) + "," + std::to_string(high) + "]");
    return result;
}
template<class T, class Parse> void pair(std::string_view key, std::string_view value, T& from, T& to, Parse parse) {
    const auto colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == value.size())
        throw std::invalid_argument("--vpp-colorspace " + std::string(key) + " requires FROM:TO");
    from = parse(value.substr(0, colon)); to = parse(value.substr(colon + 1));
}
}

std::array<float, 2> matrix_coefficients(ColorMatrix matrix) {
    switch (matrix) {
    case ColorMatrix::bt601: return {0.299F, 0.114F};
    case ColorMatrix::bt709: return {0.2126F, 0.0722F};
    case ColorMatrix::smpte240m: return {0.212F, 0.087F};
    case ColorMatrix::bt2020nc: return {0.2627F, 0.0593F};
    default: throw std::runtime_error("Missing or unsupported YCbCr matrix for colorspace conversion");
    }
}
std::array<float, 3> primaries_luma(ColorPrimaries primaries) {
    const auto m = rgb_xyz(primaries);
    return {static_cast<float>(m[1][0]), static_cast<float>(m[1][1]), static_cast<float>(m[1][2])};
}
std::array<float, 9> gamut_matrix(ColorPrimaries from, ColorPrimaries to) {
    // Validate both endpoints even for an identity conversion.
    const auto a = rgb_xyz(from), b = rgb_xyz(to);
    Mat adaptation = identity;
    if (white(from) != white(to)) {
        constexpr Mat bradford{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
        const auto src = mul(bradford, white(from)), dst = mul(bradford, white(to));
        Mat scale{};
        for (unsigned i = 0; i < 3; ++i) scale[i][i] = dst[i] / src[i];
        adaptation = mul(mul(inverse(bradford), scale), bradford);
    }
    const auto result = from == to ? identity : mul(mul(inverse(b), adaptation), a);
    std::array<float, 9> out{};
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c) out[r * 3 + c] = static_cast<float>(result[r][c]);
    return out;
}

void validate_tonemap(const ToneMapConfig& t) {
    if (t.mode == ToneMap::none) return;
    const auto range = [](float x, float low, float high) { return std::isfinite(x) && x >= low && x <= high; };
    if (!range(t.source_peak,1,10000) || !range(t.ldr_nits,1,10000) || t.source_peak < t.ldr_nits)
        throw std::invalid_argument("hdr2sdr requires 1 <= ldr_nits <= source_peak <= 10000 cd/m2");
    if (!range(t.desat_base,0,1) || !range(t.desat_strength,0,1) || !range(t.desat_exp,0.01F,10))
        throw std::invalid_argument("Invalid hdr2sdr desaturation parameters");
    switch (t.mode) {
    case ToneMap::hable: {
        const auto& h = t.hable;
        if (!range(h[0],0,10) || !range(h[1],0.0001F,10) || !range(h[2],0,1) ||
            !range(h[3],0.0001F,10) || !range(h[4],0,10) || !range(h[5],0.0001F,10) ||
            h[4] >= h[5] || h[2] < h[4]/h[5])
            throw std::invalid_argument("Invalid Hable coefficients: require e < f and e/f <= c <= 1, with b,d,f > 0");
        const double x = static_cast<double>(t.source_peak)/t.ldr_nits, ef = static_cast<double>(h[4])/h[5];
        const auto white = x*(h[0]*(1-ef)*x+h[1]*(h[2]-ef))/(x*(h[0]*x+h[1])+h[3]*h[5]);
        if (white < 1e-8) throw std::invalid_argument("Degenerate Hable curve: source peak maps to zero");
        break;
    }
    case ToneMap::mobius:
        if (!range(t.transition,0,0.999F) || !range(t.peak,0.001F,100) ||
            static_cast<double>(t.peak)*t.source_peak/t.ldr_nits < 1)
            throw std::invalid_argument("Mobius requires transition in [0,0.999] and peak*source_peak/ldr_nits >= 1");
        break;
    case ToneMap::reinhard:
        if (!range(t.contrast,0.001F,0.999F) || !range(t.peak,0.001F,100))
            throw std::invalid_argument("Reinhard requires contrast in [0.001,0.999] and peak in [0.001,100]");
        break;
    case ToneMap::bt2390: break;
    default: throw std::invalid_argument("Unknown tone mapping mode");
    }
}

void parse_colorspace(std::string_view value, FilterConfig& f) {
    auto& c = f.colorspace;
    c.enabled = true; f.requested = true;
    std::set<std::string_view> seen;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto item = value.substr(0, comma), equal = item.substr(0, item.find('='));
        if (item.find('=') == std::string_view::npos || equal.empty()) throw std::invalid_argument("--vpp-colorspace requires KEY=VALUE entries");
        const auto arg = item.substr(equal.size() + 1);
        if (!seen.insert(equal).second) throw std::invalid_argument("Duplicate --vpp-colorspace parameter: " + std::string(equal));
        if (equal == "matrix") pair(equal, arg, f.input_matrix, f.output_matrix, matrix);
        else if (equal == "transfer") pair(equal, arg, c.input_transfer, c.output_transfer, transfer);
        else if (equal == "colorprim") pair(equal, arg, c.input_primaries, c.output_primaries, primaries);
        else if (equal == "range") pair(equal, arg, f.input_range, f.range, range);
        else if (equal == "scene_ref") {
            if (arg != "true" && arg != "false") invalid(equal, arg);
            c.scene_ref = arg == "true";
        } else if (equal == "hdr2sdr") {
            if (arg == "none") c.tonemap.mode = ToneMap::none;
            else if (arg == "hable") c.tonemap.mode = ToneMap::hable;
            else if (arg == "mobius") c.tonemap.mode = ToneMap::mobius;
            else if (arg == "reinhard") c.tonemap.mode = ToneMap::reinhard;
            else if (arg == "bt2390") c.tonemap.mode = ToneMap::bt2390;
            else invalid(equal,arg);
        } else if (equal == "sdr_source_peak") c.sdr_source_peak = number(equal,arg,0.01F,10000);
        else if (equal == "source_peak") c.tonemap.source_peak = number(equal,arg,1,10000);
        else if (equal == "ldr_nits") c.tonemap.ldr_nits = number(equal,arg,1,10000);
        else if (equal == "desat_base") c.tonemap.desat_base = number(equal,arg,0,1);
        else if (equal == "desat_strength") c.tonemap.desat_strength = number(equal,arg,0,1);
        else if (equal == "desat_exp") c.tonemap.desat_exp = number(equal,arg,0.01F,10);
        else if (equal == "transition") c.tonemap.transition = number(equal,arg,0,0.999F);
        else if (equal == "peak") c.tonemap.peak = number(equal,arg,0.001F,100);
        else if (equal == "contrast") c.tonemap.contrast = number(equal,arg,0.001F,0.999F);
        else if (equal.size() == 1 && equal[0] >= 'a' && equal[0] <= 'f')
            c.tonemap.hable[static_cast<std::size_t>(equal[0]-'a')] = number(equal,arg,0,10);
        else invalid(equal, arg);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
        if (value.empty()) throw std::invalid_argument("Trailing comma in --vpp-colorspace");
    }
    const auto mode = c.tonemap.mode;
    for (const auto key : {"source_peak", "ldr_nits", "desat_base", "desat_strength", "desat_exp", "a", "b", "c", "d", "e", "f", "transition", "peak", "contrast"}) {
        if (!seen.contains(key)) continue;
        if (mode == ToneMap::none) throw std::invalid_argument(std::string(key) + " requires enabled hdr2sdr");
        const std::string_view name(key);
        if (name.size() == 1 && mode != ToneMap::hable) throw std::invalid_argument(std::string(key) + " requires hdr2sdr=hable");
        if (name == "transition" && mode != ToneMap::mobius) throw std::invalid_argument("transition requires hdr2sdr=mobius");
        if (name == "contrast" && mode != ToneMap::reinhard) throw std::invalid_argument("contrast requires hdr2sdr=reinhard");
        if (name == "peak" && mode != ToneMap::mobius && mode != ToneMap::reinhard)
            throw std::invalid_argument("peak requires hdr2sdr=mobius or reinhard");
    }
    if (mode != ToneMap::none) {
        if (c.scene_ref) throw std::invalid_argument("hdr2sdr requires scene_ref=false (display luminance)");
        if (seen.contains("sdr_source_peak") && c.sdr_source_peak != c.tonemap.ldr_nits)
            throw std::invalid_argument("With hdr2sdr, sdr_source_peak must equal ldr_nits; prefer ldr_nits alone");
        if (c.input_transfer != ColorTransfer::automatic && c.input_transfer != ColorTransfer::pq && c.input_transfer != ColorTransfer::hlg)
            throw std::invalid_argument("hdr2sdr requires PQ or HLG input transfer");
        if (c.output_transfer == ColorTransfer::pq || c.output_transfer == ColorTransfer::hlg)
            throw std::invalid_argument("hdr2sdr requires an SDR output transfer");
    }
    validate_tonemap(c.tonemap);
    if (seen.empty()) throw std::invalid_argument("Empty --vpp-colorspace");
}
} // namespace vtenc
