#pragma once

#include "vtenc/command_line.hpp"

#include <csignal>
#include <cstdint>

namespace vtenc {

struct EncodeResult {
    std::int64_t frames;
    std::uint64_t bytes;
    bool cancelled;
    std::int64_t audio_samples = 0;
    std::int64_t gpu_frames = 0;
    DecodeMode decoder = DecodeMode::hardware;
    bool timestamps_csv = false;
};

// Hardware only. Muxer selected by filename/override; raw elementary file output also publishes a timestamps CSV.
// On cancellation drains submitted frames and discards temporary files. Stdout cannot be retracted.
[[nodiscard]] EncodeResult encode_video(const EncodeConfig& config,
    const volatile std::sig_atomic_t& cancelled);

[[nodiscard]] EncodeResult transcode_file(const EncodeConfig& config,
    const volatile std::sig_atomic_t& cancelled);

} // namespace vtenc
