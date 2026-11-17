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
        if (arg == "--avhw" || arg == "--avsw") {
            result.encode.decode = arg == "--avhw" ? DecodeMode::hardware : DecodeMode::software;
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
            arg != "--frames" && arg != "--gop-len" && arg != "--bitrate" && arg != "--quality" &&
            arg != "--output-res" && arg != "--crop" && arg != "--output-range" &&
            arg != "--input-matrix" && arg != "--output-matrix" && arg != "--input-chroma-loc" && arg != "--deinterlace" && arg != "--field-order") {
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
        else if (arg == "--deinterlace") {
            if (value != "bob") throw std::invalid_argument("--deinterlace supports only bob");
            result.encode.filter.bob = true;
        } else if (arg == "--field-order") {
            if (value == "auto") result.encode.filter.field_order = FieldOrder::automatic;
            else if (value == "tff") result.encode.filter.field_order = FieldOrder::tff;
            else if (value == "bff") result.encode.filter.field_order = FieldOrder::bff;
            else throw std::invalid_argument("--field-order requires auto, tff or bff");
        } else if (arg == "--output-range") {
            result.encode.filter.requested = true;
            if (value == "limited") result.encode.filter.range = ColorRange::limited;
            else if (value == "full") result.encode.filter.range = ColorRange::full;
            else throw std::invalid_argument("--output-range requires limited or full");
        } else if (arg == "--input-matrix" || arg == "--output-matrix") {
            result.encode.filter.requested = true;
            auto& matrix = arg == "--input-matrix" ? result.encode.filter.input_matrix : result.encode.filter.output_matrix;
            if (value == "bt601") matrix = ColorMatrix::bt601;
            else if (value == "bt709") matrix = ColorMatrix::bt709;
            else throw std::invalid_argument(std::string(arg) + " requires bt601 or bt709");
        } else if (arg == "--input-chroma-loc") {
            result.encode.filter.requested = true;
            if (value == "left") result.encode.filter.input_chroma = ChromaLocation::left;
            else if (value == "center") result.encode.filter.input_chroma = ChromaLocation::center;
            else if (value == "top-left") result.encode.filter.input_chroma = ChromaLocation::top_left;
            else throw std::invalid_argument("--input-chroma-loc requires left, center or top-left");
        } else if (arg == "--crop") {
            result.encode.filter.requested = true;
            auto rest = value;
            for (std::size_t n = 0; n < 4; ++n) {
                const auto split = rest.find(',');
                const auto part = rest.substr(0, split);
                int number = 0;
                const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), number);
                if (error != std::errc{} || end != part.data() + part.size() || number < 0 || number % 2 ||
                    (n < 3) != (split != std::string_view::npos)) {
                    throw std::invalid_argument("--crop requires four nonnegative even margins: LEFT,TOP,RIGHT,BOTTOM");
                }
                result.encode.filter.crop[n] = number;
                if (n < 3) rest.remove_prefix(split + 1);
            }
        } else if (arg == "--input-res" || arg == "--output-res") {
            const auto split = value.find('x');
            if (split == std::string_view::npos) throw std::invalid_argument(std::string(arg) + " requires WIDTHxHEIGHT");
            const int width = positive_integer(value.substr(0, split), arg);
            const int height = positive_integer(value.substr(split + 1), arg);
            if (width % 2 || height % 2) {
                throw std::invalid_argument(std::string(arg) + " requires even width and height for 4:2:0");
            }
            if (arg == "--input-res") { result.probe.width = width; result.probe.height = height; }
            else { result.encode.filter.output_size = {width, height}; result.encode.filter.requested = true; }
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
    if (seen.contains("--avhw") && seen.contains("--avsw")) {
        throw std::invalid_argument("--avhw and --avsw are mutually exclusive");
    }
    if (seen.contains("--field-order") && !result.encode.filter.bob) {
        throw std::invalid_argument("--field-order requires --deinterlace bob");
    }
    if (encode_options) {
        if (has_command) throw std::invalid_argument("Encoding options cannot be combined with a query command");
        result.command = Command::encode;
        if (synthetic == !result.encode.input.empty()) {
            throw std::invalid_argument("Encoding requires exactly one source: --synthetic or -i input-file");
        }
        if (result.encode.output.empty() || result.encode.output == "-") {
            throw std::invalid_argument("Encoding requires -o output-file (stdout is not supported)");
        }
        const auto extension = [](const std::string& path) {
            auto ext = std::filesystem::path(path).extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        };
        // Compressed input is probed by avformat, independent of its extension.
        // --input-res explicitly selects the existing tightly packed raw path.
        if (!synthetic && !seen.contains("--input-res")) {
            result.command = Command::transcode;
            const auto output_ext = extension(result.encode.output);
            if (output_ext != ".mp4" && output_ext != ".mov") {
                throw std::invalid_argument("Container transcoding requires an .mp4 or .mov output");
            }
            if (seen.contains("--frames") || seen.contains("--fps")) {
                throw std::invalid_argument("Container transcoding reads source dimensions/timing; --input-res, --frames and --fps are raw-only");
            }
            if (seen.contains("--audio-copy") && seen.contains("--no-audio")) {
                throw std::invalid_argument("--audio-copy and --no-audio are mutually exclusive");
            }
        } else {
            if (seen.contains("--avhw") || seen.contains("--avsw")) {
                throw std::invalid_argument("--avhw and --avsw require compressed input (without --input-res)");
            }
            if (result.encode.filter.requested) throw std::invalid_argument("GPU filters require compressed input");
            if (!seen.contains("--input-res")) throw std::invalid_argument("Encoding requires --input-res WIDTHxHEIGHT");
            if (result.encode.filter.bob) {
                if (synthetic) throw std::invalid_argument("Bob requires file input, not --synthetic");
                if (result.encode.filter.field_order == FieldOrder::automatic) throw std::invalid_argument("Raw Bob requires --field-order tff|bff");
                if (result.encode.frames > std::numeric_limits<std::int32_t>::max() / 2 ||
                    (result.encode.fps_den % 2 && result.encode.fps_num > std::numeric_limits<std::int32_t>::max() / 2)) {
                    throw std::invalid_argument("Raw Bob doubled frame count or frame-rate timescale exceeds 32-bit range");
                }
                if (result.probe.height % 4) throw std::invalid_argument("Raw Bob requires height divisible by four for interlaced 4:2:0");
            }
            if (!seen.contains("--frames")) throw std::invalid_argument("Encoding requires --frames COUNT");
            if (seen.contains("--audio-copy") || seen.contains("--no-audio")) {
                throw std::invalid_argument("Audio options require compressed input");
            }
        }
        if (result.command == Command::encode && result.encode.input == "-") throw std::invalid_argument("Raw input must be a regular file; stdin is not supported");
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
  vtenc -i input-file -o output.mp4|output.mov
        [--avhw | --avsw]
        [--codec h264|hevc] [--output-depth 8|10] [--audio-copy | --no-audio]
        [--crop LEFT,TOP,RIGHT,BOTTOM] [--output-res WIDTHxHEIGHT]
        [--output-range limited|full] [--output-matrix bt601|bt709]
        [--input-matrix bt601|bt709] [--input-chroma-loc left|center|top-left]
        [--deinterlace bob] [--field-order auto|tff|bff]
        [--bitrate KBIT/S | --quality 0..1] [--gop-len COUNT]
  vtenc (--synthetic | -i raw-file) -o output-file --input-res WIDTHxHEIGHT
        --frames COUNT [--codec h264|hevc] [--output-depth 8|10]
        [--fps NUM[/DEN]] [--bitrate KBIT/S | --quality 0..1] [--gop-len COUNT]

Encoding defaults: h264, 8-bit, 30/1 fps, 5000 kbit/s average target, GOP limit 60.
Input: tightly packed NV12 (8-bit) or P010 little-endian (10-bit in high bits),
video range, BT.709 SDR. No raw pixel format or HDR conversion.
Raw Bob: add --deinterlace bob --field-order tff|bff; --fps and --frames describe
input frame pairs, output has twice the frame count/rate. Interleaved fields in
both planes, left chroma siting per field, height divisible by four.
--frames is the exact number to read; trailing raw frames are not consumed.
Output: Annex B elementary stream plus <output-file>.timestamps.csv with rational
PTS/DTS/duration and byte offsets. Existing files are never overwritten.
P1 disables frame reordering. --quality is native VT Quality, not CRF/CQP.
SIGINT drains submitted frames and discards the incomplete output (exit 130).
Feature probing creates a hardware-only 1920x1080 VideoToolbox session.
Listed properties are not validated value combinations; the probe encodes no frames.
Compressed input: avformat probes any format; hardware-only encode.
MP4/MOV output, first audio track copied by default.
--input-res selects tightly packed raw input instead of avformat.
--avhw: avformat demuxing, require hardware decoding; any hardware decode failure is fatal.
--avsw: use FFmpeg libavformat demuxing and libavcodec software decoding.
Default: try hardware decoding, then restart once with software decoding on a
replayable input after a decoder failure. Non-seekable input cannot be restarted;
select --avsw in advance when hardware decoding is unavailable. Encoder, filter,
demux and output failures never trigger a retry.
Video PTS, VFR and display orientation are preserved. If the final frame duration
is absent, the preceding display interval is used with a diagnostic (single-frame
input without duration fails). Audio timestamp rounding to the output sampling
clock is reported and bounded to half an audio tick. GPU filters use Metal only.
Crop uses even margins in stored pixels, then bilinear resize to even output size.
Output pixel aspect ratio is preserved; explicit output size may stretch the image.
8/10-bit and range conversion are supported; depth follows --output-depth (default 8).
Matrix conversion changes YCbCr coefficients only, preserving primaries and transfer.
Missing matrix/chroma metadata requires an explicit input override when needed.
No gamut/transfer conversion, tone mapping, dithering or antialiasing downscale.
SDR only; interlaced input requires explicit --deinterlace bob (Metal, two outputs
per input). Auto field order requires unambiguous metadata; tff/bff overrides it.
Bob requires height and vertical crop margins divisible by four, keeps telecine
repeated fields, and is not inverse telecine. Software decoding accepts 8/10-bit
4:2:0 video; other decoded pixel layouts are rejected.
HDR and Dolby Vision are rejected.
Exit codes: 0 success, 1 runtime/hardware failure, 2 invalid arguments, 130 cancelled.
)";
}

} // namespace vtenc
