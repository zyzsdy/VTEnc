#include "bitstream_output.hpp"
#include "encoder_session.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace vtenc {
namespace {
[[noreturn]] void file_error(const std::string& operation, const std::string& path) {
    const auto error = errno;
    throw std::runtime_error(operation + " [" + path + "]: " + std::strerror(error) +
        " (errno=" + std::to_string(error) + ")");
}
constexpr std::array<unsigned char, 4> start_code{0, 0, 0, 1};
void write_nal(OutputFile& file, const void* bytes, std::size_t size) {
    file.write(start_code.data(), start_code.size());
    file.write(bytes, size);
}
}

OutputFile::OutputFile(std::string path) : path_(std::move(path)) {
    struct stat info{};
    if (lstat(path_.c_str(), &info) == 0) throw std::runtime_error("Output already exists: " + path_);
    if (errno != ENOENT) file_error("Cannot inspect output", path_);
    std::string pattern = path_ + ".part.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) file_error("Cannot create temporary output", path_);
    // Allocate the pathname before acquiring the FILE ownership.
    try { temporary_ = name.data(); }
    catch (...) { ::close(fd); unlink(name.data()); throw; }
    file_ = fdopen(fd, "wb");
    if (!file_) {
        const int error = errno;
        ::close(fd);
        unlink(temporary_.c_str());
        errno = error;
        file_error("fdopen", path_);
    }
}
OutputFile::~OutputFile() {
    if (file_) std::fclose(file_);
    if (!temporary_.empty()) unlink(temporary_.c_str());
}
void OutputFile::write(const void* data, std::size_t size) {
    if (std::fwrite(data, 1, size, file_) != size) file_error("Write output", path_);
    bytes_ += size;
}
void OutputFile::close() {
    if (!file_) return;
    FILE* file = std::exchange(file_, nullptr);
    if (std::fclose(file) != 0) file_error("Close output", path_);
}
void OutputFile::publish() {
    close();
    // link is atomic and refuses to clobber even when another process creates the destination.
    if (link(temporary_.c_str(), path_.c_str()) != 0) file_error("Publish output", path_);
    published_ = true;
}
void OutputFile::rollback() noexcept {
    if (published_) unlink(path_.c_str());
    published_ = false;
}

BitstreamOutput::BitstreamOutput(const EncodeConfig& config)
    : config_(config), stream_(config.output), timestamps_(config.output + ".timestamps.csv") {
    constexpr char header[] = "frame,pts_value,pts_timescale,dts_value,dts_timescale,duration_value,duration_timescale,keyframe,offset,size\n";
    timestamps_.write(header, sizeof(header) - 1);
}

