#include "vtenc/command_line.hpp"
#include "vtenc/colorspace.hpp"
#include "vtenc/subtitle.hpp"

#include <charconv>
#include <cmath>
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
void parse_ivtc(std::string_view value, IvtcConfig& config) {
    if (value.empty()) throw std::invalid_argument("Empty --vpp-ivtc parameter");
    std::set<std::string_view> keys;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto pair = value.substr(0, comma);
        const auto equal = pair.find('=');
        if (equal == std::string_view::npos) throw std::invalid_argument("--vpp-ivtc requires key=value pairs");
        const auto key = pair.substr(0, equal), text = pair.substr(equal + 1);
        if (!keys.insert(key).second) throw std::invalid_argument("Duplicate --vpp-ivtc parameter: " + std::string(key));
        if (key == "log" || key == "mixed" || key == "expand") {
            if (key == "expand") {
                if (text == "auto") config.expand = RffExpansion::automatic;
                else if (text == "on") config.expand = RffExpansion::on;
                else if (text == "off") config.expand = RffExpansion::off;
                else throw std::invalid_argument("--vpp-ivtc expand requires auto, on or off");
            } else {
                if (text != "true" && text != "false") throw std::invalid_argument("--vpp-ivtc " + std::string(key) + " requires true or false");
                (key == "log" ? config.log : config.mixed) = text == "true";
            }
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
            if (value.empty()) throw std::invalid_argument("Empty --vpp-ivtc parameter");
            continue;
        }
        int* target = nullptr;
        if (key == "cycle") target = &config.cycle;
        else if (key == "post") target = &config.post;
        else if (key == "nt") target = &config.nt;
        else if (key == "cthresh") target = &config.cthresh;
        else if (key == "combpel") target = &config.combpel;
        else if (key == "dthresh") target = &config.dthresh;
        else throw std::invalid_argument("Unsupported --vpp-ivtc parameter: " + std::string(key));
        int number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{} || end != text.data() + text.size() || number < 0 || number > 255)
            throw std::invalid_argument("--vpp-ivtc values must be integers in 0..255");
        *target = number;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
        if (value.empty()) throw std::invalid_argument("Empty --vpp-ivtc parameter");
    }
    if (config.cycle != 0 && config.cycle != 5) throw std::invalid_argument("--vpp-ivtc cycle must be 0 or 5");
    if (config.post != 0 && config.post != 2) throw std::invalid_argument("--vpp-ivtc post must be 0 or 2");
    if (config.combpel < 1 || config.combpel > 128) throw std::invalid_argument("--vpp-ivtc combpel must be in 1..128");
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
        if (arg == "-f") arg = "--output-format";
        if (arg != "--vpp-subburn" && !seen.insert(arg).second) throw std::invalid_argument("Duplicate option: " + std::string(arg));
        if (arg == "--help" || arg == "--version" || arg == "--check-device" || arg == "--check-features") {
            if (has_command) throw std::invalid_argument("Specify exactly one command");
            has_command = true;
            if (arg == "--help") result.command = Command::help;
            else if (arg == "--version") result.command = Command::version;
            else if (arg == "--check-device") result.command = Command::check_device;
            else result.command = Command::check_features;
            continue;
        }
        if (arg == "--json-status") { result.json_status = true; continue; }
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
        if (arg == "--vpp-ivtc") {
            result.encode.filter.ivtc.enabled = true;
            encode_options = true;
            if (i + 1 < args.size() && !args[i + 1].starts_with("-")) parse_ivtc(args[++i], result.encode.filter.ivtc);
            continue;
        }
        const bool common = arg == "--codec" || arg == "--output-depth";
        if (!common && arg != "--output-format" && arg != "-i" && arg != "-o" && arg != "--input-res" && arg != "--fps" &&
            arg != "--avsync" && arg != "--dar" && arg != "--audio-codec" && arg != "--audio-bitrate" && arg != "--audio-samplerate" &&
            arg != "--frames" && arg != "--gop-len" && arg != "--bitrate" && arg != "--quality" &&
            arg != "--vpp-subburn" && arg != "--color-tags" && arg != "--hdr-metadata" && arg != "--master-display" && arg != "--max-cll" && arg != "--vpp-colorspace" && arg != "--output-res" && arg != "--crop" && arg != "--output-range" &&
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
        else if (arg == "--output-format") {
            if (value.starts_with("-")) throw std::invalid_argument("Missing value for --output-format");
            result.encode.output_format = value;
        } else if (arg == "--vpp-subburn") {
            if (result.encode.subtitles.size() >= 8) throw std::invalid_argument("At most eight subtitle layers are supported");
            result.encode.subtitles.push_back(parse_subburn(value));
        } else if (arg == "--color-tags") {
            parse_color_tags(value, result.encode.color_tags);
        } else if (arg == "--hdr-metadata") {
            if (value == "auto") result.encode.hdr.mode = HdrMetadataMode::automatic;
            else if (value == "clear") result.encode.hdr.mode = HdrMetadataMode::clear;
            else if (value == "preserve") result.encode.hdr.mode = HdrMetadataMode::preserve;
            else throw std::invalid_argument("--hdr-metadata requires auto, clear or preserve");
        } else if (arg == "--master-display" || arg == "--max-cll") {
            const auto count = arg == "--master-display" ? 10U : 2U;
            std::array<std::uint32_t, 10> numbers{}; auto rest = value;
            for (unsigned n=0; n<count; ++n) {
                auto comma=rest.find(','); auto part=rest.substr(0,comma);
                auto [end,ec]=std::from_chars(part.data(),part.data()+part.size(),numbers[n]);
                const auto limit = count==2 ? 65535U : n<8 ? 50000U : 100000000U;
                if (ec!=std::errc{}||end!=part.data()+part.size()||numbers[n]>limit||((n+1<count)!=(comma!=std::string_view::npos)))
                    throw std::invalid_argument("Invalid HDR metadata integer list: " + std::string(arg));
                if (n+1<count) rest.remove_prefix(comma+1);
            }
            if (count==10) {
                if (!numbers[8] || numbers[9]>=numbers[8]) throw std::invalid_argument("Mastering max luminance must exceed min luminance");
                for (unsigned n=0;n<8;n+=2) if (numbers[n]+numbers[n+1]>50000 || !numbers[n+1])
                    throw std::invalid_argument("Invalid mastering chromaticity coordinates");
                result.encode.hdr.mastering=numbers;
            } else {
                if (numbers[0] && numbers[1]>numbers[0]) throw std::invalid_argument("MaxFALL exceeds MaxCLL");
                result.encode.hdr.light=std::array<std::uint16_t,2>{static_cast<std::uint16_t>(numbers[0]),static_cast<std::uint16_t>(numbers[1])};
            }
        } else if (arg == "--vpp-colorspace") {
            parse_colorspace(value, result.encode.filter);
        } else if (arg == "--deinterlace") {
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
            else if (value == "bt2020nc" || value == "bt2020") matrix = ColorMatrix::bt2020nc;
            else if (value == "smpte240m") matrix = ColorMatrix::smpte240m;
            else throw std::invalid_argument(std::string(arg) + " requires bt601, bt709, bt2020nc or smpte240m");
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
        } else if (arg == "--avsync") {
            if (value == "vfr") result.encode.sync = SyncMode::vfr;
            else if (value == "cfr") result.encode.sync = SyncMode::cfr;
            else if (value == "forcecfr") result.encode.sync = SyncMode::forcecfr;
            else throw std::invalid_argument("--avsync requires vfr, cfr or forcecfr");
        } else if (arg == "--dar") {
            const auto split = value.find(':');
            if (split == std::string_view::npos) throw std::invalid_argument("--dar requires NUM:DEN");
            auto n = positive_integer(value.substr(0, split), arg);
            auto d = positive_integer(value.substr(split + 1), arg);
            const auto gcd = std::gcd(n, d);
            result.encode.dar = {n / gcd, d / gcd};
        } else if (arg == "--audio-codec") {
            if (value != "aac") throw std::invalid_argument("--audio-codec supports only aac (AudioToolbox AAC-LC)");
            result.encode.aac_audio = true;
            result.encode.copy_audio = false;
        } else if (arg == "--audio-bitrate") {
            const auto kbps = positive_integer(value, arg);
            if (kbps > 2147483) throw std::invalid_argument("--audio-bitrate exceeds 2147483 kbit/s");
            result.encode.audio_bitrate = kbps * 1000;
        } else if (arg == "--audio-samplerate") {
            const auto rate = positive_integer(value, arg);
            if (rate != 44100 && rate != 48000) throw std::invalid_argument("--audio-samplerate requires 44100 or 48000 Hz");
            result.encode.audio_sample_rate = rate;
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
    if (seen.contains("--vpp-colorspace") && (seen.contains("--input-matrix") || seen.contains("--output-matrix") || seen.contains("--output-range"))) {
        throw std::invalid_argument("--vpp-colorspace cannot be combined with --input-matrix, --output-matrix or --output-range; use its matrix/range pairs");
    }
    if (seen.contains("--avhw") && seen.contains("--avsw")) {
        throw std::invalid_argument("--avhw and --avsw are mutually exclusive");
    }
    if (result.encode.filter.ivtc.enabled && result.encode.filter.bob)
        throw std::invalid_argument("--vpp-ivtc and --deinterlace bob are mutually exclusive");
    if (seen.contains("--field-order") && !result.encode.filter.bob && !result.encode.filter.ivtc.enabled) {
        throw std::invalid_argument("--field-order requires --deinterlace bob or --vpp-ivtc");
    }
    if (static_cast<int>(seen.contains("--audio-copy")) + static_cast<int>(seen.contains("--no-audio")) +
        static_cast<int>(seen.contains("--audio-codec")) > 1)
        throw std::invalid_argument("--audio-copy, --no-audio and --audio-codec are mutually exclusive");
    if ((seen.contains("--audio-bitrate") || seen.contains("--audio-samplerate")) && !result.encode.aac_audio)
        throw std::invalid_argument("--audio-bitrate/--audio-samplerate require --audio-codec aac");
    if (result.encode.hdr.mode == HdrMetadataMode::clear && (result.encode.hdr.mastering || result.encode.hdr.light))
        throw std::invalid_argument("--hdr-metadata clear conflicts with explicit HDR metadata");
    if (encode_options) {
        if (has_command) throw std::invalid_argument("Encoding options cannot be combined with a query command");
        result.command = Command::encode;
        if (synthetic == !result.encode.input.empty()) {
            throw std::invalid_argument("Encoding requires exactly one source: --synthetic or -i input-file");
        }
        if (result.encode.output.empty()) {
            throw std::invalid_argument("Encoding requires -o output-file");
        }
        if (result.encode.output == "-" && result.encode.output_format.empty()) {
            throw std::invalid_argument("-o - requires -f/--output-format to select the stdout muxer");
        }
        // Compressed input is probed by avformat, independent of its extension.
        // --input-res explicitly selects the existing tightly packed raw path.
        if (!synthetic && !seen.contains("--input-res")) {
            result.command = Command::transcode;
            if (seen.contains("--frames")) throw std::invalid_argument("--frames is raw-only");
            if (result.encode.sync != SyncMode::vfr && !seen.contains("--fps"))
                throw std::invalid_argument("--avsync cfr/forcecfr requires explicit --fps NUM[/DEN]");
            if (result.encode.sync == SyncMode::vfr && seen.contains("--fps"))
                throw std::invalid_argument("Container --fps requires --avsync cfr or forcecfr; otherwise it is raw-only");
            if (seen.contains("--audio-copy") && seen.contains("--no-audio")) {
                throw std::invalid_argument("--audio-copy and --no-audio are mutually exclusive");
            }
        } else {
            if (seen.contains("--avsync")) throw std::invalid_argument("--avsync requires compressed input; raw input already has a fixed rate");
            if (seen.contains("--avhw") || seen.contains("--avsw")) {
                throw std::invalid_argument("--avhw and --avsw require compressed input (without --input-res)");
            }
            if (result.encode.filter.ivtc.enabled) throw std::invalid_argument("--vpp-ivtc requires compressed input");
            if (!result.encode.subtitles.empty() || result.encode.color_tags.enabled || seen.contains("--hdr-metadata") || result.encode.hdr.mastering || result.encode.hdr.light)
                throw std::invalid_argument("Subtitle and output metadata options require avformat input (Y4M with --avsw is supported)");
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
            if (seen.contains("--audio-copy") || seen.contains("--no-audio") || result.encode.aac_audio) {
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
    if (result.json_status && (result.command == Command::help || result.command == Command::check_device))
        throw std::invalid_argument("--json-status requires encoding, --version or --check-features");
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
  vtenc -i input-file -o output-file [-f FORMAT | --output-format FORMAT]
        [--avhw | --avsw]
        [--codec h264|hevc] [--output-depth 8|10] [--audio-copy | --no-audio | --audio-codec aac]
        [--audio-bitrate KBIT/S] [--audio-samplerate 44100|48000]
        [--avsync vfr|cfr|forcecfr] [--fps NUM[/DEN]] [--dar NUM:DEN]
        [--crop LEFT,TOP,RIGHT,BOTTOM] [--output-res WIDTHxHEIGHT]
        [--output-range limited|full] [--output-matrix bt601|bt709|bt2020nc|smpte240m]
        [--input-matrix bt601|bt709|bt2020nc|smpte240m] [--input-chroma-loc left|center|top-left]
        [--vpp-colorspace KEY=VALUE[,KEY=VALUE]...]
        [--vpp-subburn filename=ASS[,fontsdir=DIR,fontfile=FONT,font=Arial,
                       ts_offset=SECONDS,opacity=0..1,white=NITS,clip=X:Y:W:H]]
        [--color-tags matrix=NAME,transfer=NAME,colorprim=NAME]
        [--hdr-metadata auto|clear|preserve] [--master-display INTEGER_LIST]
        [--max-cll MaxCLL,MaxFALL]
        [--deinterlace bob | --vpp-ivtc [PARAMS]] [--field-order auto|tff|bff]
        [--bitrate KBIT/S | --quality 0..1] [--gop-len COUNT]
  vtenc (--synthetic | -i raw-file) -o output-file --input-res WIDTHxHEIGHT
        --frames COUNT [-f FORMAT | --output-format FORMAT] [--codec h264|hevc] [--output-depth 8|10]
        [--fps NUM[/DEN]] [--dar NUM:DEN] [--bitrate KBIT/S | --quality 0..1] [--gop-len COUNT]

--json-status: versioned JSON Lines on stderr, including diagnostics and terminal
events; without it, live progress and all encoding logs also use stderr. Unknown
duration has no percent or ETA. --version/--check-features also accept this flag.
Y4M: use -i - --avsw, without --input-res; avformat auto-detects the stream.
Supported layouts: planar 4:2:0 8-bit and 10-bit little-endian. Missing color
interpretation requires --vpp-colorspace matrix/transfer/colorprim/range=FROM:auto
and, for C420p10 without chroma metadata, --input-chroma-loc left|center|top-left.
The caller must check producer exit status: clean EOF cannot reveal producer failure.
Encoding defaults: h264, 8-bit, 30/1 fps, 5000 kbit/s average target, GOP limit 60.
Input: tightly packed NV12 (8-bit) or P010 little-endian (10-bit in high bits),
video range, BT.709 SDR. No raw pixel format or HDR conversion.
Raw Bob: add --deinterlace bob --field-order tff|bff; --fps and --frames describe
input frame pairs, output has twice the frame count/rate. Interleaved fields in
both planes, left chroma siting per field, height divisible by four.
--frames is the exact number to read; trailing raw frames are not consumed.
Output: avformat selects the muxer from the filename; -f/--output-format overrides
the filename with an FFmpeg muxer name (e.g. matroska, mpegts, mp4). Unknown formats
and incompatible streams fail; no output-format whitelist. Existing files are never
overwritten. -o - streams to stdout and requires -f/--output-format; all job logs
go to stderr. The muxer must support non-seekable output. Muxers managing their own
IO (e.g. HLS/segment) are not supported. Raw/synthetic elementary file output also
writes a timestamps CSV; stdout never writes sidecar files.
P1 disables frame reordering. --quality is native VT Quality, not CRF/CQP.
SIGINT drains submitted frames (exit 130). Incomplete files are discarded; stdout
bytes already written cannot be retracted.
Feature probing creates a hardware-only 1920x1080 VideoToolbox session.
Listed properties are not validated value combinations; the probe encodes no frames.
Compressed input: avformat probes any format; hardware-only encode.
--avsync defaults to vfr. cfr/forcecfr require --fps; sample-and-hold on a grid
anchored at the first filtered video PTS, after Bob/IVTC. cfr clips the last
period to the source end; forcecfr extends it to a full period. Gaps hold the
previous picture; duplicate/backward PTS fail. Audio timestamps stay unchanged.
--dar sets display aspect after rotation, deriving SAR from final stored size;
only orthogonal unit rotation/reflection is accepted with a DAR override.
--audio-codec aac uses AudioToolbox AAC-LC, libavcodec audio decode and
libswresample; mono/stereo only, no downmix, source rate or explicit 44100/48000.
Default AAC bitrate is 192 kbit/s ABR; unsupported settings and missing audio fail.
AAC requires file output with verifiable priming/padding; stdout supports copy/none.
Movie AAC input requires a locally readable native packet table for exact trims.
First audio track copied by default; the selected output must accept its codec.
--input-res selects tightly packed raw input instead of avformat.
--avhw: avformat demuxing, require hardware decoding; any hardware decode failure is fatal.
--avsw: use FFmpeg libavformat demuxing and libavcodec software decoding.
Default: try hardware decoding, then restart once with software decoding on a
replayable input after a decoder failure. Non-seekable input cannot be restarted;
select --avsw in advance when hardware decoding is unavailable. Encoder, filter,
demux and output failures never trigger a retry. Once stdout muxing has started,
hardware decode failure is fatal: retrying would concatenate incomplete streams.
Video PTS/VFR are rescaled to the muxer time base; rounding is reported and limited
to half an output tick, and collapsed frame intervals fail. Metadata preservation
depends on the selected container. If the final frame duration is absent, the
preceding display interval is used with a diagnostic (single-frame input without
duration fails). Audio timestamp rounding to the output
time base is reported and bounded to half an audio tick. GPU filters use Metal only.
Crop uses even margins in stored pixels, then bilinear resize to even output size.
Output pixel aspect ratio is preserved; explicit output size may stretch the image.
8/10-bit and range conversion are supported; depth follows --output-depth (default 8).
Matrix conversion changes YCbCr coefficients only, preserving primaries and transfer.
Missing matrix/chroma metadata requires an explicit input override when needed.
--vpp-subburn is repeatable (up to 8, later layers on top). External UTF-8
ASS/SSA only. libass shapes/rasterizes glyph masks; Metal caches and composites
in display-linear RGB. Geometry/field processing/CFR precede subtitles. Layout
uses final stored dimensions and SAR; display rotation rotates subtitles too.
PTS is absolute output PTS (floor to milliseconds); positive ts_offset delays.
ASS clipping/layers are honored; clip adds a final-pixel rectangle. Quote paths
containing commas inside the argument. Fonts are validated; fallback selections
are diagnosed, missing glyphs fail. SDR follows video transfer/primaries and ASS
YCbCr Matrix; HDR subtitle colors are sRGB/BT.709 and white defaults
to 203 cd/m2, PQ is absolute and HLG uses a 1000-nit display / system gamma 1.2.
--color-tags changes only output matrix/transfer/primaries labels after filters;
it does not change pixel values. Range conversion remains --output-range.
HDR metadata auto preserves only applicable HDR10 values, clears stale values;
preserve errors if a transform invalidates values. Explicit values require HEVC
Main10 PQ/BT.2020 output. --master-display takes Gx,Gy,Bx,By,Rx,Ry,Wx,Wy,maxL,minL
in 0.00002 chromaticity / 0.0001 cd/m2 units, without guessing defaults.

--vpp-colorspace uses NVEnc-style matrix=FROM:TO,transfer=FROM:TO,
colorprim=FROM:TO,range=FROM:TO pairs. Source auto reads frame/track metadata;
destination auto preserves the resolved source (hdr2sdr defaults to BT.709).
Missing required source metadata fails.
Matrix: bt601 (smpte170m/bt470bg), bt709, smpte240m, bt2020nc (bt2020).
Transfer: bt709 (smpte170m/bt2020-10/bt2020-12), linear, srgb (iec61966-2-1),
smpte2084 (pq), arib-std-b67 (hlg). Primaries: bt709, smpte170m (smpte240m),
bt470bg, bt2020, display-p3 (smpte432), dci-p3 (smpte431). Range: limited/full.
All four pairs accept auto. scene_ref=true|false defaults false: BT.709 uses
BT.1886 display gamma 2.4; true uses the BT.709 scene OETF. sdr_source_peak=100
sets linear-light reference luminance in cd/m2; valid range [0.01,10000].
Cannot combine --vpp-colorspace with --input-matrix/--output-matrix/--output-range.
HDR input requires explicit --vpp-colorspace. hdr2sdr=none|hable|mobius|reinhard|
bt2390 enables display-referred PQ/HLG -> SDR tone mapping before gamut conversion.
Defaults: hdr2sdr=none, source_peak=1000, ldr_nits=100 (cd/m2), desat_base=0.18,
desat_strength=0.75, desat_exp=1.5. Source peak is explicit, not read from MaxCLL.
Hable a=0.22,b=0.3,c=0.1,d=0.2,e=0.01,f=0.3; Mobius transition=0.3,peak=1;
Reinhard contrast=0.5,peak=1. Curves accept only their own tuning parameters.
Require 1 <= ldr_nits <= source_peak <= 10000; desat_base/strength in [0,1],
desat_exp in [0.01,10], transition in [0,0.999], contrast in [0.001,0.999],
peak in [0.001,100] (Mobius peak*source_peak/ldr_nits >= 1).
Hable requires a,e in [0,10], b,d,f in [0.0001,10], e<f, e/f<=c<=1 and a
nonzero curve. hdr2sdr requires scene_ref=false and normalizes linear RGB to
ldr_nits; explicit sdr_source_peak must equal ldr_nits. Auto matrix/primaries/
transfer output becomes BT.709; range still inherits source. HDR output is rejected.
BT.2390 retains NVEnc's 203 reference-white scale; values above source_peak clamp
before the Hermite shoulder. Without hdr2sdr, HDR->SDR is clipping-only conversion.
No LUT3D, gamut compression, dithering or antialiasing downscale.
Out-of-range output clips at quantization; HDR mastering/content-light metadata
is removed after filtering. RGB and constant-luminance YCbCr are unsupported.
--vpp-ivtc performs Metal C/P/N field matching for hard telecine before other
filters. Parameters: cycle=5 (drop one per five) or 0 (match only), post=2
(adaptive residual repair) or 0, nt=10, cthresh=4, combpel=8, dthresh=7.
expand=auto|on|off (default auto) expands per-picture repeat fields in Metal.
mixed=true|false (default false) preserves progressive video and drops only
convincing clean duplicates in soft/hard telecine cycles; output may be VFR.
expand=off,mixed=true retains soft coded-picture intervals without expansion.
log=true writes per-frame decisions to stderr (default false).
Thresholds use 8-bit units; combpel is 1..128, other thresholds 0..255.
Full cycles retain their original time span; incomplete tails retain all frames.
Use --field-order tff|bff for hard rasters when metadata is missing or varies;
original RFF flags retain their per-picture field order. Mixed mode requires
per-picture metadata. Native H.264 parses SEI without software decoding.
HEVC parses SPS/PPS/VUI and picture-timing for both native and software decode,
including reordered pictures and in-band parameter sets (hvcC or Annex B).
HEVC single-field pictures (pic_struct=1/2/9..12) are paired in Metal before
RFF/IVTC, including when expand=off. Bitstream spatial parity takes precedence
over --field-order; previous/next complementary-field hints guide matching.
Unmatched fields are spatially reconstructed with diagnostics and retain their
own intervals; a missing final field duration is an error. Crop/resize uses the
assembled full-height raster. Missing required timing, reserved/contradictory
pic_struct and multilayer HEVC fail explicitly.
Raw IVTC input, cadence lock and other NVEnc IVTC parameters are unsupported.
Interlaced input requires explicit --vpp-ivtc or --deinterlace bob. Bob produces
two Metal outputs per input. Auto field order requires unambiguous metadata; tff/bff overrides it.
Bob requires height and vertical crop margins divisible by four, keeps telecine
repeated fields, and is not inverse telecine. Software decoding accepts 8/10-bit
4:2:0 video; other decoded pixel layouts are rejected.
Dolby Vision is rejected, including when --vpp-colorspace is enabled.
Exit codes: 0 success, 1 runtime/hardware failure, 2 invalid arguments, 130 cancelled.
)";
}

} // namespace vtenc
