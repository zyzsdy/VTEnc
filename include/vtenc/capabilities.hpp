#pragma once

#include "vtenc/command_line.hpp"

#include <string>
#include <vector>

namespace vtenc {

struct EncoderCapabilities {
    ProbeConfig config;
    bool hardware;
    std::vector<std::string> property_names;
};

// Throws on any failure; never tries a software encoder.
[[nodiscard]] EncoderCapabilities probe_encoder(const ProbeConfig& config);
[[nodiscard]] std::string metal_device_name();

} // namespace vtenc
