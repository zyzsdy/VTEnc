#pragma once
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "cf_ref.hpp"
namespace vtenc {
// Reads public codec configuration records; rejects unsupported chroma/depth.
[[nodiscard]] OSType source_pixel_format(CMFormatDescriptionRef format);
[[nodiscard]] CFRef<CMVideoFormatDescriptionRef> hardware_decode_format(CMFormatDescriptionRef format);
}
