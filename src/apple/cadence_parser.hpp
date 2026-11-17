#pragma once
#include "frame_cadence.hpp"
#include <memory>
struct AVCodecParameters;
struct AVPacket;
namespace vtenc {
// Metadata-only H.264/HEVC parser. No software decoder is opened or run.
class CadenceParser {
public:
    explicit CadenceParser(const AVCodecParameters* parameters);
    ~CadenceParser();
    [[nodiscard]] FrameCadence parse(const AVPacket* packet);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
