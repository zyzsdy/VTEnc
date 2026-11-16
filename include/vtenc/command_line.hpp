#pragma once

#include <span>
#include <string_view>

namespace vtenc {

enum class Command { help, version, check_device, check_features };
enum class Codec { h264, hevc };

struct ProbeConfig {
    Codec codec = Codec::h264;
    int bit_depth = 8;
    // A probe describes this configuration, not the encoder's maximum resolution.
    int width = 1920;
    int height = 1080;
};

struct CommandLine {
    Command command = Command::help;
    ProbeConfig probe;
};

[[nodiscard]] CommandLine parse_command_line(std::span<const std::string_view> args);
[[nodiscard]] std::string_view codec_name(Codec codec);
[[nodiscard]] std::string_view help_text();

} // namespace vtenc
