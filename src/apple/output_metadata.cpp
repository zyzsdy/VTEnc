#include "output_metadata.hpp"
#include "encoder_session.hpp"
#include "source_format.hpp"
extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
}
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

namespace vtenc {
namespace {
using Bytes=std::vector<std::uint8_t>;
void put(Bytes& b,std::uint32_t v,int n) { for (int i=n-1;i>=0;--i) b.push_back(static_cast<std::uint8_t>(v>>(i*8))); }
std::uint32_t get(const std::uint8_t*& p,int n) { std::uint32_t v=0; for (int i=0;i<n;++i) v=(v<<8)|*p++; return v; }
void data(CFMutableDictionaryRef d,CFStringRef key,const Bytes& b) {
    CFRef value(CFDataCreate(nullptr,b.data(),static_cast<CFIndex>(b.size()))); CFDictionarySetValue(d,key,value.get());
}
std::uint32_t units(AVRational value,int scale,std::uint32_t limit) {
    if (value.num<0||value.den<=0) throw std::runtime_error("Invalid HDR metadata rational");
    const auto v=av_rescale_rnd(value.num,scale,value.den,AV_ROUND_NEAR_INF);
    if (v<0||v>limit) throw std::runtime_error("HDR metadata exceeds SEI field range");
    return static_cast<std::uint32_t>(v);
}
void mastering(CFMutableDictionaryRef d,const void* ptr,std::size_t size) {
    if (size<sizeof(AVMasteringDisplayMetadata)) throw std::runtime_error("Truncated mastering display metadata");
    const auto& m=*static_cast<const AVMasteringDisplayMetadata*>(ptr);
    if (!m.has_primaries||!m.has_luminance) throw std::runtime_error("Incomplete mastering display metadata");
    Bytes b;
    for (auto i:{1,2,0}) for (int j=0;j<2;++j) put(b,units(m.display_primaries[i][j],50000,50000),2);
    for (auto v:m.white_point) put(b,units(v,50000,50000),2);
    put(b,units(m.max_luminance,10000,100000000),4); put(b,units(m.min_luminance,10000,100000000),4);
    data(d,kCMFormatDescriptionExtension_MasteringDisplayColorVolume,b);
}
void light(CFMutableDictionaryRef d,const void* ptr,std::size_t size) {
    if (size<sizeof(AVContentLightMetadata)) throw std::runtime_error("Truncated content light metadata");
    const auto& m=*static_cast<const AVContentLightMetadata*>(ptr);
    if (m.MaxCLL>65535||m.MaxFALL>65535) throw std::runtime_error("Content light metadata exceeds SEI range");
    Bytes b; put(b,m.MaxCLL,2); put(b,m.MaxFALL,2); data(d,kCMFormatDescriptionExtension_ContentLightLevelInfo,b); data(d,source_light_key(),b);
}
CFRef<CFMutableDictionaryRef> dictionary() { return CFRef(CFDictionaryCreateMutable(nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks)); }
}
void import_hdr_metadata(CFMutableDictionaryRef d,const AVCodecParameters* p) {
    for (int i=0;i<p->nb_coded_side_data;++i) {
        const auto& s=p->coded_side_data[i];
        if (s.type==AV_PKT_DATA_MASTERING_DISPLAY_METADATA) mastering(d,s.data,s.size);
        if (s.type==AV_PKT_DATA_CONTENT_LIGHT_LEVEL) light(d,s.data,s.size);
    }
}
void import_hdr_metadata(CVPixelBufferRef pixel,const AVFrame* frame) {
    auto d=dictionary();
    if (auto s=av_frame_get_side_data(frame,AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) mastering(d.get(),s->data,s->size);
    if (auto s=av_frame_get_side_data(frame,AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) light(d.get(),s->data,s->size);
    CVBufferSetAttachments(pixel,d.get(),kCVAttachmentMode_ShouldPropagate);
}
void export_hdr_metadata(AVCodecParameters* p,CMFormatDescriptionRef format) {
    for (auto key:{kCMFormatDescriptionExtension_MasteringDisplayColorVolume,kCMFormatDescriptionExtension_ContentLightLevelInfo}) {
        auto v=CMFormatDescriptionGetExtension(format,key);
        if (!v&&CFEqual(key,kCMFormatDescriptionExtension_ContentLightLevelInfo)) v=CMFormatDescriptionGetExtension(format,source_light_key());
        if (!v) continue;
        const bool master=CFEqual(key,kCMFormatDescriptionExtension_MasteringDisplayColorVolume);
        if (CFGetTypeID(v)!=CFDataGetTypeID()||CFDataGetLength(static_cast<CFDataRef>(v))!=(master?24:4)) throw std::runtime_error("Invalid encoded HDR metadata");
        auto bytes=CFDataGetBytePtr(static_cast<CFDataRef>(v));
        const auto size=master?sizeof(AVMasteringDisplayMetadata):sizeof(AVContentLightMetadata);
        auto s=av_packet_side_data_new(&p->coded_side_data,&p->nb_coded_side_data,
            master?AV_PKT_DATA_MASTERING_DISPLAY_METADATA:AV_PKT_DATA_CONTENT_LIGHT_LEVEL,size,0);
        if (!s) throw std::bad_alloc();
        std::memset(s->data,0,size);
        if (master) {
            auto& m=*reinterpret_cast<AVMasteringDisplayMetadata*>(s->data);
            for (auto i:{1,2,0}) for (int j=0;j<2;++j) m.display_primaries[i][j]={static_cast<int>(get(bytes,2)),50000};
            for (auto& w:m.white_point) w={static_cast<int>(get(bytes,2)),50000};
            m.max_luminance={static_cast<int>(get(bytes,4)),10000}; m.min_luminance={static_cast<int>(get(bytes,4)),10000};
            m.has_primaries=m.has_luminance=1;
        } else {
            auto& m=*reinterpret_cast<AVContentLightMetadata*>(s->data); m.MaxCLL=get(bytes,2); m.MaxFALL=get(bytes,2);
        }
    }
}
CFRef<CMVideoFormatDescriptionRef> output_format(const EncodeConfig& c,CMFormatDescriptionRef processed,CMFormatDescriptionRef original,CVPixelBufferRef pixel) {
    auto d=CFRef(CFDictionaryCreateMutableCopy(nullptr,0,CMFormatDescriptionGetExtensions(processed)));
    const auto set=[&](CFStringRef key,CFStringRef value) { if (!value) throw std::runtime_error("CoreVideo cannot represent output color tag"); CFDictionarySetValue(d.get(),key,value); };
    if (c.color_tags.matrix!=ColorMatrix::automatic) set(kCVImageBufferYCbCrMatrixKey,CVYCbCrMatrixGetStringForIntegerCodePoint(static_cast<int>(c.color_tags.matrix)));
    if (c.color_tags.transfer!=ColorTransfer::automatic) set(kCVImageBufferTransferFunctionKey,CVTransferFunctionGetStringForIntegerCodePoint(static_cast<int>(c.color_tags.transfer)));
    if (c.color_tags.primaries!=ColorPrimaries::automatic) set(kCVImageBufferColorPrimariesKey,CVColorPrimariesGetStringForIntegerCodePoint(static_cast<int>(c.color_tags.primaries)));
    const auto is=[&](CFStringRef key,CFStringRef value) { auto v=CFDictionaryGetValue(d.get(),key); return v&&CFEqual(v,value); };
    const bool hdr10=c.video.codec==Codec::hevc && c.video.bit_depth==10 &&
        is(kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) &&
        is(kCVImageBufferColorPrimariesKey,kCVImageBufferColorPrimaries_ITU_R_2020) &&
        is(kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_2020);
    bool changed=!c.subtitles.empty()||c.filter.bob||c.filter.ivtc.enabled||c.sync!=SyncMode::vfr||
        c.filter.crop!=std::array<int,4>{}||c.filter.output_size.has_value()||c.filter.colorspace.tonemap.mode!=ToneMap::none;
    // Source interpretation overrides and changed color semantics invalidate old metadata.
    const auto& f=c.filter;
    changed|=f.input_matrix!=ColorMatrix::automatic||f.input_range!=ColorRange::automatic||
        f.colorspace.input_transfer!=ColorTransfer::automatic||f.colorspace.input_primaries!=ColorPrimaries::automatic;
    for (auto key:{kCVImageBufferTransferFunctionKey,kCVImageBufferColorPrimariesKey,kCVImageBufferYCbCrMatrixKey,kCMFormatDescriptionExtension_FullRangeVideo}) {
        const auto a=CMFormatDescriptionGetExtension(original,key);
        const auto same=[&](CFTypeRef b) {
            if (CFEqual(key,kCMFormatDescriptionExtension_FullRangeVideo))
                return bool(a&&CFEqual(a,kCFBooleanTrue))==bool(b&&CFEqual(b,kCFBooleanTrue));
            return (!a&&!b)||(a&&b&&CFEqual(a,b));
        };
        // A tag override must not hide an earlier pixel-space conversion.
        changed|=!same(CMFormatDescriptionGetExtension(processed,key))||!same(CFDictionaryGetValue(d.get(),key));
    }
    unsigned index=0;
    for (auto key:{kCMFormatDescriptionExtension_MasteringDisplayColorVolume,kCMFormatDescriptionExtension_ContentLightLevelInfo}) {
        const bool explicit_value=index==0?c.hdr.mastering.has_value():c.hdr.light.has_value();
        // CreateForImageBuffer may omit the all-zero ContentLightLevelInfo.
        // Read the original pixel attachment before consulting format extensions.
        std::optional<CFRef<CFTypeRef>> attachment;
        if (auto raw=CVBufferCopyAttachment(pixel,key,nullptr)) attachment.emplace(raw);
        auto source=attachment?attachment->get():CMFormatDescriptionGetExtension(processed,key);
        if (!source) source=CMFormatDescriptionGetExtension(original,key);
        const auto evidence=index==0?source_master_key():source_light_key();
        if (!source) {
            if (auto raw=CVBufferCopyAttachment(pixel,evidence,nullptr)) { attachment.emplace(raw); source=attachment->get(); }
            if (!source) source=CMFormatDescriptionGetExtension(original,evidence);
        }
        CFDictionaryRemoveValue(d.get(),evidence);
        CFDictionaryRemoveValue(d.get(),key);
        if (source && c.hdr.mode!=HdrMetadataMode::clear && !explicit_value) {
            if (CFGetTypeID(source)!=CFDataGetTypeID()||CFDataGetLength(static_cast<CFDataRef>(source))!=(index==0?24:4))
                throw std::runtime_error("Invalid source HDR static metadata");
            if (hdr10&&!changed) {
                CFDictionarySetValue(d.get(),key,source);
                if (index==1) CFDictionarySetValue(d.get(),source_light_key(),source);
            }
            else if (c.hdr.mode==HdrMetadataMode::preserve) throw std::runtime_error("HDR metadata preserve is invalid after pixel/color/timeline changes or for non-HDR10 output; clear or provide new values");
            else std::cerr << "vtenc: cleared inapplicable HDR " << (index==0?"mastering display":"content light") << " metadata\n";
        }
        ++index;
    }
    if ((c.hdr.mastering||c.hdr.light)&&!hdr10) throw std::runtime_error("Explicit HDR static metadata requires HEVC Main10 PQ / BT.2020 NCL output");
    if (c.hdr.mastering) { Bytes b; for (unsigned i=0;i<10;++i) put(b,(*c.hdr.mastering)[i],i<8?2:4); data(d.get(),kCMFormatDescriptionExtension_MasteringDisplayColorVolume,b); }
    if (c.hdr.light) { Bytes b; for (auto v:*c.hdr.light) put(b,v,2); data(d.get(),kCMFormatDescriptionExtension_ContentLightLevelInfo,b); data(d.get(),source_light_key(),b); }
    const auto size=CMVideoFormatDescriptionGetDimensions(processed); CMVideoFormatDescriptionRef raw=nullptr;
    check_status(CMVideoFormatDescriptionCreate(nullptr,CMFormatDescriptionGetMediaSubType(processed),size.width,size.height,d.get(),&raw),"Create output metadata description");
    return CFRef(raw);
}
void verify_output_metadata(CMFormatDescriptionRef expected,CMFormatDescriptionRef actual) {
    for (auto key:{kCVImageBufferColorPrimariesKey,kCVImageBufferTransferFunctionKey,kCVImageBufferYCbCrMatrixKey,
        kCMFormatDescriptionExtension_MasteringDisplayColorVolume,kCMFormatDescriptionExtension_ContentLightLevelInfo}) {
        auto a=CMFormatDescriptionGetExtension(expected,key),b=CMFormatDescriptionGetExtension(actual,key);
        const bool hdr=CFEqual(key,kCMFormatDescriptionExtension_MasteringDisplayColorVolume)||CFEqual(key,kCMFormatDescriptionExtension_ContentLightLevelInfo);
        if ((a&&(!b||!CFEqual(a,b)))||(!a&&b&&hdr)) throw std::runtime_error("Encoder changed requested output color/HDR metadata");
    }
}
// CoreVideo can omit ContentLightLevelInfo=0,0 from its attachments and
// format descriptions. Normalize static SEI in the compressed packet so
// preserved/explicit/cleared metadata has identical semantics in all muxers.
CFRef<CMSampleBufferRef> normalize_hdr_sei(CMSampleBufferRef sample,CMFormatDescriptionRef expected) {
    const auto format=CMSampleBufferGetFormatDescription(sample);
    if (CMFormatDescriptionGetMediaSubType(format)!=kCMVideoCodecType_HEVC) { CFRetain(sample); return CFRef(sample); }
    const std::uint8_t* set=nullptr; std::size_t set_size=0; int length_size=0;
    check_status(CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format,0,&set,&set_size,nullptr,&length_size),"Read HDR SEI NAL framing");
    if (length_size!=4) throw std::runtime_error("HDR SEI adapter requires four-byte encoder NAL lengths");
    bool sync=true;
    const auto attachments=CMSampleBufferGetSampleAttachmentsArray(sample,false);
    if (attachments&&CFArrayGetCount(attachments)) {
        const auto d=static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments,0));
        const auto flag=CFDictionaryGetValue(d,kCMSampleAttachmentKey_NotSync); sync=!flag||!CFEqual(flag,kCFBooleanTrue);
    }
    Bytes requested;
    unsigned type=137;
    for (auto key:{kCMFormatDescriptionExtension_MasteringDisplayColorVolume,kCMFormatDescriptionExtension_ContentLightLevelInfo}) {
        auto v=CMFormatDescriptionGetExtension(expected,key);
        if (!v&&type==144) v=CMFormatDescriptionGetExtension(expected,source_light_key());
        if (sync&&v) {
            const auto d=static_cast<CFDataRef>(v); const auto n=CFDataGetLength(d);
            requested.push_back(static_cast<std::uint8_t>(type)); requested.push_back(static_cast<std::uint8_t>(n));
            requested.insert(requested.end(),CFDataGetBytePtr(d),CFDataGetBytePtr(d)+n);
        }
        type=144;
    }
    auto block=CMSampleBufferGetDataBuffer(sample); Bytes input(CMBlockBufferGetDataLength(block));
    check_status(CMBlockBufferCopyDataBytes(block,0,input.size(),input.data()),"Read HDR SEI packet");
    Bytes output;
    const auto nal=[&](const Bytes& data) { if (data.size()>UINT32_MAX) throw std::runtime_error("Oversized HDR NAL"); put(output,static_cast<std::uint32_t>(data.size()),4); output.insert(output.end(),data.begin(),data.end()); };
    const auto sei=[&](Bytes rbsp,std::uint8_t h0,std::uint8_t h1) {
        rbsp.push_back(0x80); Bytes data{h0,h1}; unsigned zeros=0;
        for (auto b:rbsp) { if (zeros>=2&&b<=3) { data.push_back(3); zeros=0; } data.push_back(b); zeros=b==0?zeros+1:0; }
        nal(data);
    };
    if (!requested.empty()) sei(requested,39<<1,1);
    std::size_t at=0;
    while (at<input.size()) {
        if (input.size()-at<4) throw std::runtime_error("Truncated encoder NAL prefix");
        const auto* ptr=input.data()+at; const auto n=get(ptr,4); at+=4;
        if (n<2||n>input.size()-at) throw std::runtime_error("Invalid encoder NAL size");
        Bytes data(input.begin()+static_cast<std::ptrdiff_t>(at),input.begin()+static_cast<std::ptrdiff_t>(at+n)); at+=n;
        const auto kind=(data[0]>>1)&63;
        if (kind!=39&&kind!=40) { nal(data); continue; }
        Bytes rbsp; unsigned zeros=0;
        for (std::size_t i=2;i<data.size();++i) {
            const auto b=data[i]; if (zeros>=2&&b==3) { zeros=0; continue; }
            rbsp.push_back(b); zeros=b==0?zeros+1:0;
        }
        Bytes kept; std::size_t pos=0;
        while (pos<rbsp.size()) {
            if (rbsp[pos]==0x80 && pos+1==rbsp.size()) { ++pos; break; }
            const auto start=pos;
            const auto field=[&]() { std::size_t v=0; unsigned b=0; do { if (pos==rbsp.size()) throw std::runtime_error("Truncated encoder SEI field"); b=rbsp[pos++]; v+=b; } while (b==255); return v; };
            const auto payload=field(),size=field();
            if (size>rbsp.size()-pos) throw std::runtime_error("Truncated encoder SEI payload");
            pos+=size;
            if (payload!=137&&payload!=144) kept.insert(kept.end(),rbsp.begin()+static_cast<std::ptrdiff_t>(start),rbsp.begin()+static_cast<std::ptrdiff_t>(pos));
        }
        if (!kept.empty()) sei(std::move(kept),data[0],data[1]);
    }
    CMBlockBufferRef raw_block=nullptr;
    check_status(CMBlockBufferCreateWithMemoryBlock(nullptr,nullptr,output.size(),nullptr,nullptr,0,output.size(),0,&raw_block),"Allocate normalized HDR packet");
    CFRef buffer(raw_block);
    check_status(CMBlockBufferReplaceDataBytes(output.data(),raw_block,0,output.size()),"Write normalized HDR packet");
    CMSampleTimingInfo timing{CMSampleBufferGetDuration(sample),CMSampleBufferGetPresentationTimeStamp(sample),CMSampleBufferGetDecodeTimeStamp(sample)};
    const auto size=output.size(); CMSampleBufferRef raw=nullptr;
    check_status(CMSampleBufferCreateReady(nullptr,raw_block,format,1,1,&timing,1,&size,&raw),"Create normalized HDR sample");
    CFRef result(raw);
    if (attachments&&CFArrayGetCount(attachments)) {
        const auto from=static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments,0));
        const auto array=CMSampleBufferGetSampleAttachmentsArray(raw,true);
        auto to=static_cast<CFMutableDictionaryRef>(const_cast<void*>(CFArrayGetValueAtIndex(array,0)));
        const auto count=CFDictionaryGetCount(from); std::vector<const void*> keys(static_cast<std::size_t>(count)),values(static_cast<std::size_t>(count));
        CFDictionaryGetKeysAndValues(from,keys.data(),values.data());
        for (CFIndex i=0;i<count;++i) CFDictionarySetValue(to,keys[static_cast<std::size_t>(i)],values[static_cast<std::size_t>(i)]);
    }
    return result;
}

