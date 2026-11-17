#include "vtenc/capabilities.hpp"
#include "vtenc/command_line.hpp"
#include "vtenc/encoder.hpp"

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
    try {
        std::vector<std::string_view> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        vtenc::CommandLine options;
        try {
            options = vtenc::parse_command_line(args);
        } catch (const std::invalid_argument& error) {
            std::cerr << "vtenc: " << error.what() << "\nRun vtenc --help for usage.\n";
            return 2;
        }
        switch (options.command) {
        case vtenc::Command::help:
            std::cout << vtenc::help_text();
            break;
        case vtenc::Command::version:
            std::cout << "VTEnc " << VTENC_VERSION << " (macOS arm64; VideoToolbox hardware encoder)\n";
            break;
        case vtenc::Command::check_device:
            std::cout << vtenc::metal_device_name() << '\n';
            break;
        case vtenc::Command::check_features: {
            const auto capabilities = vtenc::probe_encoder(options.probe);
            std::cout << "VideoToolbox hardware session: " << vtenc::codec_name(capabilities.config.codec)
                      << ' ' << capabilities.config.bit_depth << "-bit "
                      << capabilities.config.width << 'x' << capabilities.config.height << '\n'
                      << "Hardware verified: " << (capabilities.hardware ? "yes" : "no") << '\n'
                      << "Session prepared; no frames encoded. Advertised properties (values not validated):\n";
            for (const auto& name : capabilities.property_names) std::cout << "  " << name << '\n';
            break;
        }
        case vtenc::Command::encode:
        case vtenc::Command::transcode: {
            SignalHandler interrupt_handler(SIGINT, handle_interrupt);
            // Report EPIPE through avformat instead of dying without a diagnostic.
            SignalHandler pipe_handler(SIGPIPE, SIG_IGN);
            stdout_output = options.encode.output == "-";
            auto& log = stdout_output ? std::cerr : std::cout;
            const bool transcode = options.command == vtenc::Command::transcode;
            const auto result = transcode ? vtenc::transcode_file(options.encode, interrupted)
                                          : vtenc::encode_video(options.encode, interrupted);
            if (result.cancelled) {
                std::cerr << "vtenc: cancelled; drained " << result.frames << " frames; "
                          << (stdout_output ? "stdout may contain incomplete output; written bytes cannot be retracted\n"
                                            : "incomplete output discarded\n");
                return 130;
            }
            log << "Hardware verified: yes\nEncoded " << result.frames << " frames, " << result.bytes
                      << " bytes (" << vtenc::codec_name(options.encode.video.codec) << ' '
                      << options.encode.video.bit_depth << "-bit)\nOutput: " << options.encode.output << '\n';
            if (transcode) {
                log << "Input demuxer: FFmpeg libavformat\n";
                log << (result.decoder == vtenc::DecodeMode::hardware
                    ? "Hardware decoder verified: yes\n" : "Software decoder: FFmpeg libavcodec (libavformat demux)\n")
                    << "Audio samples copied (first track): " << result.audio_samples << '\n';
            } else if (result.timestamps_csv) log << "Timestamps: " << options.encode.output << ".timestamps.csv\n";
            log << "Metal GPU frames completed: " << result.gpu_frames << '\n';
            break;
        }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vtenc: " << error.what() << '\n';
        if (interrupted) {
            std::cerr << (stdout_output ? "vtenc: cancelled; stdout may contain incomplete output; written bytes cannot be retracted\n"
                                       : "vtenc: cancelled; incomplete output discarded\n");
            return 130;
        }
        return 1;
    }
}
