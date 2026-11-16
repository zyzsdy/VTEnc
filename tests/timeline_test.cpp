#include "media_io.hpp"
#include "source_format.hpp"
#include "encoder_session.hpp"

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
        std::cout << "Source format and rational movie timeline checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
