#include "vtenc/capabilities.hpp"
#include "vtenc/command_line.hpp"
#include "vtenc/encoder.hpp"
#include "vtenc/status.hpp"
#include "vtenc/error.hpp"
extern "C" {
#include <libavutil/log.h>
}
#include <unistd.h>
#include <algorithm>

#include <exception>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void print_version(std::ostream& output) {
    output << "VTEnc " << VTENC_VERSION << " (macOS arm64; VideoToolbox hardware encoder)\n"
           << "Build: " << VTENC_BUILD_INFO << '\n';
}

vtenc::StatusReporter* active_reporter = nullptr;
void ffmpeg_log(void* context, int level, const char* format, va_list args) noexcept {
    if (level > av_log_get_level()) return;
    char buffer[4096]{};
    int prefix = 1;
    const int size = av_log_format_line2(context, level, format, args, buffer, sizeof(buffer), &prefix);
    if (size < 0) { if (active_reporter) active_reporter->diagnostic_failed(); return; }
    try {
        std::string text(buffer);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (size >= static_cast<int>(sizeof(buffer))) text += " [diagnostic truncated]";
        if (active_reporter) active_reporter->diagnostic(text);
    } catch (...) {
        // Report on the scheduler thread, never unwind across FFmpeg's C ABI.
        if (active_reporter) active_reporter->diagnostic_failed();
    }
}
class Diagnostics {
public:
    explicit Diagnostics(vtenc::StatusReporter& reporter) : buffer_(reporter), original_(std::cerr.rdbuf(&buffer_)) {
        active_reporter = &reporter;
        av_log_set_callback(ffmpeg_log);
    }
    ~Diagnostics() {
        av_log_set_callback(av_log_default_callback);
        active_reporter = nullptr;
        std::cerr.rdbuf(original_);
    }
private:
    vtenc::DiagnosticBuffer buffer_;
    std::streambuf* original_;
};
volatile std::sig_atomic_t interrupted = 0;
extern "C" void handle_interrupt(int) { interrupted = 1; }
class SignalHandler {
public:
    using Handler = void (*)(int);
    SignalHandler(int number, Handler handler) : number_(number) {
        struct sigaction action{};
        action.sa_handler = handler;
        sigemptyset(&action.sa_mask);
        // Do not restart a blocked stdout write after SIGINT. avformat's
        // interrupt callback then observes cancellation instead of retrying it.
        if (sigaction(number, &action, &previous_) != 0) {
            throw std::runtime_error("Cannot install signal handler: " + std::string(std::strerror(errno)));
        }
    }
    ~SignalHandler() { sigaction(number_, &previous_, nullptr); }
private:
    int number_;
    struct sigaction previous_{};
};
}

int main(int argc, char* argv[]) {
    bool stdout_output = false;
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    const bool json = std::find(args.begin(), args.end(), "--json-status") != args.end();
    std::ostream sink(std::cerr.rdbuf());
    vtenc::StatusReporter reporter(json, sink, isatty(STDERR_FILENO));
    Diagnostics diagnostics(reporter);
    try {
        vtenc::CommandLine options;
        try {
            options = vtenc::parse_command_line(args);
        } catch (const std::invalid_argument& error) {
            reporter.failure(error.what(), 2);
            return 2;
        }
        switch (options.command) {
        case vtenc::Command::help:
            print_version(std::cout);
            std::cout << '\n' << vtenc::help_text(options.help_keyword);
            break;
        case vtenc::Command::version:
            if (json) reporter.event("version", "\"version\":" + vtenc::json_string(VTENC_VERSION) + ",\"platform\":\"macos-arm64\"");
            else print_version(std::cout);
            break;
        case vtenc::Command::check_device:
            std::cout << vtenc::metal_device_name() << '\n';
            break;
        case vtenc::Command::check_features: {
            const auto capabilities = vtenc::probe_encoder(options.probe);
            reporter.check_diagnostics();
            if (json) { reporter.capabilities(capabilities, VTENC_VERSION); break; }
            std::cout << "VideoToolbox hardware session: " << vtenc::codec_name(capabilities.config.codec)
                      << ' ' << capabilities.config.bit_depth << "-bit "
                      << capabilities.config.width << 'x' << capabilities.config.height << '\n'
                      << "Hardware verified: " << (capabilities.hardware ? "yes" : "no") << '\n'
                      << "Session prepared; no frames encoded. Advertised properties (values not validated):\n";
            for (const auto& name : capabilities.property_names) std::cout << "  " << name << '\n';
            std::cout << "Public control probes (no frames encoded; no bitrate/bitstream guarantee):\n";
            for (const auto& control : capabilities.controls) {
                std::cout << "  " << control.name << ": " << control.stage << " " << control.settings_json << '\n';
                if (!control.error.empty()) std::cout << "    " << control.error << '\n';
            }
            std::cout << "Native preset dictionaries (read only; not CLI presets): " << capabilities.native_presets_json << '\n';
            break;
        }
        case vtenc::Command::encode:
        case vtenc::Command::transcode: {
            SignalHandler interrupt_handler(SIGINT, handle_interrupt);
            // Report EPIPE through avformat instead of dying without a diagnostic.
            SignalHandler pipe_handler(SIGPIPE, SIG_IGN);
            stdout_output = options.encode.output == "-";
            auto& log = std::cerr;
            const bool transcode = options.command == vtenc::Command::transcode;
            options.encode.status = &reporter;
            reporter.start(options.encode, transcode, VTENC_VERSION);
            const auto result = transcode ? vtenc::transcode_file(options.encode, interrupted)
                                          : vtenc::encode_video(options.encode, interrupted);
            reporter.check_diagnostics();
            if (result.cancelled) {
                std::cerr << "vtenc: cancelled; drained " << result.frames << " frames; "
                          << (stdout_output ? "stdout may contain incomplete output; written bytes cannot be retracted\n"
                                            : "incomplete output discarded\n");
                reporter.finish(result);
                return 130;
            }
            log << "Hardware verified: yes\nEncoded " << result.frames << " frames, " << result.bytes
                      << " bytes (" << vtenc::codec_name(options.encode.video.codec) << ' '
                      << options.encode.video.bit_depth << "-bit)\nOutput: " << options.encode.output << '\n';
            if (transcode) {
                log << "Input demuxer: FFmpeg libavformat\n";
                log << (result.decoder == vtenc::DecodeMode::hardware
                    ? "Hardware decoder verified: yes\n" : "Software decoder: FFmpeg libavcodec (libavformat demux)\n")
                    << (options.encode.aac_audio ? "AAC packets encoded (first track): " : "Audio packets copied (first track): ") << result.audio_samples << '\n';
            } else if (result.timestamps_csv) log << "Timestamps: " << options.encode.output << ".timestamps.csv\n";
            log << "Metal GPU frames completed: " << result.gpu_frames << '\n';
            reporter.finish(result);
            break;
        }
        }
        return 0;
    } catch (const std::exception& error) {
        if (interrupted) {
            std::cerr << (stdout_output ? "vtenc: cancelled; stdout may contain incomplete output; written bytes cannot be retracted\n"
                                       : "vtenc: cancelled; incomplete output discarded\n");
            reporter.failure(error.what(), 130, vtenc::native_status(error));
            return 130;
        }
        reporter.failure(error.what(), 1, vtenc::native_status(error));
        return 1;
    }
}
