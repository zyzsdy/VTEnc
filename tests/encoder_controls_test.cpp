#include "encoder_controls.hpp"
#include "encoder_session.hpp"
#include "vtenc/status.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function action, std::string_view message) {
    try { action(); } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(message) != std::string_view::npos, error.what()); return;
    }
    throw std::runtime_error("Expected rejection: " + std::string(message));
}
vtenc::CFRef<CMSampleBufferRef> sample(std::size_t bytes, CMTime pts, bool sync = true) {
    CMBlockBufferRef block = nullptr;
    vtenc::check_status(CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, nullptr, nullptr, 0, bytes, 0, &block), "Create test block");
    vtenc::CFRef data(block);
    CMSampleBufferRef raw = nullptr;
    const CMSampleTimingInfo time{CMTimeMake(1, 30), pts, pts};
    vtenc::check_status(CMSampleBufferCreateReady(nullptr, block, nullptr, 1, 1, &time, 1, &bytes, &raw), "Create test sample");
    vtenc::CFRef result(raw);
    auto attachments = CMSampleBufferGetSampleAttachmentsArray(raw, true);
    auto entry = static_cast<CFMutableDictionaryRef>(const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
    CFDictionarySetValue(entry, kCMSampleAttachmentKey_NotSync, sync ? kCFBooleanFalse : kCFBooleanTrue);
    return result;
}
}
int main() try {
    using namespace vtenc;
    EncodeConfig config; config.bitrate = 800; config.max_bitrate = 800; // 100 bytes per second.
    EncoderRateVerifier cap(config);
    cap.append(sample(60, CMTimeMake(0, 30)).get());
    cap.append(sample(40, CMTimeMake(29, 30)).get());
    cap.append(sample(60, CMTimeMake(30, 30)).get()); // Half-open window: t=0 has expired exactly.
    rejects([&] { cap.append(sample(1, CMTimeMake(31, 30)).get()); }, "DataRateLimits violated");
    EncoderRateVerifier single(config);
    rejects([&] { single.append(sample(101, kCMTimeZero).get()); }, "DataRateLimits violated");
    EncoderRateVerifier gap(config);
    gap.append(sample(100, CMTimeMake(1001, 30000)).get());
    gap.append(sample(100, CMTimeMake(1'001'001, 30000)).get()); // VFR gap expires old bytes.
    rejects([&] { gap.append(sample(1, CMTimeMake(1'001'001, 30000)).get()); }, "increasing");
    config.max_bitrate.reset(); config.rate_control = RateControl::variable;
    config.vbv_maxrate = 800; config.vbv_buffer_bits = 1600; config.vbv_duration = 2.0;
    EncoderRateVerifier vbv(config);
    vbv.append(sample(200, CMTimeMake(0, 30)).get()); // Full initial bucket.
    vbv.append(sample(100, CMTimeMake(30, 30)).get()); // Exactly 800 bits drained.
    rejects([&] { vbv.append(sample(1, CMTimeMake(30001, 30000)).get()); }, "VBR/VBV envelope violated");
    EncoderRateVerifier rational(config);
    rational.append(sample(200, CMTimeMake(0, 30)).get());
    rational.append(sample(100, CMTimeMake(30000, 30000)).get());
    rational.append(sample(100, CMTimeMake(60, 30)).get());
    rejects([&] { rational.append(sample(200, CMTimeMake(210, 60)).get()); }, "VBR/VBV envelope violated");
    config.force_keyframes = {0, 5};
    verify_keyframe(config, 0, sample(1, kCMTimeZero).get());
    verify_keyframe(config, 1, sample(1, kCMTimeZero, false).get());
    rejects([&] { verify_keyframe(config, 5, sample(1, kCMTimeZero, false).get()); }, "ignored keyframe");
    rejects([&] { verify_keyframe_count(config, 5); }, "does not exist");
    verify_keyframe_count(config, 6);
    const auto options = keyframe_options(config, 5);
    require(CFDictionaryContainsKey(options.get(), kVTEncodeFrameOptionKey_ForceKeyFrame), "Missing per-frame force request");
    const auto parsed = parse_command_line(std::vector<std::string_view>{"-i", "missing.mp4", "-o", "out.mp4", "--vbr", "2000", "--max-bitrate", "3000", "--vbv-bufsize", "6000", "-u", "performance"});
    require(parsed.encode.bitrate == 2'000'000 && parsed.encode.vbv_maxrate == 3'000'000 && !parsed.encode.max_bitrate, "Wrong rate mode/unit mapping");
    require(parsed.encode.vbv_buffer_bits == 6'000'000 && parsed.encode.vbv_duration == 2.0, "Wrong kbit-to-seconds mapping");
    require(parsed.encode.prioritize_speed == true, "Wrong preset mapping");
    const auto json = configuration_json(parsed.encode, true);
    require(json.find("\"rate_control\":\"vbr\"") != std::string::npos && json.find("\"vbv_buffer_bits\":6000000") != std::string::npos, "Control config missing from protocol");
    std::cout << "PASS encoder control units, rational bounds and keyframe contracts\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
