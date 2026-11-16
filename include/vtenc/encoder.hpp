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
};

// Hardware only. Publishes Annex B and <output>.timestamps.csv on success.
// On cancellation drains submitted frames, discards temporary files, and returns cancelled.
[[nodiscard]] EncodeResult encode_video(const EncodeConfig& config,
    const volatile std::sig_atomic_t& cancelled);

[[nodiscard]] EncodeResult transcode_file(const EncodeConfig& config,
    const volatile std::sig_atomic_t& cancelled);

} // namespace vtenc
