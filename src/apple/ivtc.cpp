// Hard-telecine matching/decimation adapted from NVEnc, MIT, rigaya.
// See THIRD_PARTY_NOTICES.md for upstream revision and license.
#include "ivtc.hpp"
#include "bob.hpp"
#include <algorithm>
#include <deque>
#include <iostream>
#include <limits>
#include <numeric>
#include <tuple>
#include <vector>

namespace vtenc {
namespace {
void exact(CMTime time) {
    if (!CMTIME_IS_NUMERIC(time) || time.epoch != 0 || CMTIME_HAS_BEEN_ROUNDED(time))
        throw std::runtime_error("IVTC requires finite, exact timestamps with epoch zero");
}
}
unsigned ivtc_select(const IvtcScores& scores) {
    // Upstream primary-parity comb-first ordering; deterministic ties prefer C.
    const auto rank = [](const IvtcScore& s) { return std::tie(s.blocks, s.maximum, s.comb, s.match); };
    unsigned best = 0;
    for (unsigned i = 1; i < scores.size(); ++i) if (rank(scores[i]) < rank(scores[best])) best = i;
    return best;
}
CMTime ivtc_divide(CMTime duration, int divisor) {
    exact(duration);
    if (duration.value <= 0 || divisor <= 0) throw std::runtime_error("IVTC requires positive duration and divisor");
    const auto common = std::gcd(duration.value, static_cast<std::int64_t>(divisor));
    const auto factor = divisor / common;
    if (duration.timescale > std::numeric_limits<std::int32_t>::max() / factor)
        throw std::runtime_error("IVTC duration cannot be divided exactly within CoreMedia timescale");
    return CMTimeMake(duration.value / common, static_cast<std::int32_t>(duration.timescale * factor));
}
CMTime ivtc_quarter(CMTime duration) { return ivtc_divide(duration, 4); }
std::int32_t ivtc_output_timescale(std::int32_t source, const IvtcConfig& config) {
    // lcm(2,3,4,6) for field expansion, then four-way cycle redistribution.
    const int factor = (config.expand == RffExpansion::off ? 1 : 12) * (config.cycle == 5 ? 4 : 1);
    if (source <= 0 || source > std::numeric_limits<std::int32_t>::max() / factor)
        throw std::runtime_error("IVTC/RFF output timescale exceeds CoreMedia's positive 32-bit range");
    return source * factor;
}
struct IvtcFrames::Impl {
    std::unique_ptr<DecodedFrames> source;
    CFRef<CMFormatDescriptionRef> format;
    FilterConfig config;
    const volatile std::sig_atomic_t& cancelled;
    std::unique_ptr<GpuIvtc> gpu;
    std::unique_ptr<DecodedFrame> previous, current, following;
    std::optional<CFRef<CVPixelBufferRef>> last_matched;
    std::deque<std::unique_ptr<DecodedFrame>> ready;
    CMTime previous_interval = kCMTimeInvalid;
    std::optional<unsigned> field;
    bool started = false;
    unsigned last_match = 0;
    std::uint64_t input_index = 0;
    Impl(std::unique_ptr<DecodedFrames> input, CMFormatDescriptionRef description,
         const FilterConfig& settings, const volatile std::sig_atomic_t& stop)
        : source(std::move(input)), format((CFRetain(description), description)), config(settings), cancelled(stop) {}
    void fill() {
        if (!started) {
            current = source->next();
            if (current) following = source->next();
            started = true;
        }
        std::vector<std::unique_ptr<DecodedFrame>> cycle;
        std::vector<std::uint64_t> differences;
        bool expanded = false, telecine = false, residual = false;
        const auto length = config.ivtc.cycle == 0 ? 1U : 5U;
        while (current && cycle.size() < length && !cancelled) {
            if (config.ivtc.mixed && !current->cadence.known)
                throw std::runtime_error("IVTC mixed mode requires per-picture cadence metadata from the decoder");
            if (current->cadence.boundary) {
                if (!cycle.empty()) break;
                previous.reset(); last_matched.reset(); field.reset(); last_match = 0;
            }
            exact(current->pts);
            auto duration = following ? CMTimeSubtract(following->pts, current->pts) : current->duration;
            if (!following && (!CMTIME_IS_NUMERIC(duration) || duration.value == 0) && CMTIME_IS_NUMERIC(previous_interval)) {
                duration = previous_interval;
                std::cerr << "vtenc: IVTC input omits final frame duration; using preceding PTS interval "
                          << duration.value << '/' << duration.timescale << " seconds\n";
            }
            exact(duration);
            if (duration.value <= 0) throw std::runtime_error("IVTC source PTS must strictly increase and frame duration must be positive");
            previous_interval = duration;
            const auto cur = current->pixel.get();
            auto prev = previous ? previous->pixel.get() : cur;
            auto next = following && !following->cadence.boundary ? following->pixel.get() : cur;
            unsigned first = 0;
            const auto& cadence = current->cadence;
            const bool progressive = cadence.known && !cadence.interlaced && !cadence.expanded;
            if (cadence.expanded) first = cadence.first == FieldOrder::bff ? 1U : 0U;
            else if (config.field_order != FieldOrder::automatic) first = config.field_order == FieldOrder::bff ? 1U : 0U;
            else if (progressive) first = field.value_or(0U); // C is invariant to parity.
            else if (cadence.known && cadence.first != FieldOrder::automatic) first = cadence.first == FieldOrder::bff ? 1U : 0U;
            else {
                try { first = bob_first_field(config.field_order, format.get(), cur); }
                catch (const std::runtime_error& error) { throw std::runtime_error(std::string("IVTC field order: ") + error.what()); }
            }
            if (field && *field != first) {
                if (!config.ivtc.mixed) throw std::runtime_error("IVTC field order changed; specify --field-order tff|bff or mixed=true");
                if (!cycle.empty()) break;
                previous.reset(); last_matched.reset(); last_match = 0; prev = cur;
            }
            field = first;
            if (config.ivtc.mixed && following) {
                const auto& upcoming = following->cadence;
                std::optional<unsigned> next_first;
                if (upcoming.expanded) next_first = upcoming.first == FieldOrder::bff ? 1U : 0U;
                else if (config.field_order != FieldOrder::automatic) next_first = config.field_order == FieldOrder::bff ? 1U : 0U;
                else if (upcoming.known && upcoming.interlaced && upcoming.first != FieldOrder::automatic)
                    next_first = upcoming.first == FieldOrder::bff ? 1U : 0U;
                // The next fill drains/resets on a parity change. The last
                // picture of this section must not borrow across it either.
                if (next_first && *next_first != first) next = cur;
            }
            if (!gpu) gpu = std::make_unique<GpuIvtc>(cur, config.ivtc);
            // A hard scene cut must not lend a field or temporal repair pixels.
            const auto type = CVPixelBufferGetPixelFormatType(cur);
            const bool ten = type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
            const auto scene_limit = static_cast<std::uint64_t>(CVPixelBufferGetWidth(cur)) * CVPixelBufferGetHeight(cur) * (ten ? 1023U : 255U) / 5;
            if (previous && gpu->difference(prev, cur) > scene_limit) { prev = cur; last_match = 0; }
            if (following && gpu->difference(cur, next) > scene_limit) next = cur;
            const auto scores = gpu->score(prev, cur, next, first);
            // At a cut-in, N would consume the next film exposure and add a
            // second duplicate to the first five-frame cycle. Keep the first
            // source exposure; post=2 repairs its unavailable opposite field.
            auto match = previous ? ivtc_select(scores) : 0U;
            // Upstream's N -> P guard: those two decisions produce the exact
            // same field pair, potentially consuming a second drop in a cycle.
            if (previous && last_match == 2 && match == 1) {
                auto candidates = scores;
                candidates[1].blocks = std::numeric_limits<std::uint64_t>::max();
                match = ivtc_select(candidates);
            }
            // The converse P -> N transition skips a film exposure when both
            // adjacent field matches are clean (compression noise can reverse
            // their rank). Keep P through that ambiguity; a combed P releases it.
            if (previous && last_match == 1 && match == 2 && scores[1].maximum < 65 &&
                scores[1].comb <= scores[2].comb + 6) match = 1;
            const bool direct = config.ivtc.mixed && ((progressive && scores[0].maximum < 65) ||
                (cadence.repeat_fields > 0 && config.ivtc.expand == RffExpansion::off));
            if (direct) match = 0;
            expanded |= cadence.expanded;
            telecine |= !direct && (cadence.interlaced || scores[0].maximum >= 65);
            residual |= scores[match].maximum >= 65;
            last_match = match;
            // Conservative block gate; per-pixel residual gate remains in the kernel.
            const bool repair = !direct && config.ivtc.post == 2 && scores[match].maximum >= 65;
            auto matched = gpu->synthesize(prev, cur, next, first, match, repair);
            differences.push_back(last_matched ? gpu->difference(last_matched->get(), matched.get()) : std::numeric_limits<std::uint64_t>::max());
            if (config.ivtc.log) {
                std::cerr << "vtenc: ivtc frame=" << input_index << " match=" << "CPN"[match] << " repair=" << repair
                          << " section=" << (cadence.expanded ? "rff" : direct ? "progressive" : "match")
                          << " sad=" << differences.back();
                for (const auto& score : scores) std::cerr << " [" << score.blocks << ',' << score.maximum << ',' << score.comb << ',' << score.match << ']';
                std::cerr << '\n';
            }
            ++input_index;
            last_matched.reset();
            CFRetain(matched.get()); last_matched.emplace(matched.get());
            cycle.push_back(std::make_unique<DecodedFrame>(DecodedFrame{std::move(matched), current->pts, duration}));
            previous = std::move(current);
            current = std::move(following);
            if (current) following = source->next();
        }
        if (cancelled || cycle.empty()) return;
        int drop = -1;
        if (config.ivtc.cycle == 5 && cycle.size() == 5) {
            const auto minimum = *std::min_element(differences.begin(), differences.end());
            std::uint64_t largest = 0;
            for (auto value : differences) if (value != std::numeric_limits<std::uint64_t>::max()) largest = std::max(largest, value);
            const auto pixel = cycle.front()->pixel.get();
            const auto type = CVPixelBufferGetPixelFormatType(pixel);
            const bool ten = type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
            const auto duplicate_limit = static_cast<std::uint64_t>(CVPixelBufferGetWidth(pixel)) * CVPixelBufferGetHeight(pixel) * (ten ? 1023U : 255U) / 200;
            // Mixed video must not lose a unique progressive/interlaced exposure.
            // Require a clean match and a convincing duplicate; explicit hard
            // mode retains the original unconditional five-to-four behavior.
            const bool duplicate = minimum <= duplicate_limit && (expanded || (largest > 0 && minimum <= largest / 5));
            if (!config.ivtc.mixed || (!residual && (expanded || telecine) && duplicate))
                drop = static_cast<int>(std::min_element(differences.begin(), differences.end()) - differences.begin());
        }
        if (drop >= 0) {
            const auto start = cycle.front()->pts;
            const auto end = CMTimeAdd(cycle.back()->pts, cycle.back()->duration);
            exact(end);
            const auto duration = ivtc_quarter(CMTimeSubtract(end, start));
            auto pts = start;
            for (std::size_t i = 0; i < cycle.size(); ++i) {
                if (static_cast<int>(i) == drop) continue;
                cycle[i]->pts = pts; cycle[i]->duration = duration;
                pts = CMTimeAdd(pts, duration); exact(pts);
            }
            if (CMTimeCompare(pts, end) != 0) throw std::runtime_error("IVTC cycle timeline cannot be represented exactly");
        }
        if (config.ivtc.log) std::cerr << "vtenc: ivtc cycle_start=" << input_index - cycle.size()
                                      << " count=" << cycle.size() << " drop=" << drop << '\n';
        // Partial EOF cycles keep all frames and their original intervals.
        for (std::size_t i = 0; i < cycle.size(); ++i) if (static_cast<int>(i) != drop) ready.push_back(std::move(cycle[i]));
    }
};
IvtcFrames::IvtcFrames(std::unique_ptr<DecodedFrames> source, CMFormatDescriptionRef format,
                     const FilterConfig& config, const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(std::make_unique<RffFrames>(std::move(source), config, cancelled), format, config, cancelled)) {}
IvtcFrames::~IvtcFrames() = default;
std::unique_ptr<DecodedFrame> IvtcFrames::next() {
    auto& s = *impl_;
    if (s.cancelled) return nullptr;
    if (s.ready.empty()) s.fill();
    if (s.cancelled || s.ready.empty()) return nullptr;
    auto frame = std::move(s.ready.front()); s.ready.pop_front(); return frame;
}
} // namespace vtenc
