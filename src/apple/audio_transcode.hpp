#pragma once
#include "media_io.hpp"
namespace vtenc {
// Audio-only libavcodec decoder / libswresample / native AudioToolbox AAC-LC.
// Single selected track, bounded PCM and packet buffers; no video encoder APIs.
class AudioTranscoder {
public:
    AudioTranscoder(ContainerInput& input, const EncodeConfig& config,
                    const volatile std::sig_atomic_t& cancelled);
    ~AudioTranscoder();
    const AVCodecParameters* parameters() const;
    PacketPtr next();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vtenc
