#include "vtenc/command_line.hpp"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <limits>
#include <numeric>
#include <locale>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>

namespace vtenc {
namespace {
std::int32_t positive_integer(std::string_view value, std::string_view option) {
    std::int32_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() || number <= 0) {
        throw std::invalid_argument(std::string(option) + " requires an integer in 1..2147483647");
    }
    return number;
}
}

std::string_view codec_name(Codec codec) {
    switch (codec) {
    case Codec::h264: return "h264";
    case Codec::hevc: return "hevc";
    }
    throw std::invalid_argument("Unknown codec enum");
}

CommandLine parse_command_line(std::span<const std::string_view> args) {
    CommandLine result;
    bool has_command = false;
    bool encode_options = false;
    bool synthetic = false;
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        if (arg == "-h") arg = "--help";
        if (arg == "--input") arg = "-i";
        if (arg == "--output") arg = "-o";
        if (!seen.insert(arg).second) throw std::invalid_argument("Duplicate option: " + std::string(arg));
        if (arg == "--help" || arg == "--version" || arg == "--check-device" || arg == "--check-features") {
            if (has_command) throw std::invalid_argument("Specify exactly one command");
            has_command = true;
            if (arg == "--help") result.command = Command::help;
            else if (arg == "--version") result.command = Command::version;
            else if (arg == "--check-device") result.command = Command::check_device;
            else result.command = Command::check_features;
            continue;
        }
        if (arg == "--synthetic") {
            synthetic = true;
            encode_options = true;
            continue;
        }
        if (arg == "--audio-copy" || arg == "--no-audio") {
            result.encode.copy_audio = arg == "--audio-copy";
            encode_options = true;
            continue;
        }
        const bool common = arg == "--codec" || arg == "--output-depth";
        if (!common && arg != "-i" && arg != "-o" && arg != "--input-res" && arg != "--fps" &&
            arg != "--frames" && arg != "--gop-len" && arg != "--bitrate" && arg != "--quality") {
            throw std::invalid_argument("Unknown argument: " + std::string(arg));
        }
        encode_options |= !common;
        if (++i == args.size() || args[i].empty() || args[i].starts_with("--")) {
            throw std::invalid_argument("Missing value for " + std::string(arg));
        }
        const auto value = args[i];
        if (arg == "--codec") {
            if (value == "h264") result.probe.codec = Codec::h264;
            else if (value == "hevc") result.probe.codec = Codec::hevc;
            else throw std::invalid_argument("Supported codecs: h264, hevc");
        } else if (arg == "--output-depth") {
            if (value == "8") result.probe.bit_depth = 8;
            else if (value == "10") result.probe.bit_depth = 10;
            else throw std::invalid_argument("--output-depth must be 8 or 10");
        } else if (arg == "-i") result.encode.input = value;
        else if (arg == "-o") result.encode.output = value;
        else if (arg == "--input-res") {
            const auto split = value.find('x');
            if (split == std::string_view::npos) throw std::invalid_argument("--input-res requires WIDTHxHEIGHT");
            result.probe.width = positive_integer(value.substr(0, split), arg);
            result.probe.height = positive_integer(value.substr(split + 1), arg);
            if (result.probe.width % 2 || result.probe.height % 2) {
                throw std::invalid_argument("--input-res requires even width and height for 4:2:0");
            }
        } else if (arg == "--fps") {
            const auto split = value.find('/');
            result.encode.fps_num = positive_integer(value.substr(0, split), arg);
            result.encode.fps_den = split == std::string_view::npos ? 1 : positive_integer(value.substr(split + 1), arg);
            const auto divisor = std::gcd(result.encode.fps_num, result.encode.fps_den);
            result.encode.fps_num /= divisor;
            result.encode.fps_den /= divisor;
        } else if (arg == "--frames") result.encode.frames = positive_integer(value, arg);
        else if (arg == "--gop-len") result.encode.gop = positive_integer(value, arg);
        else if (arg == "--bitrate") {
            const auto kbps = positive_integer(value, arg);
            if (kbps > std::numeric_limits<std::int32_t>::max() / 1000) {
                throw std::invalid_argument("--bitrate exceeds 2147483 kbit/s");
            }
            result.encode.bitrate = kbps * 1000;
        } else if (arg == "--quality") {
            double quality = 0;
            std::istringstream parser{std::string(value)};
            parser.imbue(std::locale::classic());
            parser >> std::noskipws >> quality;
            if (!parser || parser.peek() != std::char_traits<char>::eof() ||
                !std::isfinite(quality) || quality < 0 || quality > 1) {
                throw std::invalid_argument("--quality requires a finite number in 0..1 (VideoToolbox Quality)");
            }
            result.encode.quality = quality;
        }
    }
    if (encode_options) {
        if (has_command) throw std::invalid_argument("Encoding options cannot be combined with a query command");
        result.command = Command::encode;
        if (synthetic == !result.encode.input.empty()) {
            throw std::invalid_argument("Encoding requires exactly one source: --synthetic or -i raw-file");
        }
        if (result.encode.output.empty() || result.encode.output == "-") {
            throw std::invalid_argument("Encoding requires -o output-file (stdout is not supported)");
        }
        const auto extension = [](const std::string& path) {
            auto ext = std::filesystem::path(path).extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        };
        const auto input_ext = extension(result.encode.input);
        if (!synthetic && (input_ext == ".mp4" || input_ext == ".mov")) {
            result.command = Command::transcode;
            const auto output_ext = extension(result.encode.output);
            if (output_ext != ".mp4" && output_ext != ".mov") {
                throw std::invalid_argument("Container transcoding requires an .mp4 or .mov output");
            }
            if (seen.contains("--input-res") || seen.contains("--frames") || seen.contains("--fps")) {
                throw std::invalid_argument("Container transcoding preserves source dimensions/timing; --input-res, --frames and --fps are raw-only");
            }
            if (seen.contains("--audio-copy") && seen.contains("--no-audio")) {
                throw std::invalid_argument("--audio-copy and --no-audio are mutually exclusive");
            }
        } else {
            if (!seen.contains("--input-res")) throw std::invalid_argument("Encoding requires --input-res WIDTHxHEIGHT");
            if (!seen.contains("--frames")) throw std::invalid_argument("Encoding requires --frames COUNT");
            if (seen.contains("--audio-copy") || seen.contains("--no-audio")) {
                throw std::invalid_argument("Audio options require MP4/MOV input");
            }
        }
        if (result.encode.input == "-") throw std::invalid_argument("Raw input must be a regular file; stdin is not supported");
        if (seen.contains("--bitrate") && seen.contains("--quality")) {
            throw std::invalid_argument("--bitrate and --quality are mutually exclusive");
        }
    } else if ((seen.contains("--codec") || seen.contains("--output-depth")) && result.command != Command::check_features) {
        throw std::invalid_argument("--codec and --output-depth require --check-features or an encoding source");
    }
    if (result.probe.codec == Codec::h264 && result.probe.bit_depth != 8) {
        throw std::invalid_argument("H.264 supports only 8-bit; use HEVC for 10-bit");
    }
    result.encode.video = result.probe;
    return result;
}

std::string_view help_text() {
    return R"(VTEnc — native Apple Silicon hardware encoder
Usage:
  vtenc --help
  vtenc --version
  vtenc --check-device
  vtenc --check-features [--codec h264|hevc] [--output-depth 8|10]
  vtenc -i input.mp4|input.mov -o output.mp4|output.mov
        [--codec h264|hevc] [--output-depth 8|10] [--audio-copy | --no-audio]
        [--bitrate KBIT/S | --quality 0..1] [--gop-len COUNT]
  vtenc (--synthetic | -i raw-file) -o output-file --input-res WIDTHxHEIGHT
        --frames COUNT [--codec h264|hevc] [--output-depth 8|10]
        [--fps NUM[/DEN]] [--bitrate KBIT/S | --quality 0..1] [--gop-len COUNT]

Encoding defaults: h264, 8-bit, 30/1 fps, 5000 kbit/s average target, GOP limit 60.
Input: tightly packed NV12 (8-bit) or P010 little-endian (10-bit in high bits),
video range, progressive BT.709 SDR. No pixel format or HDR conversion.
--frames is the exact number to read; trailing raw frames are not consumed.
Output: Annex B elementary stream plus <output-file>.timestamps.csv with rational
PTS/DTS/duration and byte offsets. Existing files are never overwritten.
P1 disables frame reordering. --quality is native VT Quality, not CRF/CQP.
SIGINT drains submitted frames and discards the incomplete output (exit 130).
Feature probing creates a hardware-only 1920x1080 VideoToolbox session.
Listed properties are not validated value combinations; the probe encodes no frames.
MP4/MOV: hardware-only decode and encode, first audio track copied by default.
Source dimensions, PTS, VFR and display transform are preserved. No depth conversion.
SDR progressive video only; HDR, Dolby Vision and interlaced input are rejected.
Exit codes: 0 success, 1 runtime/hardware failure, 2 invalid arguments, 130 cancelled.
)";
}

} // namespace vtenc
