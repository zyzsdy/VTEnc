#pragma once

#include <span>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vtenc {

enum class Command { help, version, check_device, check_features, encode, transcode };
enum class Codec { h264, hevc };

struct ProbeConfig {
    Codec codec = Codec::h264;
    int bit_depth = 8;
    // A probe describes this configuration, not the encoder's maximum resolution.
    int width = 1920;
    int height = 1080;
};

struct EncodeConfig {
    ProbeConfig video;
    std::string input; // Empty means synthetic input; otherwise tightly packed NV12/P010 LE.
    std::string output;
    std::int32_t frames = 120;
    std::int32_t fps_num = 30;
    std::int32_t fps_den = 1;
    std::int32_t gop = 60;
    std::int32_t bitrate = 5'000'000; // AverageBitRate, bits/s; unused when quality is set.
    std::optional<double> quality;
    bool copy_audio = true; // Container jobs copy the first audio track, when present.
};

struct CommandLine {
    Command command = Command::help;
    ProbeConfig probe;
    EncodeConfig encode;
};

[[nodiscard]] CommandLine parse_command_line(std::span<const std::string_view> args);
[[nodiscard]] std::string_view codec_name(Codec codec);
[[nodiscard]] std::string_view help_text();

} // namespace vtenc
