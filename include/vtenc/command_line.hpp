#pragma once

#include <array>
#include <span>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vtenc {

enum class Command { help, version, check_device, check_features, encode, transcode };
enum class Codec { h264, hevc };
enum class DecodeMode { automatic, hardware, software };

struct ProbeConfig {
    Codec codec = Codec::h264;
    int bit_depth = 8;
    // A probe describes this configuration, not the encoder's maximum resolution.
    int width = 1920;
    int height = 1080;
};

enum class FieldOrder { automatic, tff, bff };

enum class ColorMatrix { automatic, bt601, bt709 };
enum class ColorRange { automatic, limited, full };
enum class ChromaLocation { automatic, left, center, top_left };

struct FilterConfig {
    bool bob = false;
    FieldOrder field_order = FieldOrder::automatic;
    std::optional<std::array<int, 2>> output_size;
    std::array<int, 4> crop{}; // left, top, right, bottom in stored luma pixels.
    ColorRange range = ColorRange::automatic;
    ColorMatrix input_matrix = ColorMatrix::automatic;
    ColorMatrix output_matrix = ColorMatrix::automatic;
    ChromaLocation input_chroma = ChromaLocation::automatic;
    bool requested = false;
};

struct EncodeConfig {
    ProbeConfig video;
    DecodeMode decode = DecodeMode::automatic;
    FilterConfig filter;
    std::string input; // Empty: synthetic. Raw jobs: NV12/P010 LE. Compressed jobs: avformat input.
    std::string output;
    std::string output_format; // Empty: avformat guesses from the destination filename.
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
