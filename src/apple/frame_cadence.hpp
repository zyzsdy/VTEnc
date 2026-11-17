#pragma once
#include "vtenc/command_line.hpp"

namespace vtenc {
enum class FieldPairing { unspecified, previous, next };
// Display-order information from the decoder/bitstream, independent of any
// --field-order interpretation override. repeat_fields follows AVFrame semantics.
struct FrameCadence {
    bool known = false;
    bool interlaced = false;
    int repeat_fields = 0; // 0, 1 (RFF), 2 (double frame), 4 (triple frame).
    FieldOrder first = FieldOrder::automatic;
    bool expanded = false; // Display raster assembled by the RFF stage.
    bool boundary = false; // Do not borrow fields or decimate across this point.
    bool single_field = false; // Packed half-height HEVC picture; first is spatial parity.
    FieldPairing pairing = FieldPairing::unspecified;
    bool duplicate = false; // Already coded/displayed, never an extra RFF interval.
    bool assembled_fields = false; // Spatial parity comes from the bitstream.
};
}
