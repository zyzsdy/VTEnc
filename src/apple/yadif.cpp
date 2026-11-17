// Three-frame scheduling based on NVEncFilterYadif.cu (rigaya, MIT).
// Exact VFR/RFF intervals and section boundaries are VTEnc-specific.
#include "yadif.hpp"
#include "ivtc.hpp"
#include "bob.hpp"
#include <limits>

namespace vtenc {
std::int32_t yadif_output_timescale(std::int32_t source, bool bob) {
    // RFF splits by 2/3/4/6, then optionally split the assembled raster in half.
    const int factor = bob ? 24 : 12;
    if (source <= 0 || source > std::numeric_limits<std::int32_t>::max() / factor)
        throw std::runtime_error("YADIF/RFF output timescale exceeds CoreMedia's positive 32-bit range");
    return source * factor;
}
struct YadifFrames::Impl {
    std::unique_ptr<DecodedFrames> source;
    CFRef<CMFormatDescriptionRef> format;
    FilterConfig config;
    const volatile std::sig_atomic_t& cancelled;
    std::unique_ptr<DecodedFrame> previous, current, following;
    std::unique_ptr<GpuYadif> gpu;
    bool started = false;
    unsigned output_index = 0;
    Impl(std::unique_ptr<DecodedFrames> input, CMFormatDescriptionRef description,
         const FilterConfig& settings, const volatile std::sig_atomic_t& stop)
        : source(std::move(input)), format((CFRetain(description), description)), config(settings), cancelled(stop) {}
    bool progressive(const DecodedFrame& f) const {
        // A paired bitstream has authoritative spatial parity, even with an override.
        if (f.cadence.assembled_fields && f.cadence.orphan_field) return true;
        return config.field_order == FieldOrder::automatic && f.cadence.known &&
            !f.cadence.interlaced && !f.cadence.expanded;
    }
    unsigned first(const DecodedFrame& f) const {
        const auto& c = f.cadence;
        if (c.expanded || c.assembled_fields) {
            if (c.first == FieldOrder::automatic) throw std::runtime_error("YADIF assembled raster lacks field parity");
            return c.first == FieldOrder::bff ? 1U : 0U;
        }
        if (config.field_order != FieldOrder::automatic) return config.field_order == FieldOrder::bff ? 1U : 0U;
        if (progressive(f)) return 0;
        if (c.known && c.first != FieldOrder::automatic) return c.first == FieldOrder::bff ? 1U : 0U;
        try { return bob_first_field(FieldOrder::automatic, format.get(), f.pixel.get()); }
        catch (const std::runtime_error& e) { throw std::runtime_error(std::string("YADIF field order: ") + e.what()); }
    }
    bool compatible(const DecodedFrame& a, const DecodedFrame& b) const {
        return progressive(a) == progressive(b) && first(a) == first(b) &&
            !a.cadence.orphan_field && !b.cadence.orphan_field;
    }
    std::unique_ptr<DecodedFrame> next() {
        if (cancelled) return nullptr;
        if (!started) {
            current = source->next();
            if (current) following = source->next();
            started = true;
        }
        if (!current || cancelled) return nullptr;
        const auto cur = current->pixel.get();
        if (!gpu) gpu = std::make_unique<GpuYadif>(cur);
        const auto prev = previous && !current->cadence.boundary && compatible(*previous, *current)
            ? previous->pixel.get() : cur;
        const auto next = following && !following->cadence.boundary && compatible(*current, *following)
            ? following->pixel.get() : cur;
        const bool doubled = config.yadif.bob && !current->cadence.orphan_field;
        const auto timing = doubled ? bob_time(current->pts, current->duration, output_index)
                                    : BobTime{current->pts, current->duration};
        auto pixel = gpu->process(prev, cur, next, first(*current) ^ output_index,
            output_index != 0, progressive(*current), current->cadence.orphan_field);
        FrameCadence cadence{true, false};
        cadence.boundary = current->cadence.boundary && output_index == 0;
        auto result = std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel), timing.pts, timing.duration, cadence});
        if (doubled && output_index == 0) output_index = 1;
        else {
            previous = std::move(current);
            current = std::move(following);
            // Release the oldest source before advancing the bounded decode pool.
            if (current) following = source->next();
            output_index = 0;
        }
        return cancelled ? nullptr : std::move(result);
    }
};
YadifFrames::YadifFrames(std::unique_ptr<DecodedFrames> source, CMFormatDescriptionRef format,
                         const FilterConfig& config, const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(std::make_unique<RffFrames>(
          std::make_unique<FieldFrames>(std::move(source), config, cancelled), config, cancelled), format, config, cancelled)) {}
YadifFrames::~YadifFrames() = default;
std::unique_ptr<DecodedFrame> YadifFrames::next() { return impl_->next(); }
}
