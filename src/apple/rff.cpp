// Pending-field scheduling adapted from NVEncFilterIvtc.cpp (rigaya, MIT).
// See THIRD_PARTY_NOTICES.md. Streaming, exact-timeline implementation.
#include "ivtc.hpp"
#include <deque>
#include <iostream>
#include <optional>

namespace vtenc {
namespace {
void exact_rff(CMTime value) {
    if (!CMTIME_IS_NUMERIC(value) || value.epoch || CMTIME_HAS_BEEN_ROUNDED(value))
        throw std::runtime_error("RFF requires finite exact times with epoch zero");
}
}
struct RffFrames::Impl {
    struct Field {
        std::shared_ptr<DecodedFrame> frame;
        unsigned parity;
        CMTime pts, duration;
    };
    std::unique_ptr<DecodedFrames> source;
    FilterConfig config;
    const volatile std::sig_atomic_t& cancelled;
    std::shared_ptr<DecodedFrame> current, following;
    std::optional<Field> pending;
    std::unique_ptr<GpuIvtc> gpu;
    std::deque<std::unique_ptr<DecodedFrame>> ready;
    CMTime previous_interval = kCMTimeInvalid;
    bool started = false, next_boundary = false;
    std::uint64_t index = 0;
    Impl(std::unique_ptr<DecodedFrames> input, const FilterConfig& settings, const volatile std::sig_atomic_t& stop)
        : source(std::move(input)), config(settings), cancelled(stop) {}
    void emit(const Field& a, const Field* b) {
        if (!gpu) gpu = std::make_unique<GpuIvtc>(a.frame->pixel.get(), config.ivtc);
        auto top = a.frame->pixel.get(), bottom = top;
        if (b) {
            if (a.parity == b->parity) throw std::runtime_error("RFF attempted to weave two fields of the same parity");
            if (a.parity == 0) bottom = b->frame->pixel.get();
            else top = b->frame->pixel.get();
        }
        // P at top-first = cur.top + prev.bottom. This uses the same Metal
        // field weave for Y and interleaved UV, without touching CPU pixels.
        auto pixel = gpu->synthesize(bottom, top, top, 0, 1, false);
        const auto duration = b ? CMTimeAdd(a.duration, b->duration) : a.duration;
        exact_rff(duration);
        const FrameCadence cadence{true, true, 0, a.parity == 0 ? FieldOrder::tff : FieldOrder::bff, true, next_boundary || !b};
        ready.push_back(std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel), a.pts, duration, cadence}));
        next_boundary = !b;
    }
    void flush_pending(const char* reason) {
        if (!pending) return;
        std::cerr << "vtenc: RFF " << reason << "; preserving the unmatched field interval using its source picture\n";
        emit(*pending, nullptr);
        pending.reset();
    }
    void fill() {
        if (!started) {
            current = source->next();
            if (current) following = source->next();
            started = true;
        }
        if (!current) { flush_pending("end of input"); return; }
        exact_rff(current->pts);
        auto duration = following ? CMTimeSubtract(following->pts, current->pts) : current->duration;
        if (!following && (!CMTIME_IS_NUMERIC(duration) || duration.value == 0) && CMTIME_IS_NUMERIC(previous_interval)) {
            // In a soft cadence the previous coded picture can have a different
            // field count. A missing last duration is therefore not inferable.
            if (current->cadence.repeat_fields || pending)
                throw std::runtime_error("Final RFF picture has no duration; cannot infer its field interval from the previous coded picture");
            duration = previous_interval;
            std::cerr << "vtenc: IVTC input omits final frame duration; using preceding PTS interval "
                      << duration.value << '/' << duration.timescale << " seconds\n";
        }
        exact_rff(duration);
        if (duration.value <= 0) throw std::runtime_error("RFF source PTS must increase and durations must be positive");
        current->duration = duration;
        previous_interval = duration;
        const auto repeat = current->cadence.repeat_fields;
        if ((config.ivtc.mixed || repeat != 0) && !current->cadence.known)
            throw std::runtime_error("IVTC RFF/mixed mode requires per-picture cadence metadata from the decoder");
        if (repeat != 0 && repeat != 1 && repeat != 2 && repeat != 4)
            throw std::runtime_error("Unsupported repeat-field count=" + std::to_string(repeat) + " at coded frame=" + std::to_string(index));
        if (repeat && config.ivtc.expand == RffExpansion::off) {
            if (!config.ivtc.mixed) throw std::runtime_error("RFF input with expand=off requires mixed=true to preserve coded-picture intervals");
        }
        if (current->cadence.boundary) {
            flush_pending("source boundary");
            next_boundary = true;
        }
        const bool expanding = config.ivtc.expand != RffExpansion::off && (repeat || pending);
        if (expanding) {
            unsigned first = 0;
            auto order = current->cadence.first;
            if (order == FieldOrder::automatic) order = config.field_order;
            if (order == FieldOrder::automatic && repeat == 1)
                throw std::runtime_error("RFF has no first-field order; specify --field-order tff|bff");
            // Full progressive frame repetition is field-order invariant.
            first = order == FieldOrder::bff ? 1U : 0U;
            // A progressive, non-RFF picture has no distinct field exposures.
            // Its absent/default first-field flag must not break a pending pair.
            if (pending && !current->cadence.interlaced && repeat != 1) first = pending->parity ^ 1U;
            if (pending && pending->parity == first) flush_pending("field-order discontinuity");
            const auto interval = ivtc_divide(duration, 2 + repeat);
            auto pts = current->pts;
            for (int n = 0; n < 2 + repeat; ++n) {
                Field field{current, first ^ (static_cast<unsigned>(n) & 1U), pts, interval};
                if (pending) { emit(*pending, &field); pending.reset(); }
                else pending.emplace(std::move(field));
                pts = CMTimeAdd(pts, interval); exact_rff(pts);
            }
            const auto end = CMTimeAdd(current->pts, duration); exact_rff(end);
            if (CMTimeCompare(pts, end)) throw std::runtime_error("RFF expansion changed the coded-picture interval");
        } else {
            CFRetain(current->pixel.get());
            auto cadence = current->cadence; cadence.boundary |= next_boundary;
            ready.push_back(std::make_unique<DecodedFrame>(DecodedFrame{CFRef(current->pixel.get()), current->pts, duration, cadence}));
            next_boundary = false;
        }
        if (config.ivtc.log) std::cerr << "vtenc: rff coded=" << index << " repeat_fields=" << repeat
            << " expanded=" << expanding << " pending=" << pending.has_value() << '\n';
        ++index;
        current = std::move(following);
        // Release the consumed source before asking the bounded decoder pool
        // for another frame. Only a pending field may retain it.
        if (current) following = source->next();
    }
};
RffFrames::RffFrames(std::unique_ptr<DecodedFrames> source, const FilterConfig& config,
                     const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(std::move(source), config, cancelled)) {}
RffFrames::~RffFrames() = default;
std::unique_ptr<DecodedFrame> RffFrames::next() {
    auto& s = *impl_;
    if (s.cancelled) return nullptr;
    if (s.ready.empty()) s.fill();
    if (s.cancelled || s.ready.empty()) return nullptr;
    auto frame = std::move(s.ready.front()); s.ready.pop_front(); return frame;
}
}
