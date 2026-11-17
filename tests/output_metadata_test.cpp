#include "output_metadata.hpp"
#include "encoder_session.hpp"
#include <iostream>
using namespace vtenc;
void require(bool b,const char* message) { if (!b) throw std::runtime_error(message); }
CFRef<CMVideoFormatDescriptionRef> describe(CVPixelBufferRef pixel) {
    CMVideoFormatDescriptionRef raw=nullptr;
    check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,pixel,&raw),"Test metadata format"); return CFRef(raw);
}
int main() try {
    CVPixelBufferRef raw=nullptr;
    check_status(CVPixelBufferCreate(nullptr,4,4,kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,nullptr,&raw),"Test metadata pixel"); CFRef pixel(raw);
    const auto set=[&](CFStringRef k,CFTypeRef v) { CVBufferSetAttachment(raw,k,v,kCVAttachmentMode_ShouldPropagate); };
    set(kCVImageBufferColorPrimariesKey,kCVImageBufferColorPrimaries_ITU_R_2020);
    set(kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_2020);
    set(kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
    const std::array<std::uint8_t,24> master{0x21,0x34,0x9b,0xaa,0x19,0x96,0x08,0xfc,0x8a,0x48,0x39,0x08,0x3d,0x13,0x40,0x42,0x00,0x98,0x96,0x80,0,0,0,50};
    CFRef m(CFDataCreate(nullptr,master.data(),master.size())); set(kCMFormatDescriptionExtension_MasteringDisplayColorVolume,m.get());
    const std::array<std::uint8_t,4> zero{}; CFRef light(CFDataCreate(nullptr,zero.data(),zero.size()));
    set(kCMFormatDescriptionExtension_ContentLightLevelInfo,light.get()); set(source_light_key(),light.get());
    auto original=describe(raw);
    EncodeConfig c; c.video.codec=Codec::hevc; c.video.bit_depth=10; c.hdr.mode=HdrMetadataMode::preserve;
    auto kept=output_format(c,original.get(),original.get(),raw);
    require(CMFormatDescriptionGetExtension(kept.get(),kCMFormatDescriptionExtension_MasteringDisplayColorVolume),"Mastering lost");
    require(CMFormatDescriptionGetExtension(kept.get(),source_light_key()),"Zero CLL evidence lost");
    // A real transfer conversion followed by a label override back to PQ must
    // not be mistaken for unchanged HDR pixels.
    set(kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_ITU_R_709_2);
    auto converted=describe(raw); c.color_tags.transfer=ColorTransfer::pq; c.color_tags.enabled=true;
    c.filter.colorspace.enabled=true; c.filter.colorspace.output_transfer=ColorTransfer::bt709;
    bool rejected=false;
    try { auto bad=output_format(c,converted.get(),original.get(),raw); } catch (const std::runtime_error&) { rejected=true; }
    require(rejected,"Output tags hid a pixel conversion from HDR preservation");
    c.hdr.mode=HdrMetadataMode::automatic;
    auto cleared=output_format(c,converted.get(),original.get(),raw);
    require(!CMFormatDescriptionGetExtension(cleared.get(),kCMFormatDescriptionExtension_MasteringDisplayColorVolume)&&
        !CMFormatDescriptionGetExtension(cleared.get(),source_light_key()),"Auto retained invalid HDR data");
    c.hdr.mastering=std::array<std::uint32_t,10>{8500,39850,6550,2300,35400,14600,15635,16450,10000000,50};
    c.hdr.light=std::array<std::uint16_t,2>{1000,400};
    auto explicit_values=output_format(c,converted.get(),original.get(),raw);
    require(CMFormatDescriptionGetExtension(explicit_values.get(),kCMFormatDescriptionExtension_ContentLightLevelInfo),"Explicit CLL missing");
    require(CFEqual(CMFormatDescriptionGetExtension(converted.get(),kCVImageBufferTransferFunctionKey),kCVImageBufferTransferFunction_ITU_R_709_2),"Tag override mutated input description");
    std::cout << "PASS HDR applicability, zero CLL, explicit replacement, tag isolation\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