void collect_hdr_sei(CMSampleBufferRef sample,CFMutableDictionaryRef state) {
    const auto format=CMSampleBufferGetFormatDescription(sample);
    const auto codec=CMFormatDescriptionGetMediaSubType(format);
    if (codec!=kCMVideoCodecType_HEVC&&codec!=0x68657631U) return;
    auto compatible=hardware_decode_format(format);
    const std::uint8_t* first=nullptr; std::size_t first_size=0; int prefix=0;
    check_status(CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(compatible.get(),0,&first,&first_size,nullptr,&prefix),"Read source HDR NAL framing");
    if (prefix<1||prefix>4) throw std::runtime_error("Invalid source HDR NAL prefix");
    auto block=CMSampleBufferGetDataBuffer(sample); Bytes input(CMBlockBufferGetDataLength(block));
    check_status(CMBlockBufferCopyDataBytes(block,0,input.size(),input.data()),"Read source HDR packet");
    std::size_t at=0;
    while (at<input.size()) {
        if (input.size()-at<static_cast<std::size_t>(prefix)) throw std::runtime_error("Truncated source HDR NAL prefix");
        const auto* ptr=input.data()+at; const auto size=get(ptr,prefix); at+=static_cast<std::size_t>(prefix);
        if (size<2||size>input.size()-at) throw std::runtime_error("Invalid source HDR NAL size");
        const auto begin=at; at+=size; const auto type=(input[begin]>>1)&63;
        if (type!=39&&type!=40) continue;
        Bytes rbsp; unsigned zeros=0;
        for (std::size_t i=begin+2;i<at;++i) {
            const auto b=input[i]; if (zeros>=2&&b==3) { zeros=0; continue; }
            rbsp.push_back(b); zeros=b==0?zeros+1:0;
        }
        std::size_t pos=0;
        while (pos<rbsp.size()) {
            if (rbsp[pos]==0x80&&pos+1==rbsp.size()) break;
            const auto field=[&]() { std::size_t v=0; unsigned b=0; do { if (pos==rbsp.size()) throw std::runtime_error("Truncated source HDR SEI field"); b=rbsp[pos++]; v+=b; } while (b==255); return v; };
            const auto payload=field(),n=field();
            if (n>rbsp.size()-pos) throw std::runtime_error("Truncated source HDR SEI payload");
            if (payload==137||payload==144) {
                if (n!=(payload==137?24U:4U)) throw std::runtime_error("Invalid source static HDR SEI size");
                const auto key=payload==137?kCMFormatDescriptionExtension_MasteringDisplayColorVolume:source_light_key();
                CFRef value(CFDataCreate(nullptr,rbsp.data()+pos,static_cast<CFIndex>(n)));
                if (auto old=CFDictionaryGetValue(state,key); old&&!CFEqual(old,value.get()))
                    throw std::runtime_error("Changing static HDR metadata is unsupported");
                CFDictionarySetValue(state,key,value.get());
                if (payload==144) CFDictionarySetValue(state,kCMFormatDescriptionExtension_ContentLightLevelInfo,value.get());
            }
            pos+=n;
        }
    }
}

}
