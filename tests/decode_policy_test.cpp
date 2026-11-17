#include "decode_policy.hpp"
#include <array>
#include <iostream>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}
int main() try {
    using vtenc::DecodeMode;
    using vtenc::HardwareDecodeError;
    using vtenc::with_decode_policy;
    const auto parse = [](auto... flags) {
        const std::array<std::string_view, 4 + sizeof...(flags)> args{"-i", "input.mp4", "-o", "output.mp4", flags...};
        return vtenc::parse_command_line(args).encode.decode;
    };
    require(parse() == DecodeMode::automatic, "Wrong default mode");
    require(parse("--avhw") == DecodeMode::hardware, "--avhw not selected");
    require(parse("--avsw") == DecodeMode::software, "--avsw not selected");

    for (auto mode : {DecodeMode::automatic, DecodeMode::hardware, DecodeMode::software}) {
        std::vector<DecodeMode> calls;
        int reports = 0;
        const auto value = with_decode_policy(mode, [&](DecodeMode selected) {
            calls.push_back(selected);
            return 42;
        }, [&](const auto&) { ++reports; });
        require(value == 42 && calls.size() == 1 && reports == 0, "Successful attempt retried");
        require(calls[0] == (mode == DecodeMode::software ? mode : DecodeMode::hardware), "Wrong first attempt");
    }
    for (const auto name : {"input.mkv", "input.ts", "input.webm", "input.unknown", "no-extension", "-"}) {
        const std::array<std::string_view, 5> args{"-i", name, "-o", "output.mp4", "--avhw"};
        require(vtenc::parse_command_line(args).command == vtenc::Command::transcode, "Input extension filtered before avformat");
    }
    {
        const std::array<std::string_view, 8> args{"-i", "raw.any", "--input-res", "320x180", "--frames", "1", "-o", "out.h264"};
        require(vtenc::parse_command_line(args).command == vtenc::Command::encode, "Explicit raw mode lost");
    }
    std::vector<DecodeMode> calls;
    bool attempt_alive = false;
    int reports = 0;
    const auto result = with_decode_policy(DecodeMode::automatic, [&](DecodeMode selected) {
        require(!attempt_alive, "Old attempt not destroyed");
        struct Lifetime {
            bool& alive;
            explicit Lifetime(bool& value) : alive(value) { alive = true; }
            ~Lifetime() { alive = false; }
        } lifetime(attempt_alive);
        calls.push_back(selected);
        if (selected == DecodeMode::hardware) throw HardwareDecodeError("late decode failure");
        return 17;
    }, [&](const auto& error) {
        require(!attempt_alive, "Report before attempt cleanup");
        require(std::string(error.what()) == "late decode failure", "Hardware diagnostic lost");
        ++reports;
    });
    require(result == 17 && reports == 1 && calls == std::vector{DecodeMode::hardware, DecodeMode::software}, "Wrong fallback sequence");

    for (auto mode : {DecodeMode::automatic, DecodeMode::hardware, DecodeMode::software}) {
        int attempts = 0;
        reports = 0;
        try {
            with_decode_policy(mode, [&](DecodeMode) -> int {
                ++attempts;
                throw HardwareDecodeError("failure");
            }, [&](const auto&) { ++reports; });
            throw std::logic_error("Failure swallowed");
        } catch (const HardwareDecodeError&) {}
        require(attempts == (mode == DecodeMode::automatic ? 2 : 1), "Unbounded retry or forced mode fallback");
        require(reports == (mode == DecodeMode::automatic ? 1 : 0), "Wrong retry report count");
    }
    int attempts = 0;
    try {
        with_decode_policy(DecodeMode::automatic, [&](DecodeMode) -> int {
            ++attempts;
            throw std::runtime_error("encoder/filter/output failure");
        }, [&](const auto&) { throw std::logic_error("Non-decoder failure retried"); });
        throw std::logic_error("Runtime failure swallowed");
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "encoder/filter/output failure", "Wrong runtime diagnostic");
    }
    require(attempts == 1, "Runtime error retried");
    struct Cancelled {};
    try {
        with_decode_policy(DecodeMode::automatic, [](DecodeMode) -> int { throw Cancelled{}; },
            [](const auto&) { throw std::logic_error("Cancellation retried"); });
        throw std::logic_error("Cancellation swallowed");
    } catch (const Cancelled&) {}
    std::cout << "Decode policy: forced modes, retry-once, cleanup and error boundaries passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
