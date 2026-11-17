#include "vtenc/command_line.hpp"
#include "vtenc/status.hpp"
#include "yadif.hpp"
#include <array>
#include <iostream>
#include <limits>
using namespace vtenc;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() try {
    for (const auto mode : {"auto", "tff", "bff", "bob", "bob_tff", "bob_bff"}) {
        const std::string param = std::string("mode=") + mode;
        const std::array<std::string_view,7> args{"-i","input.mkv","-o","output.mov","--vpp-yadif",param,"--avsw"};
        const auto c = parse_command_line(args).encode;
        const std::string_view m(mode);
        require(c.filter.yadif.enabled && c.filter.yadif.bob == m.starts_with("bob"), "Wrong YADIF rate");
        require(c.filter.field_order == (m.ends_with("tff") ? FieldOrder::tff : m.ends_with("bff") ? FieldOrder::bff : FieldOrder::automatic), "Wrong YADIF order");
        require(configuration_json(c,true).find("\"yadif\":{\"enabled\":true,\"bob\":" + std::string(c.filter.yadif.bob ? "true":"false")) != std::string::npos,"YADIF configuration missing from JSON");
        require(!c.filter.bob && !c.filter.ivtc.enabled, "YADIF enabled another temporal filter");
    }
    for (bool reversed : {false,true}) {
        std::array<std::string_view,8> args{"-i","input","-o","output.mov","--vpp-yadif","mode=bob","--field-order","bff"};
        if (reversed) { std::swap(args[4],args[6]); std::swap(args[5],args[7]); }
        require(parse_command_line(args).encode.filter.field_order == FieldOrder::bff,"Order override depends on option order");
    }
    require(yadif_output_timescale(30000,false)==360000 && yadif_output_timescale(30000,true)==720000,"RFF timescale incorrect");
    for (int scale : {0,-1,std::numeric_limits<int>::max()}) {
        bool rejected=false;
        try { (void)yadif_output_timescale(scale,true); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Invalid YADIF output clock accepted");
    }
    std::cout << "YADIF configuration and timescale checks passed\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
