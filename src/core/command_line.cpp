#include "vtenc/command_line.hpp"

#include <stdexcept>
#include <string>

namespace vtenc {

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
    bool has_codec = false;
    bool has_depth = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--codec" || arg == "--output-depth") {
            auto& seen = arg == "--codec" ? has_codec : has_depth;
            if (seen) throw std::invalid_argument("Duplicate option: " + std::string(arg));
            seen = true;
            if (++i == args.size()) {
                throw std::invalid_argument("Missing value for " + std::string(arg));
            }
            const auto value = args[i];
            if (arg == "--codec") {
                if (value == "h264") result.probe.codec = Codec::h264;
                else if (value == "hevc") result.probe.codec = Codec::hevc;
                else throw std::invalid_argument("Supported probe codecs: h264, hevc");
            } else {
                if (value == "8") result.probe.bit_depth = 8;
                else if (value == "10") result.probe.bit_depth = 10;
                else throw std::invalid_argument("--output-depth must be 8 or 10");
            }
        } else {
            Command command;
            if (arg == "--help" || arg == "-h") command = Command::help;
            else if (arg == "--version") command = Command::version;
            else if (arg == "--check-device") command = Command::check_device;
            else if (arg == "--check-features") command = Command::check_features;
            else throw std::invalid_argument("Unknown argument: " + std::string(arg));
            if (has_command) throw std::invalid_argument("Specify exactly one command");
            has_command = true;
            result.command = command;
        }
    }
    if ((has_codec || has_depth) && result.command != Command::check_features) {
        throw std::invalid_argument("--codec and --output-depth require --check-features");
    }
    if (result.probe.codec == Codec::h264 && result.probe.bit_depth != 8) {
        throw std::invalid_argument("H.264 probe supports only 8-bit; use HEVC for 10-bit");
    }
    return result;
}

std::string_view help_text() {
    return R"(VTEnc — native Apple Silicon encoder development scaffold
Usage:
  vtenc --help
  vtenc --version
  vtenc --check-device
  vtenc --check-features [--codec h264|hevc] [--output-depth 8|10]

Feature probing creates a hardware-only 1920x1080 VideoToolbox session.
Defaults: h264, 8-bit. Listed properties are not validated value combinations.
No frames are encoded by this probe. File transcoding is not implemented yet.
Exit codes: 0 success, 1 runtime/hardware failure, 2 invalid arguments.
)";
}

} // namespace vtenc
