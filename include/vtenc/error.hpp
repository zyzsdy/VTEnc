#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace vtenc {
class MediaError : public std::runtime_error {
public:
    explicit MediaError(std::string message, std::optional<std::int32_t> status = {})
        : std::runtime_error(std::move(message)), os_status(status) {}
    const std::optional<std::int32_t> os_status;
};
inline std::optional<std::int32_t> native_status(const std::exception& error) noexcept {
    const auto* media = dynamic_cast<const MediaError*>(&error);
    return media ? media->os_status : std::nullopt;
}
}
