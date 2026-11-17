#pragma once

#include "vtenc/command_line.hpp"
#include <CoreMedia/CoreMedia.h>
#include <array>
#include <csignal>
#include <memory>

struct AVPacket;
struct AVCodecParameters;
struct AVRational;

namespace vtenc {

struct MediaCancelled {};
struct PacketDeleter { void operator()(AVPacket* packet) const noexcept; };
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// +1 result; preserves compressed payload and maps media time to the shared movie timeline.
[[nodiscard]] CMSampleBufferRef map_container_sample(CMSampleBufferRef sample, CMTimeMapping mapping);
void validate_video_edit(CMTimeMapping mapping, CMTime first_pts, CMTime last_pts, CMTime last_duration);

struct MediaInfo {
    CMFormatDescriptionRef video_format = nullptr; // Borrowed from ContainerInput.
    const AVCodecParameters* audio_parameters = nullptr; // Borrowed from ContainerInput.
    int audio_timebase_num = 0;
    CMTimeRange video_range = kCMTimeRangeInvalid;
    bool replayable = false; // avformat IO can be restarted; pipes/live inputs cannot.
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
    [[nodiscard]] PacketPtr read_audio();
    [[nodiscard]] PacketPtr read_video_packet();
    [[nodiscard]] const AVCodecParameters* video_parameters() const;
    [[nodiscard]] AVRational video_time_base() const;
    [[nodiscard]] CMVideoFormatDescriptionRef hardware_video_format();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ContainerOutput {
public:
    ContainerOutput(const EncodeConfig& config, const MediaInfo& info, CMFormatDescriptionRef encoded_format,
                    const volatile std::sig_atomic_t& cancelled);
    ~ContainerOutput();
    void append_video(CMSampleBufferRef sample);
    void append_audio(const AVPacket* packet);
    // Finishes and publishes files only if not cancelled; stdout is irreversible.
    // Returns zero on cancellation. The cancellation flag must outlive the writer.
    [[nodiscard]] std::uint64_t finish(const volatile std::sig_atomic_t& cancelled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtenc
