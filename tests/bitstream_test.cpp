#include "bitstream_output.hpp"
#include "encoder_session.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Parameter sets from VTEnc's own 320x180 synthetic H.264 output; no external fixture.
constexpr std::array<std::uint8_t, 15> sps{
    0x27, 0x64, 0x00, 0x15, 0xac, 0x56, 0x81, 0x41, 0x9f, 0x9e, 0x6a, 0x02, 0x02, 0x02, 0x04};
constexpr std::array<std::uint8_t, 4> pps{0x28, 0xee, 0x3c, 0xb0};

vtenc::CFRef<CMSampleBufferRef> sample(const std::vector<std::uint8_t>& bytes, int length_size, CMTime pts) {
    const std::uint8_t* sets[]{sps.data(), pps.data()};
    const std::size_t sizes[]{sps.size(), pps.size()};
    CMFormatDescriptionRef raw_format = nullptr;
    vtenc::check_status(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2,
        sets, sizes, length_size, &raw_format), "Create test format");
    vtenc::CFRef format(raw_format);
    CMBlockBufferRef raw_block = nullptr;
    vtenc::check_status(CMBlockBufferCreateEmpty(kCFAllocatorDefault, 0, 0, &raw_block), "Create test block");
    vtenc::CFRef block(raw_block);
    // One-byte segments ensure length prefixes and NAL payloads cross memory blocks.
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        vtenc::check_status(CMBlockBufferAppendMemoryBlock(block.get(), nullptr, 1, kCFAllocatorDefault,
            nullptr, 0, 1, 0), "Append test segment");
        vtenc::check_status(CMBlockBufferReplaceDataBytes(&bytes[i], block.get(), i, 1), "Fill test segment");
    }
    const CMSampleTimingInfo timing{CMTimeMake(1001, 30000), pts, kCMTimeInvalid};
    const std::size_t size = bytes.size();
    CMSampleBufferRef raw_sample = nullptr;
    vtenc::check_status(CMSampleBufferCreateReady(kCFAllocatorDefault, block.get(), format.get(),
        1, 1, &timing, 1, &size, &raw_sample), "Create test sample");
    return vtenc::CFRef(raw_sample);
}

std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Specify test scratch directory");
        std::filesystem::create_directories(argv[1]);
        auto pattern = (std::filesystem::path(argv[1]) / "bitstream-XXXXXX").string();
        require(mkdtemp(pattern.data()) != nullptr, "mkdtemp failed");
        const std::filesystem::path directory(pattern);
        vtenc::EncodeConfig config;
        config.video.width = 320;
        config.video.height = 180;
        config.frames = 1;
        config.fps_num = 30000;
        config.fps_den = 1001;
        for (const int length_size : {1, 2, 4}) {
            config.output = (directory / ("valid-" + std::to_string(length_size) + ".h264")).string();
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length_size), 0);
            bytes.back() = 3;
            bytes.insert(bytes.end(), {0x65, 0xaa, 0xbb});
            auto frame = sample(bytes, length_size, kCMTimeZero);
            {
                vtenc::BitstreamOutput output(config);
                output.append(frame.get());
                output.publish();
            }
            std::vector<std::uint8_t> expected;
            for (const auto& nal : {std::vector(sps.begin(), sps.end()), std::vector(pps.begin(), pps.end()),
                                    std::vector<std::uint8_t>{0x65, 0xaa, 0xbb}}) {
                expected.insert(expected.end(), {0, 0, 0, 1});
                expected.insert(expected.end(), nal.begin(), nal.end());
            }
            require(read(config.output) == expected, "Annex B bytes differ across segmented blocks");
        }
        int index = 0;
        for (const auto& bytes : {std::vector<std::uint8_t>{0, 0, 0},
                                 std::vector<std::uint8_t>{0, 0, 0, 0},
                                 std::vector<std::uint8_t>{0, 0, 0, 3, 0x65}}) {
            config.output = (directory / ("invalid-" + std::to_string(index++) + ".h264")).string();
            auto frame = sample(bytes, 4, kCMTimeZero);
            bool rejected = false;
            {
                vtenc::BitstreamOutput output(config);
                try { output.append(frame.get()); }
                catch (const std::runtime_error&) { rejected = true; }
            }
            require(rejected, "Malformed NAL length accepted");
            require(!std::filesystem::exists(config.output), "Malformed stream published");
        }
        config.output = (directory / "bad-pts.h264").string();
        auto wrong_pts = sample({0, 0, 0, 1, 0x65}, 4, CMTimeMake(1001, 30000));
        bool rejected = false;
        {
            vtenc::BitstreamOutput output(config);
            try { output.append(wrong_pts.get()); }
            catch (const std::runtime_error&) { rejected = true; }
        }
        require(rejected, "Unexpected timestamp accepted");

        // A competing writer appearing after construction must not be overwritten.
        config.output = (directory / "race.h264").string();
        {
            vtenc::BitstreamOutput output(config);
            auto frame = sample({0, 0, 0, 1, 0x65}, 4, kCMTimeZero);
            output.append(frame.get());
            { std::ofstream other(config.output); other << "keep"; }
            rejected = false;
            try { output.publish(); }
            catch (const std::runtime_error&) { rejected = true; }
        }
        require(rejected && read(config.output) == std::vector<std::uint8_t>{'k', 'e', 'e', 'p'},
            "Publication clobbered competing output");
        require(!std::filesystem::exists(config.output + ".timestamps.csv"), "Sidecar publication not rolled back");
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            require(entry.path().string().find(".part.") == std::string::npos, "Temporary output leaked");
        }
        std::filesystem::remove_all(directory);
        std::cout << "Bitstream adapter and transactional output checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
