#include "vtenc/capabilities.hpp"
#include "vtenc/command_line.hpp"
#include "vtenc/encoder.hpp"

#include <exception>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
extern "C" void handle_interrupt(int) { interrupted = 1; }
class InterruptHandler {
public:
    InterruptHandler() : previous_(std::signal(SIGINT, handle_interrupt)) {
        if (previous_ == SIG_ERR) throw std::runtime_error("Cannot install SIGINT handler");
    }
    ~InterruptHandler() { std::signal(SIGINT, previous_); }
private:
    using Handler = void (*)(int);
    Handler previous_;
};
}

int main(int argc, char* argv[]) {
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
            InterruptHandler handler;
            const bool transcode = options.command == vtenc::Command::transcode;
            const auto result = transcode ? vtenc::transcode_file(options.encode, interrupted)
                                          : vtenc::encode_video(options.encode, interrupted);
            if (result.cancelled) {
                std::cerr << "vtenc: cancelled; drained " << result.frames << " frames; incomplete output discarded\n";
                return 130;
            }
            std::cout << "Hardware verified: yes\nEncoded " << result.frames << " frames, " << result.bytes
                      << " bytes (" << vtenc::codec_name(options.encode.video.codec) << ' '
                      << options.encode.video.bit_depth << "-bit)\nOutput: " << options.encode.output << '\n';
            if (transcode) {
                std::cout << "Input demuxer: FFmpeg libavformat\n";
                std::cout << (result.decoder == vtenc::DecodeMode::hardware
                    ? "Hardware decoder verified: yes\n" : "Software decoder: FFmpeg libavcodec (libavformat demux)\n")
                    << "Audio samples copied (first track): " << result.audio_samples << '\n';
            } else if (result.timestamps_csv) std::cout << "Timestamps: " << options.encode.output << ".timestamps.csv\n";
            std::cout << "Metal GPU frames completed: " << result.gpu_frames << '\n';
            break;
        }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vtenc: " << error.what() << '\n';
        return 1;
    }
}
