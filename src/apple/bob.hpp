#pragma once
#include "vtenc/command_line.hpp"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

namespace vtenc {
struct BobTime { CMTime pts; CMTime duration; };
// Splits a frame interval exactly; never rounds or normalizes the movie origin.
[[nodiscard]] BobTime bob_time(CMTime pts, CMTime duration, unsigned field_index);
[[nodiscard]] std::int32_t bob_timescale(std::int32_t source);
// Explicit order overrides metadata; auto needs an unambiguous full interleaved frame.
[[nodiscard]] unsigned bob_first_field(FieldOrder requested, CMFormatDescriptionRef format, CVPixelBufferRef pixel);
[[nodiscard]] EncodeConfig raw_bob_output_config(const EncodeConfig& source);
}
