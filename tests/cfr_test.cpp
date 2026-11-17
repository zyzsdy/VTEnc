#include "timeline.hpp"
#include "encoder_session.hpp"
#include <iostream>
#include <limits>
#include <vector>

namespace {
void require(bool value, const char* context) { if (!value) throw std::runtime_error(context); }
class Frames final : public vtenc::DecodedFrames {
public:
    explicit Frames(std::vector<int> ticks) : ticks_(std::move(ticks)) {}
    std::unique_ptr<vtenc::DecodedFrame> next() override {
        if (index_ == ticks_.size() - 1) return nullptr;
        CVPixelBufferRef raw = nullptr;
        vtenc::check_status(CVPixelBufferCreate(nullptr, 2, 2, kCVPixelFormatType_32BGRA, nullptr, &raw), "Create test pixel");
        vtenc::FrameCadence cadence{}; cadence.repeat_fields = static_cast<int>(index_);
        const auto start = ticks_[index_], end = ticks_[++index_];
        return std::make_unique<vtenc::DecodedFrame>(vtenc::DecodedFrame{vtenc::CFRef(raw), CMTimeMake(start, 1000), CMTimeMake(end - start, 1000), cadence});
    }
private:
    std::vector<int> ticks_;
    std::size_t index_ = 0;
};
}
int main() try {
    using namespace vtenc;
    volatile std::sig_atomic_t cancelled = 0;
    for (const auto mode : {SyncMode::cfr, SyncMode::forcecfr}) {
        CfrFrames frames(std::make_unique<Frames>(std::vector<int>{200, 210, 260, 310}), mode, 25, 1, cancelled);
        const int selected[]{0, 1, 2};
        for (int i = 0; i < 3; ++i) {
            auto frame = frames.next(); require(frame != nullptr, "Missing CFR frame");
            require(frame->cadence.repeat_fields == selected[i], "Wrong sample-and-hold picture");
            require(CMTimeCompare(frame->pts, CMTimeMake(200 + 40 * i, 1000)) == 0, "CFR shifted nonzero origin");
            require(CMTimeCompare(frame->duration, CMTimeMake(i == 2 && mode == SyncMode::cfr ? 30 : 40, 1000)) == 0, "Wrong CFR tail");
        }
        require(!frames.next(), "CFR emitted extra frame");
    }
    CfrFrames short_input(std::make_unique<Frames>(std::vector<int>{0, 10, 20}), SyncMode::cfr, 25, 1, cancelled);
    require(CMTimeCompare(short_input.next()->duration, CMTimeMake(20, 1000)) == 0, "Dropped final frame extended CFR end");
    require(!short_input.next(), "Short input repeated beyond end");
    CfrFrames gap(std::make_unique<Frames>(std::vector<int>{-100, 900, 940}), SyncMode::cfr, 25, 1, cancelled);
    for (int i = 0; i < 26; ++i) {
        auto frame = gap.next(); require(frame && frame->cadence.repeat_fields == (i == 25 ? 1 : 0), "Gap must hold preceding picture");
    }
    require(!gap.next(), "Gap end mismatch");
    // A 30-hour timeline evaluated independently from the closed-form rational.
    const auto origin = CMTimeMake(7, 5);
    for (std::int64_t index = 0; index < 3236764; index += 997) {
        const auto actual = cfr_timestamp(origin, index, 30000, 1001);
        require(!CMTIME_HAS_BEEN_ROUNDED(actual), "Long CFR clock rounded");
        require(CMTimeCompare(actual, CMTimeMake(42000 + index * 1001, 30000)) == 0, "Long CFR drift");
    }
    for (int which = 0; which < 3; ++which) {
        bool rejected = false;
        try {
            if (which == 0) (void)cfr_timestamp(kCMTimeZero, INT64_MAX, 30000, 1001);
            if (which == 1) (void)timeline_timescale(INT32_MAX, 30000);
            if (which == 2) {
                CfrFrames invalid(std::make_unique<Frames>(std::vector<int>{0, 0, 40}), SyncMode::cfr, 25, 1, cancelled);
                (void)invalid.next();
            }
        } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Invalid CFR source/overflow accepted");
    }
    require(display_sar({16, 9}, 720, 576, {1, 0, 0, 1, 0, 0}) == std::array<int, 2>{64, 45}, "DAR reduction");
    require(display_sar({9, 16}, 720, 576, {0, 1, -1, 0, 576, 0}) == std::array<int, 2>{64, 45}, "Rotated DAR");
    require(display_sar({16, 9}, 720, 576, {-1, 0, 0, 1, 720, 0}) == std::array<int, 2>{64, 45}, "Reflected DAR");
    for (auto transform : {std::array<double, 6>{2, 0, 0, 1, 0, 0}, std::array<double, 6>{1, 0, 1, 1, 0, 0}}) {
        bool rejected = false;
        try { (void)display_sar({16, 9}, 720, 576, transform); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Unsupported DAR transform accepted");
    }
    cancelled = 1;
    require(!gap.next(), "CFR cancellation ignored");
    std::cout << "PASS CFR selection, gaps, tail, drift, overflow and DAR rotation\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
