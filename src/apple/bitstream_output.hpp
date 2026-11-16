#pragma once

#include "vtenc/command_line.hpp"
#include <CoreMedia/CoreMedia.h>
#include <cstdio>
#include <cstdint>
#include <string>

namespace vtenc {

// Same-directory temporary file, published without replacing an existing path.
class OutputFile {
public:
    explicit OutputFile(std::string path);
    ~OutputFile();
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    void write(const void* data, std::size_t size);
    void close();
    void publish();
    void rollback() noexcept;
    [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
private:
    std::string path_;
    std::string temporary_;
    FILE* file_ = nullptr;
    std::uint64_t bytes_ = 0;
    bool published_ = false;
};

class BitstreamOutput {
public:
    explicit BitstreamOutput(const EncodeConfig& config);
    void append(CMSampleBufferRef sample);
    void publish();
    [[nodiscard]] std::int64_t frames() const { return frames_; }
    [[nodiscard]] std::uint64_t bytes() const { return stream_.bytes(); }
private:
    const EncodeConfig& config_;
    OutputFile stream_;
    OutputFile timestamps_;
    std::int64_t frames_ = 0;
};

} // namespace vtenc
