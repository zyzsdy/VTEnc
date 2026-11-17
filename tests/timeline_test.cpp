#include "media_io.hpp"
#include "source_format.hpp"
#include "encoder_session.hpp"
#include "bob.hpp"
#include "ivtc.hpp"
#include <limits>

#include <array>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    try {
        constexpr std::array<std::uint8_t, 15> sps{
            0x27, 0x64, 0x00, 0x15, 0xac, 0x56, 0x81, 0x41, 0x9f, 0x9e, 0x6a, 0x02, 0x02, 0x02, 0x04};
        constexpr std::array<std::uint8_t, 4> pps{0x28, 0xee, 0x3c, 0xb0};
        const std::uint8_t* sets[]{sps.data(), pps.data()};
        const std::size_t sizes[]{sps.size(), pps.size()};
        CMFormatDescriptionRef raw_format = nullptr;
        vtenc::check_status(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
            2, sets, sizes, 4, &raw_format), "Create timeline test format");
        vtenc::CFRef format(raw_format);
        require(vtenc::source_pixel_format(format.get()) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            "Incorrect SPS depth/chroma parsing");
        CMBlockBufferRef raw_block = nullptr;
        vtenc::check_status(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, 5,
            kCFAllocatorDefault, nullptr, 0, 5, 0, &raw_block), "Create test payload");
        vtenc::CFRef block(raw_block);
        const std::array<std::uint8_t, 5> bytes{0, 0, 0, 1, 0x65};
        vtenc::check_status(CMBlockBufferReplaceDataBytes(bytes.data(), block.get(), 0, bytes.size()), "Fill test payload");
        CMSampleBufferRef raw_sample = nullptr;
        const CMSampleTimingInfo timing{CMTimeMake(1001, 30000), CMTimeMake(3003, 30000), CMTimeMake(1001, 30000)};
        const std::size_t size = bytes.size();
        vtenc::check_status(CMSampleBufferCreateReady(kCFAllocatorDefault, block.get(), format.get(),
            1, 1, &timing, 1, &size, &raw_sample), "Create timeline test sample");
        vtenc::CFRef sample(raw_sample);
        const CMTimeMapping mapping{CMTimeRangeMake(CMTimeMake(2002, 30000), CMTimeMake(3, 1)),
                                    CMTimeRangeMake(CMTimeMake(1, 5), CMTimeMake(3, 1))};
        vtenc::CFRef mapped(vtenc::map_container_sample(sample.get(), mapping));
        require(CMTimeCompare(CMSampleBufferGetOutputPresentationTimeStamp(mapped.get()), CMTimeMake(7001, 30000)) == 0,
            "Shared movie offset not preserved");
        require(CMTimeCompare(CMSampleBufferGetOutputDecodeTimeStamp(mapped.get()), CMTimeMake(4999, 30000)) == 0,
            "Decode/presentation offset changed");
        require(CMTimeCompare(CMSampleBufferGetDuration(mapped.get()), timing.duration) == 0, "Frame duration changed");
        require(CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(sample.get()), timing.presentationTimeStamp) == 0,
            "Mapping mutated source timing");
        std::array<std::uint8_t, 5> actual{};
        vtenc::check_status(CMBlockBufferCopyDataBytes(CMSampleBufferGetDataBuffer(mapped.get()), 0, size, actual.data()), "Read mapped payload");
        require(actual == bytes, "Mapping changed compressed payload");
        auto bad = mapping;
        bad.target.duration = CMTimeMake(2, 1);
        bool rejected = false;
        try { vtenc::CFRef invalid(vtenc::map_container_sample(sample.get(), bad)); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Retimed edit accepted");
        bad = mapping;
        bad.source.start = kCMTimeInvalid;
        rejected = false;
        try { vtenc::CFRef invalid(vtenc::map_container_sample(sample.get(), bad)); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Invalid edit time accepted");
        rejected = false;
        try { vtenc::validate_video_edit(mapping, kCMTimeZero, CMTimeMake(20, 1), CMTimeMake(1, 30)); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Trimmed video edit accepted as a full-track transcode");
        // Odd ticks must double the denominator, never round half a frame.
        for (const auto duration : {CMTimeMake(1001, 30000), CMTimeMake(1, 25), CMTimeMake(7, 90)}) {
            const auto start = CMTimeMake(1, 5);
            const auto first = vtenc::bob_time(start, duration, 0);
            const auto second = vtenc::bob_time(start, duration, 1);
            require(CMTimeCompare(first.pts, start) == 0, "Bob changed movie origin");
            require(CMTimeCompare(CMTimeAdd(first.pts, first.duration), second.pts) == 0, "Gap between Bob fields");
            require(CMTimeCompare(CMTimeAdd(second.pts, second.duration), CMTimeAdd(start, duration)) == 0, "Bob changed interval end");
            require(CMTimeCompare(first.duration, second.duration) == 0, "Unequal Bob field durations");
        }
        require(vtenc::bob_timescale(30000) == 60000, "Wrong Bob movie timescale");
        for (const auto duration : {kCMTimeInvalid, kCMTimeZero, CMTimeMake(-1, 30), CMTimeMake(1, std::numeric_limits<int>::max())}) {
            rejected = false;
            try { (void)vtenc::bob_time(kCMTimeZero, duration, 0); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "Invalid or overflowing Bob interval accepted");
        }
        auto rounded = CMTimeMake(1, 30); rounded.flags |= kCMTimeFlags_HasBeenRounded;
        rejected = false;
        try { (void)vtenc::bob_time(rounded, CMTimeMake(1, 30), 0); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Rounded Bob origin accepted");
        vtenc::IvtcConfig ivtc;
        require(vtenc::ivtc_output_timescale(30000, ivtc) == 1440000, "Wrong RFF/IVTC mux timescale");
        ivtc.cycle = 0;
        require(vtenc::ivtc_output_timescale(30000, ivtc) == 360000, "Wrong RFF-only mux timescale");
        ivtc.expand = vtenc::RffExpansion::off;
        require(vtenc::ivtc_output_timescale(30000, ivtc) == 30000, "Match-only changed mux timescale");
        ivtc.cycle = 5;
        require(vtenc::ivtc_output_timescale(30000, ivtc) == 120000, "Wrong hard IVTC mux timescale");
        for (int fields : {2, 3, 4, 6}) {
            const auto duration = CMTimeMake(2500, 60000);
            const auto part = vtenc::ivtc_divide(duration, fields);
            require(!CMTIME_HAS_BEEN_ROUNDED(part) && CMTimeCompare(CMTimeMultiply(part, fields), duration) == 0,
                    "RFF field division lost precision");
        }
        for (auto duration : {CMTimeMake(5005, 30000), CMTimeMake(12, 25), CMTimeMake(2, 90)}) {
            auto quarter = vtenc::ivtc_quarter(duration);
            require(!CMTIME_HAS_BEEN_ROUNDED(quarter) && CMTimeCompare(CMTimeMultiply(quarter, 4), duration) == 0,
                    "IVTC cycle split lost precision");
        }
        for (auto duration : {kCMTimeInvalid, kCMTimeZero, CMTimeMake(-1, 30), rounded, CMTimeMake(1, std::numeric_limits<int>::max())}) {
            rejected = false;
            try { (void)vtenc::ivtc_quarter(duration); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "Invalid IVTC interval accepted");
        }
        ivtc.expand = vtenc::RffExpansion::automatic;
        for (int scale : {0, -1, std::numeric_limits<int>::max() / 48 + 1}) {
            rejected = false;
            try { (void)vtenc::ivtc_output_timescale(scale, ivtc); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "Overflowing IVTC mux clock accepted");
        }
        vtenc::IvtcScores scores{};
        require(vtenc::ivtc_select(scores) == 0, "IVTC tie must prefer current");
        scores[0].blocks = 1; scores[1].maximum = 3; scores[2].maximum = 2;
        require(vtenc::ivtc_select(scores) == 2, "IVTC comb-first selection ignored worst block");
        CVPixelBufferRef raw_pixel = nullptr;
        vtenc::check_status(CVPixelBufferCreate(nullptr, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &raw_pixel), "Create field order fixture");
        vtenc::CFRef pixel(raw_pixel);
        require(vtenc::bob_first_field(vtenc::FieldOrder::tff, format.get(), pixel.get()) == 0 &&
                vtenc::bob_first_field(vtenc::FieldOrder::bff, format.get(), pixel.get()) == 1, "Explicit field order ignored");
        rejected = false;
        try { (void)vtenc::bob_first_field(vtenc::FieldOrder::automatic, format.get(), pixel.get()); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Missing field order guessed");
        for (const auto detail : {kCVImageBufferFieldDetailTemporalTopFirst, kCVImageBufferFieldDetailSpatialFirstLineEarly}) {
            CVBufferSetAttachment(pixel.get(), kCVImageBufferFieldDetailKey, detail, kCVAttachmentMode_ShouldPropagate);
            require(vtenc::bob_first_field(vtenc::FieldOrder::automatic, format.get(), pixel.get()) == 0, "Auto TFF failed");
        }
        for (const auto detail : {kCVImageBufferFieldDetailTemporalBottomFirst, kCVImageBufferFieldDetailSpatialFirstLineLate}) {
            CVBufferSetAttachment(pixel.get(), kCVImageBufferFieldDetailKey, detail, kCVAttachmentMode_ShouldPropagate);
            require(vtenc::bob_first_field(vtenc::FieldOrder::automatic, format.get(), pixel.get()) == 1, "Auto BFF failed");
        }
        std::cout << "Source format and rational movie timeline checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
