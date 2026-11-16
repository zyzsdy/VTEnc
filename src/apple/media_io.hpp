#pragma once

#include "vtenc/command_line.hpp"
#include <CoreMedia/CoreMedia.h>
#include <array>
#include <csignal>
#include <memory>

namespace vtenc {

struct MediaCancelled {};

// +1 result; preserves compressed payload and maps media time to the shared movie timeline.
[[nodiscard]] CMSampleBufferRef map_container_sample(CMSampleBufferRef sample, CMTimeMapping mapping);
void validate_video_edit(CMTimeMapping mapping, CMTime first_pts, CMTime last_pts, CMTime last_duration);

struct MediaInfo {
    CMFormatDescriptionRef video_format = nullptr; // Borrowed from ContainerInput.
    CMFormatDescriptionRef audio_format = nullptr;
    CMTimeRange video_range = kCMTimeRangeInvalid;
    std::int32_t video_timescale = 0;
    std::int32_t audio_timescale = 0;
    std::array<double, 6> transform{1, 0, 0, 1, 0, 0};
};

class ContainerInput {
public:
    ContainerInput(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled);
    ~ContainerInput();
    [[nodiscard]] const MediaInfo& info() const;
    // Returns +1 compressed samples, or null on clean end of that track.
    [[nodiscard]] CMSampleBufferRef read_video();
    [[nodiscard]] CMSampleBufferRef read_audio();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ContainerOutput {
public:
    ContainerOutput(const EncodeConfig& config, const MediaInfo& info, CMFormatDescriptionRef encoded_format);
    ~ContainerOutput();
    [[nodiscard]] bool video_ready() const;
    [[nodiscard]] bool audio_ready() const;
    void append_video(CMSampleBufferRef sample);
    void append_audio(CMSampleBufferRef sample);
    void end_video();
    void end_audio();
    // Finishes and publishes only if not cancelled; returns zero on cancellation.
    [[nodiscard]] std::uint64_t finish(const volatile std::sig_atomic_t& cancelled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtenc
