#include "hevc_cadence.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace vtenc;
namespace {
using Data=std::vector<std::uint8_t>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Writer {
    Data bits;
    void u(std::uint64_t value,unsigned n) { while(n--) bits.push_back((value>>n)&1); }
    void ue(unsigned value) { auto n=value+1; unsigned width=0; for(auto x=n;x;x>>=1) ++width; u(0,width-1); u(n,width); }
    Data finish() { u(1,1); while(bits.size()%8) u(0,1); Data data(bits.size()/8); for(std::size_t i=0;i<bits.size();++i) data[i/8]|=bits[i]<<(7-i%8); return data; }
};
Data nal(unsigned type,const Data& payload) {
    Data result{static_cast<std::uint8_t>(type<<1),1}; unsigned zeros=0;
    for(auto byte:payload) { if(zeros==2 && byte<=3) { result.push_back(3); zeros=0; } result.push_back(byte); zeros=byte==0 ? zeros+1:0; }
    return result;
}
// Syntax fixtures exercise the reader independently of any encoder's SPS shape.
Data sps(unsigned id=0,bool info=true,bool complex=false,bool fields=false,bool vui=true,bool mixed=false) {
    Writer w; w.u(0,4); w.u(complex ? 2:0,3); w.u(1,1);
    w.u(1,8); w.u(0,32); w.u(1,1); w.u(mixed,1); w.u(0,46); w.u(120,8);
    if(complex) { w.u(3,2); w.u(1,2); w.u(0,12); w.u(0,64); w.u(0,24); w.u(90,8); w.u(90,8); }
    w.ue(id); w.ue(1); w.ue(192); w.ue(128); w.u(0,1); w.ue(0); w.ue(0); w.ue(4);
    w.u(0,1); w.ue(4); w.ue(2); w.ue(0);
    for(unsigned value:{0U,3U,0U,3U,0U,0U}) w.ue(value);
    w.u(complex,1);
    if(complex) {
        w.u(1,1);
        for(unsigned size=0;size<4;++size) for(unsigned matrix=0;matrix<6;matrix+=size==3 ? 3:1) {
            w.u(matrix==0,1);
            if(matrix) w.ue(0);
            else { if(size>1) w.ue(0); for(unsigned i=0;i<(size==0 ? 16U:64U);++i) w.ue(0); }
        }
    }
    w.u(3,2); w.u(complex,1);
    if(complex) { w.u(0x77,8); w.ue(0); w.ue(0); w.u(0,1); }
    w.ue(complex ? 3:0);
    if(complex) {
        w.ue(1); w.ue(1); w.ue(0); w.u(1,1); w.ue(0); w.u(1,1); // [-1,+1]
        w.u(1,1); w.u(0,1); w.ue(0); w.u(7,3); // +1 => [1,2], zero excluded
        w.u(1,1); w.u(1,1); w.ue(0); w.u(7,3); // -1 => [-1,1]
    }
    w.u(complex,1); if(complex) { w.ue(2); w.u(1,9); w.u(6,9); }
    w.u(3,2); w.u(vui,1);
    if(vui) {
        w.u(complex,1); if(complex) { w.u(255,8); w.u(1,16); w.u(1,16); }
        w.u(complex,1); if(complex) w.u(0,1);
        w.u(complex,1); if(complex) { w.u(10,4); w.u(1,1); w.u(0x010101,24); }
        w.u(complex,1); if(complex) { w.ue(0); w.ue(0); }
        w.u(0,1); w.u(fields,1); w.u(info,1); w.u(0,3);
    }
    w.u(0,1); return nal(33,w.finish());
}
Data pps(unsigned id=0,unsigned sequence=0) { Writer w; w.ue(id); w.ue(sequence); return nal(34,w.finish()); }
Data slice(unsigned id=0,bool first=true) { Writer w; w.u(first,1); w.u(0,1); w.ue(id); return nal(19,w.finish()); }
Data sei(unsigned pic,unsigned scan=1,bool duplicate=false) { return nal(39,{1,1,static_cast<std::uint8_t>((pic<<4)|(scan<<2)|(duplicate ? 2:0)|1),128}); }
Data packet(std::initializer_list<Data> nals,unsigned length=0) {
    Data out;
    for(const auto& unit:nals) {
        if(length) for(unsigned i=length;i>0;--i) out.push_back(static_cast<std::uint8_t>(unit.size()>>((i-1)*8)));
        else out.insert(out.end(),{0,0,0,1});
        out.insert(out.end(),unit.begin(),unit.end());
    }
    return out;
}
Data hvcc(unsigned length,const Data& sequence=sps()) {
    Data out(23); out[0]=1; out[21]=static_cast<std::uint8_t>(length-1); out[22]=2;
    for(const auto& unit:{sequence,pps()}) {
        out.push_back((unit[0]>>1)&63); out.insert(out.end(),{0,1});
        out.push_back(static_cast<std::uint8_t>(unit.size()>>8)); out.push_back(static_cast<std::uint8_t>(unit.size()));
        out.insert(out.end(),unit.begin(),unit.end());
    }
    return out;
}
}
int main() try {
    int cases=0;
    for(unsigned length:{0U,1U,2U,3U,4U}) for(bool complex:{false,true}) {
        HevcCadenceParser parser(length ? hvcc(length,sps(0,true,complex)):packet({sps(0,true,complex),pps()}));
        for(unsigned pic:{0U,3U,4U,5U,6U,7U,8U}) {
            const auto result=parser.parse(packet({sei(pic),slice()},length));
            require(result.known,"HEVC cadence not known");
            require(result.repeat_fields==(pic==5 || pic==6 ? 1:pic==7 ? 2:pic==8 ? 4:0),"Wrong HEVC repeat fields");
            require(result.first==(pic==3 || pic==5 ? FieldOrder::tff:pic==4 || pic==6 ? FieldOrder::bff:FieldOrder::automatic),"Wrong HEVC first field");
            ++cases;
        }
    }
    for(unsigned length:{0U,1U,2U,3U,4U}) for(bool complex:{false,true}) {
        const auto sequence=sps(0,true,complex,true);
        HevcCadenceParser fields(length ? hvcc(length,sequence):packet({sequence,pps()}));
        for(unsigned pic:{1U,2U,9U,10U,11U,12U}) for(bool duplicate:{false,true}) {
            const auto value=fields.parse(packet({sei(pic,2,duplicate),slice()},length));
            require(value.known && value.single_field && value.interlaced,"Missing HEVC field picture");
            require(value.first==((pic & 1) ? FieldOrder::tff:FieldOrder::bff),"Wrong spatial field parity");
            require(value.pairing==(pic>=11 ? FieldPairing::next:pic>=9 ? FieldPairing::previous:FieldPairing::unspecified),"Wrong field pairing hint");
            require(value.repeat_fields==0 && value.duplicate==duplicate,"Coded field duplicate expanded as RFF"); ++cases;
        }
    }
    auto check_error=[&](const Data& extra,const Data& access) {
        bool failed=false;
        try { HevcCadenceParser parser(extra); (void)parser.parse(access); } catch(const std::runtime_error&) { failed=true; }
        require(failed,"Invalid HEVC metadata was accepted"); ++cases;
    };
    const auto extra=packet({sps(),pps()});
    for(unsigned pic:{1U,2U,9U,10U,11U,12U,13U,14U,15U}) check_error(extra,packet({sei(pic),slice()}));
    for(const auto& access:{packet({slice()}),packet({sei(5,3),slice()}),packet({sei(5),sei(5),slice()}),
        packet({nal(39,{1,0,128}),slice()}),packet({nal(39,{1,2,0x55,128}),slice()}),packet({nal(39,{1,1,0x55}),slice()}),
        packet({nal(40,{1,1,0x55,128}),slice()}),packet({slice(),sei(5)}),packet({sei(5),slice(),slice()}),
        packet({sei(5),slice(1)}),packet({sei(5),slice(0,false)}),packet({sei(5)}),packet({Data{66,0,128}})}) check_error(extra,access);
    check_error(packet({sps(0,true,false,true),pps()}),packet({sei(5),slice()}));
    check_error(packet({sps(0,false,false,false,true,true),pps()}),packet({sei(5),slice()}));
    for(std::size_t n:{0U,1U,2U,10U,22U,24U}) { auto config=hvcc(4); config.resize(n); check_error(config,packet({sei(5),slice()},4)); }
    auto truncated=sps(); truncated.resize(8); check_error(packet({truncated,pps()}),packet({sei(5),slice()}));
    auto layered=sps(); layered[1]|=8; check_error(packet({layered,pps()}),packet({sei(5),slice()}));
    auto bad_escape=sps(); bad_escape.insert(bad_escape.end(),{0,0,3,4}); check_error(packet({bad_escape,pps()}),packet({sei(5),slice()}));
    HevcCadenceParser parser(extra);
    require(parser.parse(packet({sei(0,1,true),slice()})).repeat_fields==0,"duplicate_flag was mistaken for RFF"); ++cases;
    require(parser.parse(packet({sei(0,0),slice()})).interlaced,"Interlaced source_scan_type ignored"); ++cases;
    // Parameter-set switching and no inherited SEI across access units.
    auto output=parser.parse(packet({sps(1,false),pps(1,1),nal(39,{1,2,0xff,0xff,128}),slice(1)}));
    require(output.known && !output.interlaced && output.repeat_fields==0,"HRD payload mistaken for frame-field syntax"); ++cases;
    output=parser.parse(packet({sps(0,false),slice()}));
    require(output.repeat_fields==0,"In-band SPS replacement retained old field_info"); ++cases;
    HevcCadenceParser inferred(packet({sps(0,true,false,false,false,true),pps()}));
    require(inferred.parse(packet({sei(6),slice()})).repeat_fields==1,"Absent VUI inference ignored"); ++cases;
    HevcCadenceParser progressive(packet({sps(0,false,false,false,false),pps()}));
    require(!progressive.parse(packet({slice()})).interlaced,"Progressive no-VUI SPS failed"); ++cases;
    require(!hevc_field_sequence(sps()) && hevc_field_sequence(sps(0,true,false,true)),"Coded/display fields confused"); ++cases;
    // Unknown SEI with extended type/size and emulation prevention precedes PT.
    Data payload{255,5,255,2}; payload.insert(payload.end(),257,0); payload.push_back(128);
    require(inferred.parse(packet({nal(39,payload),sei(5),slice(),slice(0,false)})).repeat_fields==1,"Extended SEI/multi-slice parsing failed"); ++cases;
    std::cout<<"HEVC cadence: "<<cases<<" syntax, framing and error cases passed\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
