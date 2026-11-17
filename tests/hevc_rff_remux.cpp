// Test-only remuxer: inject RFF into existing B-frame HEVC without video encoding.
#include "ffmpeg_utils.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <span>
#include <vector>
using namespace vtenc;
namespace {
using Data=std::vector<std::uint8_t>;
using Bytes=std::span<const std::uint8_t>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct IO {
    AVFormatContext* input=nullptr;
    AVFormatContext* output=nullptr;
    ~IO() { avformat_close_input(&input); if(output) { if(output->pb) avio_closep(&output->pb); avformat_free_context(output); } }
};
Data patch_sps(Bytes nal,std::size_t bit) {
    Data raw; unsigned zeros=0;
    for(auto byte:nal) { if(zeros==2 && byte==3) { zeros=0; continue; } raw.push_back(byte); zeros=byte==0 ? zeros+1:0; }
    require(bit<raw.size()*8,"SPS bit offset outside NAL"); raw[bit/8]|=static_cast<std::uint8_t>(1U<<(7-bit%8));
    Data result; zeros=0;
    for(auto byte:raw) { if(zeros==2 && byte<=3) { result.push_back(3); zeros=0; } result.push_back(byte); zeros=byte==0 ? zeros+1:0; }
    return result;
}
unsigned u16(Bytes& bytes) { require(bytes.size()>=2,"Truncated hvcC"); const unsigned value=(static_cast<unsigned>(bytes[0])<<8)|bytes[1]; bytes=bytes.subspan(2); return value; }
void append(Data& out,Bytes bytes,unsigned length) {
    for(unsigned i=length;i>0;--i) out.push_back(static_cast<std::uint8_t>(bytes.size()>>((i-1)*8)));
    out.insert(out.end(),bytes.begin(),bytes.end());
}
Data patch_config(Bytes extra,std::size_t bit) {
    require(extra.size()>=23 && extra[0]==1,"Expected hvcC fixture source"); Data out(extra.begin(),extra.begin()+23);
    const auto arrays=extra[22]; auto tail=extra.subspan(23);
    for(unsigned i=0;i<arrays;++i) {
        require(!tail.empty(),"Truncated hvcC header"); out.push_back(tail[0]); tail=tail.subspan(1);
        const auto count=u16(tail); out.push_back(static_cast<std::uint8_t>(count>>8)); out.push_back(static_cast<std::uint8_t>(count));
        for(unsigned j=0;j<count;++j) {
            const auto n=u16(tail); require(n<=tail.size(),"Truncated parameter set"); const auto nal=tail.first(n); tail=tail.subspan(n);
            if(((nal[0]>>1)&63)==33) append(out,patch_sps(nal,bit),2); else append(out,nal,2);
        }
    }
    require(tail.empty(),"Extra hvcC data"); return out;
}
}
int main(int argc,char** argv) try {
    require(argc==4,"Usage: vtenc_hevc_rff_remux input.mp4 output.mp4 SPS-field-info-bit");
    require(!std::filesystem::exists(argv[2]),"Output already exists");
    const auto bit=static_cast<std::size_t>(std::stoul(argv[3])); IO io;
    av_check(avformat_open_input(&io.input,argv[1],nullptr,nullptr),"Open B-frame fixture");
    av_check(avformat_find_stream_info(io.input,nullptr),"Read B-frame fixture");
    const int video=av_find_best_stream(io.input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0); av_check(video,"Find fixture video");
    const auto* codec=io.input->streams[video]->codecpar;
    require(codec->codec_id==AV_CODEC_ID_HEVC && codec->extradata_size>=23,"Expected HEVC fixture");
    const unsigned length=(codec->extradata[21]&3)+1;
    std::vector<PacketPtr> all; std::vector<std::int64_t> pts;
    while(true) {
        auto packet=make_packet(); const auto status=av_read_frame(io.input,packet.get()); if(status==AVERROR_EOF) break; av_check(status,"Read fixture packet");
        if(packet->stream_index==video) { require(packet->pts!=AV_NOPTS_VALUE,"Fixture lacks PTS"); pts.push_back(packet->pts); }
        all.push_back(std::move(packet)); require(all.size()<=4096,"Fixture too long");
    }
    std::sort(pts.begin(),pts.end()); require(std::adjacent_find(pts.begin(),pts.end())==pts.end(),"Duplicate fixture PTS");
    av_check(avformat_alloc_output_context2(&io.output,nullptr,nullptr,argv[2]),"Allocate fixture muxer");
    for(unsigned i=0;i<io.input->nb_streams;++i) {
        auto* stream=avformat_new_stream(io.output,nullptr); if(!stream) throw std::bad_alloc();
        av_check(avcodec_parameters_copy(stream->codecpar,io.input->streams[i]->codecpar),"Copy fixture stream");
        stream->codecpar->codec_tag=0; stream->time_base=io.input->streams[i]->time_base;
        if(static_cast<int>(i)==video) {
            const auto extra=patch_config({codec->extradata,static_cast<std::size_t>(codec->extradata_size)},bit);
            av_freep(&stream->codecpar->extradata);
            stream->codecpar->extradata=static_cast<std::uint8_t*>(av_mallocz(extra.size()+AV_INPUT_BUFFER_PADDING_SIZE));
            if(!stream->codecpar->extradata) throw std::bad_alloc();
            std::copy(extra.begin(),extra.end(),stream->codecpar->extradata); stream->codecpar->extradata_size=static_cast<int>(extra.size());
        }
    }
    av_check(avio_open(&io.output->pb,argv[2],AVIO_FLAG_WRITE),"Open fixture output");
    av_check(avformat_write_header(io.output,nullptr),"Write fixture header");
    for(auto& original:all) {
        auto packet=make_packet(); av_check(av_packet_ref(packet.get(),original.get()),"Reference fixture packet");
        if(packet->stream_index==video) {
            const auto rank=static_cast<std::size_t>(std::lower_bound(pts.begin(),pts.end(),packet->pts)-pts.begin());
            constexpr unsigned cadence[]={5,4,6,3}; const auto pic=rank<pts.size()/4*4 ? cadence[rank%4]:0U;
            const Data sei{78,1,1,1,static_cast<std::uint8_t>((pic<<4)|5),128};
            Bytes tail(packet->data,static_cast<std::size_t>(packet->size)); Data data; bool inserted=false;
            while(!tail.empty()) {
                require(tail.size()>=length,"Truncated fixture NAL size"); std::size_t n=0;
                for(unsigned i=0;i<length;++i) n=(n<<8)|tail[i]; tail=tail.subspan(length);
                require(n>=2 && n<=tail.size(),"Invalid fixture NAL"); const auto nal=tail.first(n); tail=tail.subspan(n);
                const auto type=(nal[0]>>1)&63;
                if(type<32 && !inserted) { append(data,sei,length); inserted=true; }
                // Remove prior prefix SEI (test fixture only); slice bytes remain unchanged.
                if(type==39) continue;
                if(type==33) append(data,patch_sps(nal,bit),length); else append(data,nal,length);
            }
            require(inserted,"Fixture packet has no slices"); auto updated=make_packet();
            av_check(av_new_packet(updated.get(),static_cast<int>(data.size())),"Allocate injected packet");
            std::copy(data.begin(),data.end(),updated->data); av_check(av_packet_copy_props(updated.get(),packet.get()),"Copy fixture timing");
            packet=std::move(updated);
        }
        const auto index=packet->stream_index;
        av_packet_rescale_ts(packet.get(),io.input->streams[index]->time_base,io.output->streams[index]->time_base);
        packet->pos=-1; av_check(av_interleaved_write_frame(io.output,packet.get()),"Mux RFF fixture");
    }
    av_check(av_write_trailer(io.output),"Finalize fixture"); std::cout<<pts.size()<<" pictures remuxed without video encoding\n";
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
