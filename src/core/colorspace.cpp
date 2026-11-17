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
        } else if (equal == "sdr_source_peak") {
            float peak = 0;
            std::istringstream parser{std::string(arg)};
            parser.imbue(std::locale::classic());
            parser >> std::noskipws >> peak;
            if (!parser || parser.peek() != std::char_traits<char>::eof() || !std::isfinite(peak) || peak < 0.01F || peak > 10000)
                throw std::invalid_argument("--vpp-colorspace sdr_source_peak requires a finite number in [0.01,10000]");
            c.sdr_source_peak = peak;
        } else invalid(equal, arg);
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
        if (value.empty()) throw std::invalid_argument("Trailing comma in --vpp-colorspace");
    }
    if (seen.empty()) throw std::invalid_argument("Empty --vpp-colorspace");
}
} // namespace vtenc
