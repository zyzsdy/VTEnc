#pragma once

#include "vtenc/command_line.hpp"

#include <string>
#include <vector>

namespace vtenc {

struct EncoderControlProbe {
    std::string name;
    bool enumerated = false;
    std::string stage = "not_started";
    std::string settings_json = "null";
    std::string encoder_id, error;
    std::optional<std::int32_t> os_status;
};
struct EncoderCapabilities {
    ProbeConfig config;
    bool hardware = false;
    std::vector<std::string> property_names;
    std::string encoder_id;
    std::vector<EncoderControlProbe> controls;
    std::string native_presets_json = "null";
    std::optional<std::int32_t> native_presets_status;
};

// Throws on any failure; never tries a software encoder.
[[nodiscard]] EncoderCapabilities probe_encoder(const ProbeConfig& config);
[[nodiscard]] std::string metal_device_name();

} // namespace vtenc
