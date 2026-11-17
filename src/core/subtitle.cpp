#include "vtenc/subtitle.hpp"
#include "vtenc/colorspace.hpp"
#include <charconv>
#include <cmath>
#include <set>
#include <sstream>
#include <locale>
#include <stdexcept>

namespace vtenc {
namespace {
// Commas inside matching single/double quotes belong to filenames. No shell expansion.
std::vector<std::pair<std::string, std::string>> pairs(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::string> seen;
    while (!text.empty()) {
        std::size_t end = 0; char quote = 0;
        for (; end < text.size(); ++end) {
            const char c = text[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == ',') break;
        }
        if (quote) throw std::invalid_argument("Unclosed quote in subtitle/color options");
        auto item = text.substr(0, end);
        const auto eq = item.find('=');
        if (eq == std::string_view::npos || eq == 0) throw std::invalid_argument("Expected key=value in subtitle/color options");
        std::string key(item.substr(0, eq)); auto value = item.substr(eq + 1);
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if (value.empty() || !seen.insert(key).second) throw std::invalid_argument("Empty or duplicate parameter: " + key);
        out.emplace_back(key, value);
        if (end == text.size()) break;
        text.remove_prefix(end + 1);
        if (text.empty()) throw std::invalid_argument("Trailing comma in subtitle/color options");
    }
    if (out.empty()) throw std::invalid_argument("Empty subtitle/color options");
    return out;
}
float number(const std::string& s, float low, float high) {
    float v = 0; std::istringstream in(s); in.imbue(std::locale::classic()); in >> std::noskipws >> v;
    if (!in || !in.eof() || !std::isfinite(v) || v < low || v > high)
        throw std::invalid_argument("Invalid subtitle numeric value: " + s);
    return v;
}
std::int64_t milliseconds(std::string_view s) {
    bool negative = s.starts_with('-'); if (negative || s.starts_with('+')) s.remove_prefix(1);
    auto dot = s.find('.'); auto whole = s.substr(0, dot);
    std::int64_t seconds = 0;
    auto [end, ec] = std::from_chars(whole.data(), whole.data()+whole.size(), seconds);
    if (ec != std::errc{} || end != whole.data()+whole.size() || seconds < 0 || seconds > 2147483647)
        throw std::invalid_argument("ts_offset requires signed seconds with at most three decimal places");
    std::int64_t fraction = 0;
    if (dot != std::string_view::npos) {
        auto f = s.substr(dot+1);
        if (f.empty() || f.size()>3) throw std::invalid_argument("ts_offset precision is milliseconds");
        for (auto c : f) { if (c<'0'||c>'9') throw std::invalid_argument("Invalid ts_offset"); fraction=fraction*10+c-'0'; }
        for (auto n=f.size();n<3;++n) fraction*=10;
    }
    return (seconds*1000+fraction)*(negative?-1:1);
}
}
SubtitleConfig parse_subburn(std::string_view value) {
    SubtitleConfig c;
    for (const auto& [key, v] : pairs(value)) {
        if (key=="filename") c.filename=v;
        else if (key=="fontsdir") c.fontsdir=v;
        else if (key=="fontfile") c.fontfile=v;
        else if (key=="font") c.font=v;
        else if (key=="ts_offset") c.offset_ms=milliseconds(v);
        else if (key=="opacity") c.opacity=number(v,0,1);
        else if (key=="white") c.white_nits=number(v,1,1000);
        else if (key=="clip") {
            std::array<int,4> r{}; std::string_view rest=v;
            for (unsigned i=0;i<4;++i) {
                const auto colon=rest.find(':'); auto part=rest.substr(0,colon);
                auto [end,ec]=std::from_chars(part.data(),part.data()+part.size(),r[i]);
                if (ec!=std::errc{}||end!=part.data()+part.size()||r[i]<0||r[i]>32768||
                    ((i<3)!=(colon!=std::string_view::npos))||(i>=2&&r[i]==0))
                    throw std::invalid_argument("clip requires nonnegative X:Y and positive W:H, each <=32768");
                if (i<3) rest.remove_prefix(colon+1);
            }
            c.clip=r;
        } else throw std::invalid_argument("Unsupported --vpp-subburn parameter: "+key);
    }
    if (c.filename.empty()) throw std::invalid_argument("--vpp-subburn requires filename=ASS/SSA");
    return c;
}
void parse_color_tags(std::string_view value, ColorTags& tags) {
    for (const auto& [key,v] : pairs(value)) {
        if ((key!="matrix"&&key!="transfer"&&key!="colorprim")||v=="auto"||v.find(':')!=std::string::npos)
            throw std::invalid_argument("--color-tags accepts explicit matrix, transfer, colorprim values only");
        FilterConfig f; parse_colorspace(key+"=auto:"+v,f);
        if (key=="matrix") tags.matrix=f.output_matrix;
        if (key=="transfer") tags.transfer=f.colorspace.output_transfer;
        if (key=="colorprim") tags.primaries=f.colorspace.output_primaries;
    }
    tags.enabled=true;
}
}
