#include "vtenc/colorspace.hpp"
#include "gpu_filter.hpp"
#include "encoder_session.hpp"
#include "colorspace_reference.hpp"
#include <cmath>
#include <iostream>

using namespace vtenc;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* text) {
    try { action(); } catch (const std::exception& e) {
        require(std::string(e.what()).find(text) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error("Expected failure was accepted");
}
}
int main() try {
    for (auto from : {ColorPrimaries::bt709, ColorPrimaries::bt2020, ColorPrimaries::display_p3})
    for (auto to : {ColorPrimaries::bt709, ColorPrimaries::bt2020, ColorPrimaries::display_p3}) {
        const auto m = gamut_matrix(from,to);
        const auto luma = primaries_luma(from);
        for (unsigned c = 0; c < 3; ++c) require(std::abs(luma[c]-color_reference::xyz(from)[1][c]) < 3e-8, "HLG luma differs from native RGB/XYZ Y row");
        for (unsigned c = 0; c < 3; ++c) {
            color_reference::V basis{}; basis[c] = 1;
            const auto expected = color_reference::mul(color_reference::rgb(to), color_reference::mul(color_reference::xyz(from),basis));
            for (unsigned r = 0; r < 3; ++r) require(std::abs(m[r*3+c]-expected[r]) < 2e-7, "Gamut matrix differs from fixed XYZ reference");
        }
    }
    for (auto from : {ColorPrimaries::bt709, ColorPrimaries::bt470bg, ColorPrimaries::smpte170m, ColorPrimaries::bt2020, ColorPrimaries::dci_p3, ColorPrimaries::display_p3}) {
        const auto forward = gamut_matrix(from,ColorPrimaries::bt709);
        const auto back = gamut_matrix(ColorPrimaries::bt709,from);
        for (unsigned r = 0; r < 3; ++r) {
            require(std::abs(forward[r*3]+forward[r*3+1]+forward[r*3+2]-1) < 3e-7, "Bradford adaptation failed to preserve neutral white");
            for (unsigned c = 0; c < 3; ++c) {
                double product = 0;
                for (unsigned k = 0; k < 3; ++k) product += static_cast<double>(forward[r*3+k])*back[k*3+c];
                require(std::abs(product-(r==c ? 1 : 0)) < 3e-7, "Gamut inverse failed");
            }
        }
    }
    FilterConfig parsed;
    parse_colorspace("matrix=bt2020nc:bt709,transfer=smpte2084:bt709,colorprim=bt2020:bt709,range=full:limited,scene_ref=true,sdr_source_peak=203",parsed);
    require(parsed.requested && parsed.colorspace.enabled && parsed.input_range == ColorRange::full && parsed.range == ColorRange::limited &&
        parsed.input_matrix == ColorMatrix::bt2020nc && parsed.output_matrix == ColorMatrix::bt709 &&
        parsed.colorspace.input_transfer == ColorTransfer::pq && parsed.colorspace.output_transfer == ColorTransfer::bt709 &&
        parsed.colorspace.input_primaries == ColorPrimaries::bt2020 && parsed.colorspace.output_primaries == ColorPrimaries::bt709 &&
        parsed.colorspace.scene_ref && parsed.colorspace.sdr_source_peak == 203, "Colorspace option did not populate conversion endpoints");

    CVPixelBufferRef raw_pixel = nullptr;
    check_status(CVPixelBufferCreate(nullptr,16,16,kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,nullptr,&raw_pixel), "Create metadata fixture");
    CFRef pixel(raw_pixel);
    const auto attach = [&](CFStringRef key, CFStringRef value) { CVBufferSetAttachment(pixel.get(),key,value,kCVAttachmentMode_ShouldPropagate); };
    const auto format = [&]() {
        CMVideoFormatDescriptionRef raw = nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,pixel.get(),&raw), "Describe metadata fixture");
        return CFRef(raw);
    };
    EncodeConfig config;
    config.video.bit_depth = 10;
    config.filter.input_chroma = ChromaLocation::left;
    parse_colorspace("transfer=auto:bt709,colorprim=auto:bt709,matrix=auto:bt709",config.filter);
    rejects([&] { (void)make_filter_plan(config,format().get(),pixel.get()); }, "matrix");
    attach(kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_2020);
    rejects([&] { (void)make_filter_plan(config,format().get(),pixel.get()); }, "transfer");
    attach(kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
    rejects([&] { (void)make_filter_plan(config,format().get(),pixel.get()); }, "primaries");
    attach(kCVImageBufferColorPrimariesKey,kCVImageBufferColorPrimaries_ITU_R_2020);
    // Old HDR mastering/content-light data must not describe transformed pixels.
    const std::uint8_t bytes[] = {1,2,3};
    CFRef hdr(CFDataCreate(nullptr,bytes,3));
    CVBufferSetAttachment(pixel.get(), kCVImageBufferMasteringDisplayColorVolumeKey, hdr.get(), kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel.get(), kCVImageBufferContentLightLevelInfoKey, hdr.get(), kCVAttachmentMode_ShouldPropagate);
    auto plan = make_filter_plan(config,format().get(),pixel.get());
    require(plan.parameters.convert_transfer && plan.parameters.input_transfer == 16 && plan.parameters.output_transfer == 1, "HDR metadata auto resolution failed");
    require(!CFDictionaryContainsKey(plan.attachments.get(),kCVImageBufferMasteringDisplayColorVolumeKey) &&
        !CFDictionaryContainsKey(plan.attachments.get(),kCVImageBufferContentLightLevelInfoKey), "Stale HDR metadata propagated");
    const auto explicit_plan = plan.parameters;
    config.filter.colorspace.input_transfer = ColorTransfer::pq;
    config.filter.colorspace.input_primaries = ColorPrimaries::bt2020;
    config.filter.input_matrix = ColorMatrix::bt2020nc;
    auto override_plan = make_filter_plan(config,format().get(),pixel.get());
    require(override_plan.parameters.input_transfer == explicit_plan.input_transfer && override_plan.parameters.input_kr == explicit_plan.input_kr, "Explicit and automatic metadata differ");
    config.filter.colorspace.enabled = false;
    rejects([&] { (void)make_filter_plan(config,format().get(),pixel.get()); }, "HDR");
    config.filter.colorspace.enabled = true;
    attach(kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_ITU_R_2100_HLG);
    rejects([&] { validate_filter_frame(plan,pixel.get()); }, "metadata changed");
    std::cout << "PASS colorspace matrices, CLI endpoints, metadata resolution and rejection checks\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
