#pragma once

#include "media_io.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <limits>
#include <stdexcept>
#include <string>

namespace vtenc {
inline void av_check(int code, const char* operation) {
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message + " (FFmpeg=" + std::to_string(code) + ")");
}
inline PacketPtr make_packet() {
    PacketPtr result(av_packet_alloc());
    if (!result) throw std::bad_alloc();
    return result;
}
inline CMTime media_time(std::int64_t value, AVRational base, const char* context) {
    if (value == AV_NOPTS_VALUE || base.num <= 0 || base.den <= 0 ||
        value > std::numeric_limits<std::int64_t>::max() / base.num ||
        value < std::numeric_limits<std::int64_t>::min() / base.num) {
        throw std::runtime_error(std::string("Invalid or overflowing FFmpeg ") + context);
    }
    return CMTimeMake(value * base.num, base.den);
}
inline std::int64_t exact_ticks(CMTime time, AVRational base) {
    if (!CMTIME_IS_NUMERIC(time) || time.epoch || CMTIME_HAS_BEEN_ROUNDED(time) || base.num <= 0 || base.den <= 0) {
        throw std::runtime_error("Invalid packet timestamp/time base");
    }
    const auto result = av_rescale_q(time.value, AVRational{1, time.timescale}, base);
    if (CMTimeCompare(media_time(result, base, "rescaled packet timestamp"), time) != 0) {
        throw std::runtime_error("Packet timestamp cannot be represented exactly in output time base");
    }
    return result;
}
}
