#include "ivtc.hpp"
#include <iostream>

namespace vtenc {
namespace {
void field_time(CMTime value) {
    if (!CMTIME_IS_NUMERIC(value) || value.epoch || CMTIME_HAS_BEEN_ROUNDED(value))
        throw std::runtime_error("HEVC field pairing requires exact finite times with epoch zero");
}
}
struct FieldFrames::Impl {
    std::unique_ptr<DecodedFrames> source;
    const volatile std::sig_atomic_t& cancelled;
    bool log, started = false, boundary = false, have_pair = false;
    FieldOrder previous_first = FieldOrder::automatic;
    FieldPairing previous_tail = FieldPairing::unspecified;
    std::unique_ptr<DecodedFrame> current, following, pending;
    std::unique_ptr<GpuFieldPair> gpu;
    std::uint64_t index = 0;
    Impl(std::unique_ptr<DecodedFrames> input, const FilterConfig& config, const volatile std::sig_atomic_t& stop)
        : source(std::move(input)), cancelled(stop), log(config.ivtc.log) {}
    void advance() {
        current = std::move(following);
        if (current) following = source->next();
    }
    std::unique_ptr<DecodedFrame> emit(bool paired) {
        auto a = std::move(pending);
        if (!gpu) gpu = std::make_unique<GpuFieldPair>(a->pixel.get());
        auto pixel = gpu->process(a->pixel.get(), paired ? current->pixel.get() : nullptr,
            a->cadence.first == FieldOrder::bff ? 1U : 0U);
        auto cadence = a->cadence;
        // A hint on the complementary field expresses the same association.
        // Do not lose it just because the leading field has pic_struct=1/2.
        if (paired && cadence.pairing == FieldPairing::unspecified) {
            if (have_pair && previous_first == cadence.first && previous_tail == FieldPairing::next)
                cadence.pairing = FieldPairing::previous;
            else if (current->cadence.pairing == FieldPairing::previous)
                cadence.pairing = FieldPairing::next;
        }
        cadence.single_field = false;
        cadence.assembled_fields = true;
        cadence.orphan_field = !paired;
        cadence.interlaced = paired;
        cadence.boundary |= boundary || !paired;
        if (!paired) cadence.pairing = FieldPairing::unspecified;
        const auto duration = paired ? CMTimeAdd(a->duration, current->duration) : a->duration;
        field_time(duration);
        if (log) std::cerr << "vtenc: hevc-fields pair=" << index++ << " first="
            << (cadence.first == FieldOrder::bff ? "bottom" : "top") << " orphan=" << !paired
            << " hint=" << static_cast<int>(cadence.pairing) << " duplicate=" << cadence.duplicate << '\n';
        boundary = !paired;
        have_pair = paired;
        previous_first = cadence.first;
        previous_tail = paired ? current->cadence.pairing : FieldPairing::unspecified;
        if (paired) { current.reset(); advance(); }
        return std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel), a->pts, duration, cadence});
    }
    std::unique_ptr<DecodedFrame> orphan(const char* reason) {
        std::cerr << "vtenc: HEVC single field " << reason
                  << "; spatially reconstructing missing lines and preserving its display interval\n";
        return emit(false);
    }
    std::unique_ptr<DecodedFrame> next() {
        if (!started) { current = source->next(); if (current) following = source->next(); started = true; }
        while (!cancelled) {
            if (!current) return pending ? orphan("at end of input") : nullptr;
            if (!current->cadence.single_field) {
                if (pending) return orphan("before a frame picture");
                current->cadence.boundary |= boundary;
                boundary = false; have_pair = false;
                auto result = std::move(current); advance(); return result;
            }
            if (!current->cadence.known || current->cadence.first == FieldOrder::automatic || current->cadence.repeat_fields)
                throw std::runtime_error("Invalid HEVC single-field cadence");
            field_time(current->pts);
            const auto duration = following ? CMTimeSubtract(following->pts, current->pts) : current->duration;
            field_time(duration);
            if (duration.value <= 0) throw std::runtime_error("HEVC field PTS must increase and field durations must be positive");
            current->duration = duration;
            if (pending) {
                if (current->cadence.boundary || current->cadence.first == pending->cadence.first)
                    return orphan("at a field-order discontinuity");
                return emit(true);
            }
            // A repeated parity can fall between two pairs, not just inside
            // a pending pair. Flush the IVTC cycle on that edit as well.
            if (have_pair && current->cadence.first != previous_first) {
                std::cerr << "vtenc: HEVC field-order discontinuity between pairs; starting a new matching section\n";
                boundary = true; have_pair = false;
            }
            if (current->cadence.boundary) { boundary = true; have_pair = false; }
            pending = std::move(current); advance();
            if (!have_pair && pending->cadence.pairing == FieldPairing::previous)
                return orphan("without its preceding complementary field");
        }
        return nullptr;
    }
};
FieldFrames::FieldFrames(std::unique_ptr<DecodedFrames> source, const FilterConfig& config,
                         const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(std::move(source), config, cancelled)) {}
FieldFrames::~FieldFrames() = default;
std::unique_ptr<DecodedFrame> FieldFrames::next() { return impl_->next(); }
}
