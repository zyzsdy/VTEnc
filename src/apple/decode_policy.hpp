#pragma once

#include "vtenc/command_line.hpp"
#include "vtenc/error.hpp"
#include <stdexcept>

namespace vtenc {

// Only decoder operations may throw this; output/encoder/filter errors are fatal.
class HardwareDecodeError : public MediaError {
public:
    using MediaError::MediaError;
    explicit HardwareDecodeError(const std::exception& cause) : MediaError(cause.what(), native_status(cause)) {}
};

template <class Attempt, class Report>
auto with_decode_policy(DecodeMode mode, Attempt attempt, Report report) {
    if (mode != DecodeMode::automatic) return attempt(mode);
    try {
        return attempt(DecodeMode::hardware);
    } catch (const HardwareDecodeError& error) {
        // The failed attempt has unwound completely before reopening the input.
        report(error);
        return attempt(DecodeMode::software);
    }
}

} // namespace vtenc
