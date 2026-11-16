#include "vtenc/capabilities.hpp"
#include "vtenc/command_line.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

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
            std::cout << "VTEnc " << VTENC_VERSION << " (macOS arm64; VideoToolbox; scaffold)\n";
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
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vtenc: " << error.what() << '\n';
        return 1;
    }
}
