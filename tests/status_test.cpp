#include "vtenc/status.hpp"
#include "vtenc/encoder.hpp"
#include "vtenc/error.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() try {
    using namespace vtenc;
    require(json_string("a\n\t\"\\") == "\"a\\u000a\\u0009\\\"\\\\\"", "JSON control escaping");
    require(json_string("中文😀") == "\"中文😀\"", "UTF-8 preservation");
    require(json_string(std::string("\xff\xc0", 2)) == "\"\\u00ff\\u00c0\"", "Invalid UTF-8 must not corrupt JSON");
    require(!native_status(std::runtime_error("filename (OSStatus=-1)")), "Text was mistaken for a native status");
    require(native_status(MediaError("operation", -50)) == -50, "Typed native status lost");
    std::ostringstream output;
    StatusReporter report(true, output);
    report.attempt(DecodeMode::hardware);
    report.advance(1, {5000, 1000}, {1, 30}, {30, 30}, 42);
    report.diagnostic("wrapped operation failed (OSStatus=-12911)\nsecond line", -12911);
    report.attempt(DecodeMode::software);
    report.advance(1, {5000, 1000}, {1, 30}, {}, 42);
    report.failure("test write failed (OSStatus=-50)", 1, -50);
    const auto before = output.str();
    report.finish({1, 100, false}); // A terminal event can never be overwritten by success.
    require(output.str() == before, "Duplicate terminal event");
    require(before.find("\"os_status\":-12911") != std::string::npos, "Wrapped OSStatus lost");
    require(before.find("\"os_status\":-50") != std::string::npos, "Failure OSStatus lost");
    require(before.find("second line") != std::string::npos && before.find("\\u000a") != std::string::npos, "Diagnostic embedded newline");
    const auto final = before.substr(before.rfind("{\"schema_version\""));
    require(final.find("\"attempt\":2") != std::string::npos, "Retry attempt not advanced");
    require(final.find("\"frames\":1") != std::string::npos, "Retry frame count not reset");
    require(final.find("\"percent\":null,\"eta_seconds\":null") != std::string::npos, "Unknown duration fabricated progress");
    require(final.find("\"output_complete\":false") != std::string::npos, "Failure marked complete");
    std::ostringstream known;
    StatusReporter complete(true, known);
    complete.attempt(DecodeMode::automatic, true);
    complete.advance(1, {0, 30}, {1, 30}, {1, 30}, 42);
    require(known.str().find("\"percent\":99.9") != std::string::npos, "Reached 100 before publication");
    complete.finish({1, 100, false});
    require(known.str().find("\"percent\":100") != std::string::npos, "Missing completed 100 percent");
    std::ostringstream cancelled;
    StatusReporter stop(true, cancelled);
    stop.finish({0, 0, true});
    require(cancelled.str().find("\"hardware\":null") != std::string::npos, "Cancellation fabricated hardware verification");
    require(cancelled.str().find("\"bytes\":null") != std::string::npos, "Cancellation fabricated final byte count");
    stop.diagnostic_failed();
    bool callback_failed = false;
    try { stop.check_diagnostics(); } catch (const std::runtime_error&) { callback_failed = true; }
    require(callback_failed, "C callback failure was swallowed");
    std::cout << "PASS status protocol boundaries\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
