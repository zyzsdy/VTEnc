#pragma once
#include "vtenc/command_line.hpp"
#include <chrono>
#include <atomic>
#include <mutex>
#include <ostream>
#include <streambuf>

namespace vtenc {
struct EncodeResult;
struct EncoderCapabilities;
// All times retain their integer rational representation in the wire protocol.
struct StatusTime { std::int64_t value = 0; std::int32_t timescale = 1; };
[[nodiscard]] std::string json_string(std::string_view text);
[[nodiscard]] std::string configuration_json(const EncodeConfig& config, bool transcode);

class StatusReporter {
public:
    StatusReporter(bool json, std::ostream& output, bool terminal = false);
    void event(std::string_view type, std::string_view fields = {});
    void diagnostic(std::string_view message, std::optional<std::int32_t> os_status = {});
    void diagnostic_failed() noexcept { diagnostic_failed_.store(true); }
    void check_diagnostics();
    void start(const EncodeConfig& config, bool transcode, std::string_view version);
    void attempt(DecodeMode mode, bool raw = false);
    void session(const EncodeConfig& config, std::string_view encoder_id, bool transcode, std::uint32_t pixel_format);
    void advance(std::int64_t frames, StatusTime pts, StatusTime media_time,
                 StatusTime total = {}, std::uint64_t video_bytes = 0);
    void finish(const EncodeResult& result);
    void failure(std::string_view message, int exit_code, std::optional<std::int32_t> os_status = {});
    void capabilities(const EncoderCapabilities& capabilities, std::string_view version);
private:
    void progress(std::string_view type, std::string_view extra = {});
    bool json_, terminal_, line_open_ = false, finished_ = false, hardware_ = false;
    std::atomic<bool> diagnostic_failed_{false};
    std::ostream& output_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point started_, updated_, attempt_started_;
    std::int64_t frames_ = 0;
    std::uint64_t bytes_ = 0, sequence_ = 0;
    unsigned attempt_ = 0;
    StatusTime pts_, media_, total_;
};

// Route existing C++ diagnostic lines through the same serializer as events.
// Does not treat ostream's unitbuf flush as the end of a diagnostic line.
class DiagnosticBuffer : public std::streambuf {
public:
    explicit DiagnosticBuffer(StatusReporter& reporter) : reporter_(reporter) {}
    ~DiagnosticBuffer() override;
protected:
    int_type overflow(int_type ch) override;
    std::streamsize xsputn(const char* data, std::streamsize size) override;
private:
    StatusReporter& reporter_;
    std::string pending_;
};
}
