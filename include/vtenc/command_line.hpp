#pragma once

#include <array>
#include <vector>
#include <span>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vtenc {

enum class Command { help, version, check_device, check_features, encode, transcode };
enum class Codec { h264, hevc };
enum class RateControl { average, constant, variable };
enum class SyncMode { vfr, cfr, forcecfr };
enum class DecodeMode { automatic, hardware, software };

struct ProbeConfig {
    Codec codec = Codec::h264;
    int bit_depth = 8;
    // A probe describes this configuration, not the encoder's maximum resolution.
    int width = 1920;
    int height = 1080;
};

enum class FieldOrder { automatic, tff, bff };

// H.273 code points where possible; automatic is never sent to the shader.
enum class ColorMatrix { automatic = 2, bt709 = 1, bt601 = 6, smpte240m = 7, bt2020nc = 9 };
enum class ColorTransfer { automatic = 2, bt709 = 1, linear = 8, srgb = 13, pq = 16, hlg = 18 };
enum class ColorPrimaries { automatic = 2, bt709 = 1, bt470bg = 5, smpte170m = 6,
                           bt2020 = 9, dci_p3 = 11, display_p3 = 12 };
enum class ToneMap : unsigned { none, hable, mobius, reinhard, bt2390 };
// Scalar-only layout also passed to Metal; keep in sync with ToneMapParameters.
struct ToneMapConfig {
    ToneMap mode = ToneMap::none;
    float source_peak = 1000.0F, ldr_nits = 100.0F;
    float desat_base = 0.18F, desat_strength = 0.75F, desat_exp = 1.5F;
    std::array<float, 6> hable = {0.22F, 0.3F, 0.1F, 0.2F, 0.01F, 0.3F};
    float transition = 0.3F, peak = 1.0F, contrast = 0.5F;
};
struct ColorspaceConfig {
    bool enabled = false;
    ToneMapConfig tonemap;
    ColorTransfer input_transfer = ColorTransfer::automatic, output_transfer = ColorTransfer::automatic;
    ColorPrimaries input_primaries = ColorPrimaries::automatic, output_primaries = ColorPrimaries::automatic;
    bool scene_ref = false; // NVEnc defaults to display-referred BT.1886 for BT.709.
    float sdr_source_peak = 100.0F; // cd/m2 represented by linear 1.0, as in NVEnc.
};
enum class ColorRange { automatic, limited, full };
enum class ChromaLocation { automatic, left, center, top_left };

enum class RffExpansion { automatic, on, off };
struct IvtcConfig {
    RffExpansion expand = RffExpansion::automatic;
    bool mixed = false;
    bool enabled = false;
    bool log = false;
    int cycle = 5; // 0: field matching only; 5: hard 3:2 telecine.
    int post = 2, nt = 10, cthresh = 4, combpel = 8, dthresh = 7;
};

struct YadifConfig {
    bool enabled = false;
    bool bob = false; // One output per field instead of per display raster.
};

struct FilterConfig {
    YadifConfig yadif;
    IvtcConfig ivtc;
    bool bob = false;
    FieldOrder field_order = FieldOrder::automatic;
    std::optional<std::array<int, 2>> output_size;
    std::array<int, 4> crop{}; // left, top, right, bottom in stored luma pixels.
    ColorRange range = ColorRange::automatic;
    ColorRange input_range = ColorRange::automatic;
    ColorspaceConfig colorspace;
    ColorMatrix input_matrix = ColorMatrix::automatic;
    ColorMatrix output_matrix = ColorMatrix::automatic;
    ChromaLocation input_chroma = ChromaLocation::automatic;
    bool requested = false;
};

struct SubtitleConfig {
    std::string filename, fontsdir, fontfile;
    std::string font = "Arial"; // Explicit fallback family; selections are diagnosed.
    std::int64_t offset_ms = 0; // Positive delays subtitles on the output PTS clock.
    float opacity = 1.0F, white_nits = 203.0F;
    std::optional<std::array<int, 4>> clip; // x:y:width:height in final stored pixels.
};
struct ColorTags {
    ColorMatrix matrix = ColorMatrix::automatic;
    ColorTransfer transfer = ColorTransfer::automatic;
    ColorPrimaries primaries = ColorPrimaries::automatic;
    bool enabled = false;
};
enum class HdrMetadataMode { automatic, clear, preserve };
struct HdrMetadataConfig {
    HdrMetadataMode mode = HdrMetadataMode::automatic;
    // HEVC SEI wire units: Gx,Gy,Bx,By,Rx,Ry,Wx,Wy,maxL,minL; MaxCLL,MaxFALL.
    std::optional<std::array<std::uint32_t, 10>> mastering;
    std::optional<std::array<std::uint16_t, 2>> light;
};

class StatusReporter;

struct EncodeConfig {
    StatusReporter* status = nullptr; // Borrowed scheduler observer; never used in C callbacks.
    ProbeConfig video;
    DecodeMode decode = DecodeMode::automatic;
    FilterConfig filter;
    std::vector<SubtitleConfig> subtitles;
    ColorTags color_tags;
    HdrMetadataConfig hdr;
    std::string input; // Empty: synthetic. Raw jobs: NV12/P010 LE. Compressed jobs: avformat input.
    std::string output;
    std::string output_format; // Empty: avformat guesses from the destination filename.
    std::int32_t frames = 120;
    std::int32_t fps_num = 30;
    std::int32_t fps_den = 1;
    std::int32_t gop = 60;
    std::int32_t bitrate = 5'000'000; // bits/s; unused when quality is set.
    RateControl rate_control = RateControl::average;
    std::optional<std::int32_t> max_bitrate; // ABR DataRateLimits, bits/s.
    double bitrate_window = 1.0; // Sliding window in seconds.
    std::optional<std::int32_t> vbv_maxrate; // Native VBR only, bits/s.
    std::optional<std::int64_t> vbv_buffer_bits; // CLI --vbv-bufsize kbit converted to bits.
    std::optional<double> vbv_duration; // Native VBR only, seconds.
    std::optional<bool> prioritize_speed; // Unset leaves the encoder default.
    std::vector<std::int64_t> force_keyframes; // Zero-based output frame indices after filters/CFR.
    std::optional<double> quality;
    SyncMode sync = SyncMode::vfr;
    std::optional<std::array<int, 2>> dar; // Display aspect after orthogonal rotation.
    bool audio_only = false;
    bool aac_audio = false;
    int audio_bitrate = 192000;
    int audio_sample_rate = 0; // Preserve source rate; only 44100/48000 accepted for AAC.
    bool copy_audio = true; // Container jobs copy the first audio track, when present.
};

struct CommandLine {
    bool json_status = false;
    Command command = Command::help;
    std::string help_keyword;
    ProbeConfig probe;
    EncodeConfig encode;
};

[[nodiscard]] CommandLine parse_command_line(std::span<const std::string_view> args);
[[nodiscard]] std::string_view codec_name(Codec codec);
[[nodiscard]] std::string help_text(std::string_view keyword = {});

} // namespace vtenc