void BitstreamOutput::append(CMSampleBufferRef sample) {
    if (!CMSampleBufferDataIsReady(sample) || CMSampleBufferGetNumSamples(sample) != 1) {
        throw std::runtime_error("Encoder output is not one ready compressed frame");
    }
    auto format = CMSampleBufferGetFormatDescription(sample);
    if (!format) throw std::runtime_error("Encoder output has no format description");
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(format);
    if (dimensions.width != config_.video.width || dimensions.height != config_.video.height ||
        CMFormatDescriptionGetMediaSubType(format) !=
            (config_.video.codec == Codec::h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC)) {
        throw std::runtime_error("Encoder changed the requested codec or dimensions");
    }
    const auto pts = CMSampleBufferGetPresentationTimeStamp(sample);
    const auto dts = CMSampleBufferGetDecodeTimeStamp(sample);
    const auto duration = CMSampleBufferGetDuration(sample);
    const auto expected = CMTimeMake(frames_ * config_.fps_den, config_.fps_num);
    if (!CMTIME_IS_NUMERIC(pts) || pts.epoch != 0 || CMTimeCompare(pts, expected) != 0 ||
        !CMTIME_IS_NUMERIC(duration) || duration.epoch != 0 ||
        CMTimeCompare(duration, CMTimeMake(config_.fps_den, config_.fps_num)) != 0) {
        throw std::runtime_error("Encoder returned unexpected PTS/duration with frame reordering disabled, frame=" +
            std::to_string(frames_));
    }
    if (CMTIME_IS_VALID(dts) && (!CMTIME_IS_NUMERIC(dts) || dts.epoch != 0 || CMTimeCompare(dts, pts) != 0)) {
        throw std::runtime_error("Encoder returned unexpected DTS with frame reordering disabled");
    }
    bool keyframe = true;
    auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        const auto not_sync = CFDictionaryGetValue(entry, kCMSampleAttachmentKey_NotSync);
        keyframe = !not_sync || !CFEqual(not_sync, kCFBooleanTrue);
    }
    if (frames_ == 0 && !keyframe) throw std::runtime_error("First encoder output is not a sync frame");
    const auto offset = stream_.bytes();
    int length_size = 0;
    std::size_t parameter_count = 0;
    const auto get_parameter = [&](std::size_t index, const std::uint8_t** data, std::size_t* size) {
        return config_.video.codec == Codec::h264
            ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, index, data, size, &parameter_count, &length_size)
            : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, index, data, size, &parameter_count, &length_size);
    };
    const std::uint8_t* parameter = nullptr;
    std::size_t parameter_size = 0;
    check_status(get_parameter(0, &parameter, &parameter_size), "Get codec parameter sets");
    if (length_size < 1 || length_size > 4 || parameter_count == 0) {
        throw std::runtime_error("Invalid encoder NAL length size or parameter set count");
    }
    if (keyframe) {
        for (std::size_t i = 0; i < parameter_count; ++i) {
            check_status(get_parameter(i, &parameter, &parameter_size), "Get codec parameter set");
            if (!parameter || !parameter_size) throw std::runtime_error("Empty codec parameter set");
            write_nal(stream_, parameter, parameter_size);
        }
    }
    auto block = CMSampleBufferGetDataBuffer(sample);
    if (!block) throw std::runtime_error("Encoder sample has no compressed data buffer");
    const auto total = CMBlockBufferGetDataLength(block);
    if (!total) throw std::runtime_error("Encoder sample contains no NAL units");
    std::size_t position = 0;
    std::array<unsigned char, 65536> buffer{};
    while (position < total) {
        const auto prefix = static_cast<std::size_t>(length_size);
        if (total - position < prefix) throw std::runtime_error("Truncated encoder NAL length");
        check_status(CMBlockBufferCopyDataBytes(block, position, prefix, buffer.data()), "Read NAL length");
        std::size_t length = 0;
        for (std::size_t i = 0; i < prefix; ++i) length = (length << 8) | buffer[i];
        position += prefix;
        if (!length || length > total - position) throw std::runtime_error("Invalid encoder NAL payload length");
        stream_.write(start_code.data(), start_code.size());
        while (length) {
            const auto count = std::min(length, buffer.size());
            check_status(CMBlockBufferCopyDataBytes(block, position, count, buffer.data()), "Read NAL payload");
            stream_.write(buffer.data(), count);
            position += count;
            length -= count;
        }
    }
    std::ostringstream row;
    row << frames_ << ',' << pts.value << ',' << pts.timescale << ',';
    if (CMTIME_IS_NUMERIC(dts)) row << dts.value << ',' << dts.timescale;
    else row << ','; // Invalid DTS stays unspecified; never invent a timestamp.
    row << ',' << duration.value << ',' << duration.timescale << ',' << (keyframe ? 1 : 0)
        << ',' << offset << ',' << (stream_.bytes() - offset) << '\n';
    const auto text = row.str();
    timestamps_.write(text.data(), text.size());
    ++frames_;
}

void BitstreamOutput::publish() {
    stream_.close();
    timestamps_.close();
    timestamps_.publish();
    try { stream_.publish(); }
    catch (...) { timestamps_.rollback(); throw; }
}

} // namespace vtenc
