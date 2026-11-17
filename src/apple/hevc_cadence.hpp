#pragma once
#include "frame_cadence.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace vtenc {
// Metadata-only, single-layer H.265 parser. Input packets contain one complete
// picture/access unit. No FFmpeg private ABI or software decoding is involved.
class HevcCadenceParser {
public:
    explicit HevcCadenceParser(std::span<const std::uint8_t> extradata);
    ~HevcCadenceParser();
    [[nodiscard]] FrameCadence parse(std::span<const std::uint8_t> packet);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Distinguishes actual separately coded fields from soft display-field flags.
[[nodiscard]] bool hevc_field_sequence(std::span<const std::uint8_t> sps_nal);
}
