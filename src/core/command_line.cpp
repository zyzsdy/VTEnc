#include "vtenc/command_line.hpp"
#include "vtenc/colorspace.hpp"
#include "vtenc/subtitle.hpp"

#include <charconv>
#include <fstream>
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
double positive_seconds(std::string_view value, std::string_view option) {
    double number = 0;
    std::istringstream parser{std::string(value)};
    parser.imbue(std::locale::classic());
    parser >> std::noskipws >> number;
    if (!parser || parser.peek() != std::char_traits<char>::eof() || !std::isfinite(number) || number <= 0 || number > 60)
        throw std::invalid_argument(std::string(option) + " requires finite seconds in (0,60]");
    return number;
}
void parse_keyfile(std::string_view filename, std::vector<std::int64_t>& frames) {
    std::ifstream input{std::string(filename)};
    if (!input) throw std::invalid_argument("Cannot open --keyfile: " + std::string(filename));
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto last = line.find_last_not_of(" \t\r");
        const auto token = std::string_view(line).substr(first, last - first + 1);
        std::int64_t number = -1;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), number);
        if (error != std::errc{} || end != token.data() + token.size() || number < 0 ||
            (!frames.empty() && number <= frames.back()) || frames.size() >= 1'000'000)
            throw std::invalid_argument("--keyfile requires strictly increasing nonnegative frame indices (one per line; max 1000000), line=" + std::to_string(line_number));
        frames.push_back(number);
    }
    if (input.bad()) throw std::runtime_error("Failed reading --keyfile: " + std::string(filename));
    if (frames.empty()) throw std::invalid_argument("--keyfile contains no frame indices");
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
    std::optional<FieldOrder> yadif_order;
    bool has_command = false;
    bool encode_options = false;
    bool synthetic = false;
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];
        if (arg == "-h") arg = "--help";
        if (arg == "--input") arg = "-i";
        if (arg == "--output") arg = "-o";
        if (arg == "-u") arg = "--preset";
        if (arg == "-f") arg = "--output-format";
        if (arg != "--vpp-subburn" && !seen.insert(arg).second) throw std::invalid_argument("Duplicate option: " + std::string(arg));
        if (arg == "--help" || arg == "--version" || arg == "--check-device" || arg == "--check-features") {
            if (has_command) throw std::invalid_argument("Specify exactly one command");
            has_command = true;
            if (arg == "--help") {
                result.command = Command::help;
                if (i + 1 < args.size() && !args[i + 1].starts_with("-")) {
                    result.help_keyword = args[++i];
                    if (result.help_keyword.empty()) throw std::invalid_argument("--help keyword must not be empty");
                }
            }
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
        if (arg == "--vpp-yadif") {
            result.encode.filter.yadif.enabled = true;
            encode_options = true;
            if (i + 1 < args.size() && !args[i + 1].starts_with("-")) {
                const auto params = args[++i];
                if (!params.starts_with("mode=")) throw std::invalid_argument("--vpp-yadif requires mode=auto|tff|bff|bob|bob_tff|bob_bff");
                const auto mode = params.substr(5);
                if (mode != "auto" && mode != "tff" && mode != "bff" && mode != "bob" && mode != "bob_tff" && mode != "bob_bff")
                    throw std::invalid_argument("Invalid --vpp-yadif mode; use auto, tff, bff, bob, bob_tff or bob_bff");
                result.encode.filter.yadif.bob = mode.starts_with("bob");
                if (mode.ends_with("tff")) yadif_order = FieldOrder::tff;
                if (mode.ends_with("bff")) yadif_order = FieldOrder::bff;
            }
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
            arg != "--cbr" && arg != "--vbr" && arg != "--max-bitrate" && arg != "--bitrate-window" &&
            arg != "--vbv-bufsize" && arg != "--preset" && arg != "--keyfile" &&
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
        else if (arg == "--keyfile") parse_keyfile(value, result.encode.force_keyframes);
        else if (arg == "--preset") {
            if (value != "default" && value != "quality" && value != "performance")
                throw std::invalid_argument("--preset requires default, performance or quality; NVENC P1-P7 are not VideoToolbox presets");
            if (value != "default") result.encode.prioritize_speed = value == "performance";
        } else if (arg == "--bitrate-window") result.encode.bitrate_window = positive_seconds(value, arg);
        else if (arg == "--vbv-bufsize") result.encode.vbv_buffer_bits = static_cast<std::int64_t>(positive_integer(value, arg)) * 1000;
        else if (arg == "--bitrate" || arg == "--cbr" || arg == "--vbr" || arg == "--max-bitrate") {
            const auto kbps = positive_integer(value, arg);
            if (kbps > std::numeric_limits<std::int32_t>::max() / 1000) {
                throw std::invalid_argument(std::string(arg) + " exceeds 2147483 kbit/s");
            }
            if (arg == "--max-bitrate") result.encode.max_bitrate = kbps * 1000;
            else {
                result.encode.bitrate = kbps * 1000;
                result.encode.rate_control = arg == "--cbr" ? RateControl::constant : arg == "--vbr" ? RateControl::variable : RateControl::average;
            }
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
    if (yadif_order) {
        if (seen.contains("--field-order") && result.encode.filter.field_order != FieldOrder::automatic &&
            result.encode.filter.field_order != *yadif_order)
            throw std::invalid_argument("--vpp-yadif mode conflicts with --field-order");
        result.encode.filter.field_order = *yadif_order;
    }
    if (result.encode.filter.yadif.enabled && (result.encode.filter.bob || result.encode.filter.ivtc.enabled))
        throw std::invalid_argument("--vpp-yadif, --vpp-ivtc and --deinterlace bob are mutually exclusive");
    if (result.encode.filter.ivtc.enabled && result.encode.filter.bob)
        throw std::invalid_argument("--vpp-ivtc and --deinterlace bob are mutually exclusive");
    if (seen.contains("--field-order") && !result.encode.filter.bob && !result.encode.filter.ivtc.enabled && !result.encode.filter.yadif.enabled) {
        throw std::invalid_argument("--field-order requires --deinterlace bob, --vpp-ivtc or --vpp-yadif");
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
            if (result.encode.filter.yadif.enabled) throw std::invalid_argument("--vpp-yadif requires avformat input; use Y4M with --avsw for raw samples");
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
        if (seen.count("--bitrate") + seen.count("--quality") + seen.count("--cbr") + seen.count("--vbr") > 1)
            throw std::invalid_argument("--bitrate, --quality, --cbr and --vbr are mutually exclusive");
        if (result.encode.max_bitrate && (result.encode.quality || result.encode.rate_control == RateControl::constant))
            throw std::invalid_argument("--max-bitrate requires ABR or native VBR; incompatible with --quality and --cbr");
        if (result.encode.max_bitrate && *result.encode.max_bitrate < result.encode.bitrate)
            throw std::invalid_argument("--max-bitrate must be at least the bitrate target");
        if (seen.contains("--bitrate-window") && (!result.encode.max_bitrate || result.encode.rate_control != RateControl::average))
            throw std::invalid_argument("--bitrate-window requires ABR with --max-bitrate; incompatible with native VBR/VBV");
        if (result.encode.vbv_buffer_bits && (result.encode.rate_control != RateControl::variable || !result.encode.max_bitrate))
            throw std::invalid_argument("--vbv-bufsize requires --vbr and explicit --max-bitrate");
        if (result.encode.rate_control == RateControl::variable) {
            result.encode.vbv_maxrate = result.encode.max_bitrate;
            result.encode.max_bitrate.reset(); // Never send DataRateLimits in VBR mode.
            if (result.encode.vbv_buffer_bits) {
                result.encode.vbv_duration = static_cast<double>(*result.encode.vbv_buffer_bits) / *result.encode.vbv_maxrate;
                if (*result.encode.vbv_duration > 60) throw std::invalid_argument("--vbv-bufsize / --max-bitrate exceeds 60 seconds");
            }
        }
        if (result.command == Command::transcode && result.encode.rate_control == RateControl::constant && result.encode.sync == SyncMode::vfr)
            throw std::invalid_argument("--cbr requires --avsync cfr/forcecfr and explicit --fps for avformat input");
        const auto output_frames = static_cast<std::int64_t>(result.encode.frames) * (result.encode.filter.bob ? 2 : 1);
        if (result.command == Command::encode && !result.encode.force_keyframes.empty() && result.encode.force_keyframes.back() >= output_frames)
            throw std::invalid_argument("--keyfile index exceeds the output frame count");
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

} // namespace vtenc
