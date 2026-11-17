// Independent metadata reader for ITU-T H.265, 7.3.2.2, 7.3.7, Annex D.2.3
// and Annex E. The decoder remains responsible for slice/HRD conformance.
// Specification: https://www.itu.int/rec/T-REC-H.265
#include "hevc_cadence.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vtenc {
namespace {
using Bytes = std::span<const std::uint8_t>;
[[noreturn]] void invalid(const char* reason) { throw std::runtime_error(std::string("HEVC cadence: ") + reason); }
class Bits {
    Bytes data_;
    std::size_t bit_ = 0;
public:
    explicit Bits(Bytes data) : data_(data) {}
    unsigned read(unsigned n) {
        if (n > 32 || n > data_.size()*8-bit_) invalid("truncated bit syntax");
        unsigned value = 0;
        while (n--) { value = (value << 1) | ((data_[bit_/8] >> (7-bit_%8)) & 1U); ++bit_; }
        return value;
    }
    void skip(std::size_t n) {
        if (n > data_.size()*8-bit_) invalid("truncated bit syntax");
        bit_ += n;
    }
    unsigned ue(unsigned maximum = std::numeric_limits<unsigned>::max()-1) {
        unsigned zeros = 0;
        while (!read(1)) if (++zeros > 31) invalid("Exp-Golomb overflow");
        const auto value = ((std::uint64_t{1} << zeros)-1) + read(zeros);
        if (value > maximum) invalid("Exp-Golomb value outside syntax range");
        return static_cast<unsigned>(value);
    }
};
unsigned nal_type(Bytes nal) {
    if (nal.size() < 2 || (nal[0] & 128) || !(nal[1] & 7)) invalid("invalid NAL header");
    if ((nal[0] & 1) || (nal[1] & 248)) invalid("multilayer pictures are unsupported");
    return (nal[0] >> 1) & 63;
}
std::vector<std::uint8_t> rbsp(Bytes nal, std::size_t limit = std::numeric_limits<std::size_t>::max()) {
    (void)nal_type(nal);
    std::vector<std::uint8_t> result;
    unsigned zeros = 0;
    for (std::size_t i=2;i<nal.size() && result.size()<limit;++i) {
        const auto byte = nal[i];
        if (zeros == 2 && byte == 3) {
            if (i+1 == nal.size() || nal[i+1] > 3) invalid("invalid emulation-prevention byte");
            zeros = 0; continue;
        }
        if (zeros == 2 && byte < 3) invalid("unescaped RBSP byte sequence");
        result.push_back(byte); zeros = byte == 0 ? zeros+1 : 0;
    }
    return result;
}
std::vector<Bytes> units(Bytes bytes, unsigned length_size) {
    std::vector<Bytes> result;
    if (length_size) {
        while (!bytes.empty()) {
            if (bytes.size() < length_size) invalid("truncated NAL length");
            std::size_t n = 0;
            for (unsigned i=0;i<length_size;++i) n = (n<<8) | bytes[i];
            bytes = bytes.subspan(length_size);
            if (!n || n > bytes.size()) invalid("invalid NAL length");
            result.push_back(bytes.first(n)); bytes = bytes.subspan(n);
        }
    } else {
        const auto start = [&](std::size_t from) {
            for (auto i=from;i+2<bytes.size();++i)
                if (bytes[i]==0 && bytes[i+1]==0 && bytes[i+2]==1) return i;
            return bytes.size();
        };
        auto pos = start(0);
        if (pos == bytes.size()) invalid("missing Annex B start code");
        for (std::size_t i=0;i<pos;++i) if (bytes[i]) invalid("invalid Annex B prefix");
        while (pos < bytes.size()) {
            const auto begin = pos+3; pos = start(begin); auto end = pos;
            while (end > begin && bytes[end-1]==0) --end;
            if (begin == end) invalid("empty Annex B NAL");
            result.push_back(bytes.subspan(begin,end-begin));
        }
    }
    return result;
}
struct Sps {
    unsigned id = 0;
    bool progressive = false, interlaced = false, field_sequence = false, field_info = false;
};
Sps read_sps(Bytes nal) {
    if (nal_type(nal) != 33) invalid("expected SPS NAL");
    const auto data = rbsp(nal); Bits b(data); Sps s;
    b.skip(4); const auto layers = b.read(3);
    if (layers > 6) invalid("invalid temporal sub-layer count");
    b.skip(1);
    b.skip(40); s.progressive = b.read(1); s.interlaced = b.read(1); b.skip(46+8);
    std::array<bool,7> profile{}, level{};
    for (unsigned i=0;i<layers;++i) { profile[i]=b.read(1); level[i]=b.read(1); }
    if (layers) b.skip(2*(8-layers));
    for (unsigned i=0;i<layers;++i) { if (profile[i]) b.skip(88); if (level[i]) b.skip(8); }
    s.id=b.ue(15);
    if (b.ue(3)==3) b.skip(1); // separate_colour_plane_flag
    (void)b.ue(); (void)b.ue(); // coded width/height
    if (b.read(1)) for (int i=0;i<4;++i) (void)b.ue();
    (void)b.ue(8); (void)b.ue(8);
    const auto poc_bits=b.ue(12)+4;
    const bool all_ordering=b.read(1);
    for (unsigned i=all_ordering ? 0:layers;i<=layers;++i) { (void)b.ue(15); (void)b.ue(15); (void)b.ue(); }
    for (int i=0;i<6;++i) (void)b.ue(); // block sizes and transform hierarchy
    if (b.read(1) && b.read(1)) {
        for (unsigned size=0;size<4;++size) for (unsigned matrix=0;matrix<6;matrix+=size==3 ? 3:1) {
            if (!b.read(1)) (void)b.ue(size==3 ? matrix/3:matrix);
            else {
                if (size>1) (void)b.ue(); // signed DC coefficient, value not needed
                for (unsigned i=0;i<(size==0 ? 16U:64U);++i) (void)b.ue();
            }
        }
    }
    b.skip(2); // AMP, SAO
    if (b.read(1)) { b.skip(8); (void)b.ue(); (void)b.ue(); b.skip(1); }
    const auto sets=b.ue(64);
    std::vector<std::int64_t> previous;
    for (unsigned set=0;set<sets;++set) {
        std::vector<std::int64_t> next;
        if (set && b.read(1)) {
            const bool negative=b.read(1);
            const auto delta=(static_cast<std::int64_t>(b.ue())+1)*(negative ? -1:1);
            for (std::size_t j=0;j<=previous.size();++j) {
                const bool used=b.read(1);
                const bool keep=used || b.read(1);
                const auto poc=delta+(j<previous.size() ? previous[j]:0);
                if (keep && poc) next.push_back(poc);
            }
            std::sort(next.begin(),next.end(),[](auto a,auto c) {
                if ((a<0)!=(c<0)) return a<0;
                return a<0 ? a>c:a<c;
            });
        } else {
            const auto negative=b.ue(16), positive=b.ue(16);
            if (negative+positive>16) invalid("too many reference pictures");
            std::int64_t poc=0;
            for (unsigned i=0;i<negative;++i) { poc-=static_cast<std::int64_t>(b.ue())+1; b.skip(1); next.push_back(poc); }
            poc=0;
            for (unsigned i=0;i<positive;++i) { poc+=static_cast<std::int64_t>(b.ue())+1; b.skip(1); next.push_back(poc); }
        }
        if (next.size()>16) invalid("too many predicted reference pictures");
        previous=std::move(next);
    }
    if (b.read(1)) { const auto count=b.ue(32); b.skip(count*(poc_bits+1)); }
    b.skip(2); // temporal MVP, strong intra smoothing
    s.field_info=s.progressive && s.interlaced; // Annex E inference without VUI
    if (b.read(1)) {
        if (b.read(1) && b.read(8)==255) b.skip(32);
        if (b.read(1)) b.skip(1);
        if (b.read(1)) { b.skip(4); if (b.read(1)) b.skip(24); }
        if (b.read(1)) { (void)b.ue(5); (void)b.ue(5); }
        b.skip(1); s.field_sequence=b.read(1); s.field_info=b.read(1);
        if ((s.field_sequence || (s.progressive && s.interlaced)) && !s.field_info)
            invalid("SPS omits required frame-field information");
    }
    // Cadence precedes HRD timing in both VUI and picture_timing. No HRD delay
    // is interpreted as pic_struct, and container timestamps remain authoritative.
    return s;
}
FrameCadence timing(const Sps& s, const std::optional<std::vector<std::uint8_t>>& payload) {
    if (s.field_sequence) invalid("separately coded fields are unsupported; IVTC requires complete frame rasters");
    FrameCadence result; result.known=true;
    result.interlaced=s.interlaced && !s.progressive;
    if (!s.field_info) return result;
    if (!payload) invalid("SPS requires picture-timing SEI for every picture");
    Bits b(*payload);
    const auto pic=b.read(4), scan=b.read(2); (void)b.read(1); // duplicate_flag is not repeat_first_field
    if (scan==3) invalid("reserved source_scan_type");
    result.interlaced=scan==0;
    switch (pic) {
    case 0: break;
    case 3: case 4: case 5: case 6:
        // Even a progressive source is displayed as fields when pic_struct
        // specifies a field sequence. Keep its first-field order for hard IVTC.
        result.interlaced=true;
        result.first=(pic==4 || pic==6) ? FieldOrder::bff:FieldOrder::tff;
        result.repeat_fields=pic>=5 ? 1:0; break;
    case 7: result.repeat_fields=2; break;
    case 8: result.repeat_fields=4; break;
    case 1: case 2: case 9: case 10: case 11: case 12:
        invalid("single-field pic_struct is unsupported; IVTC requires complete frame rasters");
    default: invalid("reserved pic_struct");
    }
    return result;
}
}
struct HevcCadenceParser::Impl {
    unsigned length_size=0;
    std::array<std::optional<Sps>,16> sps;
    std::array<std::optional<unsigned>,64> pps;
    void parameter(Bytes nal) {
        const auto type=nal_type(nal);
        if (type==33) { const auto value=read_sps(nal); sps[value.id]=value; }
        if (type==34) {
            const auto data=rbsp(nal); Bits b(data);
            const auto id=b.ue(63), sequence=b.ue(15); pps[id]=sequence;
        }
    }
};
HevcCadenceParser::HevcCadenceParser(Bytes extra) : impl_(std::make_unique<Impl>()) {
    auto& s=*impl_;
    if (extra.empty()) return; // Annex B can supply parameter sets in-band.
    if (extra[0]!=1) { for (auto nal:units(extra,0)) s.parameter(nal); return; }
    if (extra.size()<23) invalid("truncated hvcC");
    s.length_size=(extra[21]&3)+1;
    const auto arrays=extra[22]; auto tail=extra.subspan(23);
    const auto u16=[&]() {
        if (tail.size()<2) invalid("truncated hvcC array");
        const unsigned value=(static_cast<unsigned>(tail[0])<<8)|tail[1]; tail=tail.subspan(2); return value;
    };
    for (unsigned array=0;array<arrays;++array) {
        if (tail.empty()) invalid("truncated hvcC array header");
        const auto type=tail[0]&63; tail=tail.subspan(1); const auto count=u16();
        for (unsigned i=0;i<count;++i) {
            const auto n=u16();
            if (!n || n>tail.size()) invalid("invalid hvcC NAL size");
            const auto nal=tail.first(n); tail=tail.subspan(n);
            if (nal_type(nal)!=type) invalid("hvcC array NAL type mismatch");
            s.parameter(nal);
        }
    }
    if (!tail.empty()) invalid("extra bytes after hvcC arrays");
}
HevcCadenceParser::~HevcCadenceParser() = default;
FrameCadence HevcCadenceParser::parse(Bytes packet) {
    auto& s=*impl_;
    std::optional<unsigned> picture_pps;
    std::optional<Sps> picture_sps;
    std::optional<std::vector<std::uint8_t>> payload;
    for (auto nal:units(packet,s.length_size)) {
        const auto type=nal_type(nal);
        if (type==33 || type==34) {
            if (picture_pps) invalid("parameter set after picture slices in access unit");
            s.parameter(nal);
        } else if (type<32) {
            // Only first_slice_segment_in_pic_flag and PPS ID are needed.
            // Do not copy/de-escape megabytes of coded slice pixels on the CPU.
            const auto data=rbsp(nal,16); Bits b(data);
            const bool first=b.read(1);
            if (type>=16 && type<=23) b.skip(1);
            const auto id=b.ue(63);
            if (first == picture_pps.has_value()) invalid("expected one complete picture per packet");
            if (!s.pps[id] || !s.sps[*s.pps[id]]) invalid("picture references unavailable PPS/SPS");
            if (picture_pps && *picture_pps!=id) invalid("PPS changed within picture");
            picture_pps=id; picture_sps=s.sps[*s.pps[id]];
        } else if (type==39 || type==40) {
            const auto data=rbsp(nal); std::size_t offset=0;
            if (data.empty()) invalid("empty SEI RBSP");
            const auto number=[&]() {
                std::size_t value=0; unsigned byte;
                do { if (offset>=data.size()) invalid("truncated SEI header"); byte=data[offset++]; value+=byte; } while (byte==255);
                return value;
            };
            while (offset<data.size()) {
                if (offset+1==data.size() && data[offset]==128) { ++offset; break; }
                const auto kind=number(), size=number();
                if (size>data.size()-offset) invalid("truncated SEI payload");
                if (kind==1) {
                    if (type!=39 || picture_pps) invalid("picture-timing SEI must precede picture slices");
                    if (payload) invalid("duplicate picture-timing SEI in access unit");
                    payload.emplace(data.begin()+static_cast<std::ptrdiff_t>(offset),data.begin()+static_cast<std::ptrdiff_t>(offset+size));
                }
                offset+=size;
                if (offset==data.size()) invalid("missing SEI RBSP trailing bits");
            }
        }
    }
    if (!picture_sps) invalid("packet does not contain a complete picture");
    return timing(*picture_sps,payload);
}
bool hevc_field_sequence(Bytes nal) { return read_sps(nal).field_sequence; }
}
