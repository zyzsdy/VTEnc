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

    // Golden vectors evaluated independently with 60-digit decimal arithmetic
    // from the NVEnc equations (including its BT.2390 203 reference white).
    constexpr double values[4][5] = {
        {0,.10110297981581827,.4682983508245877,.8505512097507731,1},
        {0,.18,.6631016042780749,.9266650037415815,1},
        {0,.16779661016949152,.55,.88,1},
        {0,.18,.7296599267392758,.9828647914843555,1}
    };
    constexpr double inputs[] = {0,.18,1,4,10};
    for (auto mode : {ToneMap::hable,ToneMap::mobius,ToneMap::reinhard,ToneMap::bt2390}) {
        ToneMapConfig tone; tone.mode=mode;
        validate_tonemap(tone);
        for (unsigned i=0; i<5; ++i)
            require(std::abs(color_reference::tone_channel(inputs[i],tone)-values[static_cast<unsigned>(mode)-1][i]) < 1e-7, "Tone reference differs from decimal golden vector");
        double previous=-1;
        for (unsigned i=0; i<=1000; ++i) {
            const auto value=color_reference::tone_channel(static_cast<double>(i)/10,tone);
            require(std::isfinite(value) && value >= previous-1e-10, "Tone curve is nonfinite or reverses highlights");
            previous=value;
        }
        tone.desat_strength=0;
        const auto color=color_reference::tonemap({4,1,.25},tone);
        require(std::abs(color[0]/color[1]-4) < 1e-10 && std::abs(color[1]/color[2]-4) < 1e-10, "Disabled desaturation changed RGB ratios");
        const auto black=color_reference::tonemap({0,0,0},tone);
        require(black == color_reference::V{}, "Tone mapping raised black");
    }
    FilterConfig tone_args;
    parse_colorspace("hdr2sdr=hable,source_peak=4000,ldr_nits=203,a=0.3,b=0.4,c=0.2,d=0.3,e=0.02,f=0.4,desat_base=0.1,desat_strength=1,desat_exp=0.5",tone_args);
    const auto& t=tone_args.colorspace.tonemap;
    require(t.mode == ToneMap::hable && t.source_peak == 4000 && t.ldr_nits == 203 && t.hable[0] == .3F &&
        t.hable[5] == .4F && t.desat_base == .1F && t.desat_strength == 1 && t.desat_exp == .5F, "Tone parameters were ignored");

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
    const auto defaults = make_filter_plan(config,format().get(),pixel.get());
    require(!defaults.parameters.convert_matrix && !defaults.parameters.convert_transfer &&
            defaults.parameters.input_transfer == static_cast<unsigned>(ColorTransfer::bt709) &&
            defaults.parameters.input_y_offset == 64, "Untagged P010 must resolve to BT.709 limited");
    attach(kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_2020);
    const auto track = format();
    CVBufferRemoveAttachment(pixel.get(), kCVImageBufferYCbCrMatrixKey);
    const auto partial = make_filter_plan(config,track.get(),pixel.get());
    require(partial.parameters.convert_matrix && !partial.parameters.convert_transfer &&
            partial.parameters.input_kr == matrix_coefficients(ColorMatrix::bt2020nc)[0],
            "Missing transfer/primaries must preserve the detected matrix");
    attach(kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_601_4);
    const auto from_frame = make_filter_plan(config,track.get(),pixel.get());
    require(from_frame.parameters.input_kr == matrix_coefficients(ColorMatrix::bt601)[0],
            "Frame matrix must override the track matrix");
    attach(kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_2020);
    attach(kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
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
    EncodeConfig tone_config;
    tone_config.video.bit_depth=10; tone_config.filter.input_chroma=ChromaLocation::left;
    parse_colorspace("hdr2sdr=bt2390,source_peak=4000,ldr_nits=203",tone_config.filter);
    auto tone_plan=make_filter_plan(tone_config,format().get(),pixel.get());
    require(tone_plan.parameters.tonemap.mode == ToneMap::bt2390 && tone_plan.parameters.linear_peak == 203 &&
        tone_plan.parameters.input_transfer == 16 && tone_plan.parameters.output_transfer == 1 &&
        tone_plan.parameters.convert_transfer, "Tone planner did not select display luminance and SDR transfer");
    for (const auto pair : {std::pair{kCVImageBufferColorPrimariesKey,kCVImageBufferColorPrimaries_ITU_R_709_2},
                           std::pair{kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_709_2},
                           std::pair{kCVImageBufferTransferFunctionKey,kCVImageBufferTransferFunction_ITU_R_709_2}}) {
        const auto value=CFDictionaryGetValue(tone_plan.attachments.get(),pair.first);
        require(value && CFEqual(value,pair.second), "hdr2sdr default output retained HDR labels");
    }
    require(!CFDictionaryContainsKey(tone_plan.attachments.get(),kCVImageBufferMasteringDisplayColorVolumeKey) &&
        !CFDictionaryContainsKey(tone_plan.attachments.get(),kCVImageBufferContentLightLevelInfoKey), "Tone mapping retained HDR side data");
    tone_config.filter.colorspace.input_transfer=ColorTransfer::bt709;
    rejects([&] { (void)make_filter_plan(tone_config,format().get(),pixel.get()); }, "PQ or HLG input");
    tone_config.filter.colorspace.input_transfer=ColorTransfer::automatic;
    tone_config.filter.colorspace.output_transfer=ColorTransfer::pq;
    rejects([&] { (void)make_filter_plan(tone_config,format().get(),pixel.get()); }, "SDR output");
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
