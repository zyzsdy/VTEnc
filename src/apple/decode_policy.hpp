#pragma once

#include "vtenc/command_line.hpp"
#include <stdexcept>

namespace vtenc {

// Only decoder operations may throw this; output/encoder/filter errors are fatal.
class HardwareDecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
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
