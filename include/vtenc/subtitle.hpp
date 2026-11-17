#pragma once
#include "vtenc/command_line.hpp"
namespace vtenc {
SubtitleConfig parse_subburn(std::string_view value);
void parse_color_tags(std::string_view value, ColorTags& tags);
}
