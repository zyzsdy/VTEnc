#pragma once

#include "command_line.hpp"
#include <array>

namespace vtenc {
// Row-major RGB linear-light transform, including Bradford white adaptation.
[[nodiscard]] std::array<float, 9> gamut_matrix(ColorPrimaries from, ColorPrimaries to);
[[nodiscard]] std::array<float, 3> primaries_luma(ColorPrimaries primaries);
[[nodiscard]] std::array<float, 2> matrix_coefficients(ColorMatrix matrix);
void parse_colorspace(std::string_view value, FilterConfig& filter);
} // namespace vtenc
