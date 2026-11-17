#include "ffmpeg_utils.hpp"
#include "cf_ref.hpp"
#include "encoder_session.hpp"
#include "decode_policy.hpp"

extern "C" {
#include <libavutil/display.h>
}
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace vtenc {
void PacketDeleter::operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
namespace {
using Bytes = std::span<const std::uint8_t>;
std::vector<Bytes> annex_b(Bytes bytes) {
    std::vector<Bytes> result;
    const auto start = [&](std::size_t offset) -> std::pair<std::size_t, std::size_t> {
        for (auto i = offset; i + 3 <= bytes.size(); ++i) {
            if (bytes[i] || bytes[i + 1]) continue;
            if (bytes[i + 2] == 1) return {i, 3};
            if (i + 4 <= bytes.size() && bytes[i + 2] == 0 && bytes[i + 3] == 1) return {i, 4};
        }
        return {bytes.size(), 0};
    };
    auto [position, prefix] = start(0);
    if (!prefix) throw std::runtime_error("Missing Annex B start code");
    for (std::size_t i = 0; i < position; ++i) {
        if (bytes[i]) throw std::runtime_error("Invalid bytes before Annex B start code");
    }
    while (prefix) {
        const auto begin = position + prefix;
        const auto next = start(begin);
        auto end = next.first;
        while (end > begin && bytes[end - 1] == 0) --end; // trailing_zero_8bits
        if (end <= begin) throw std::runtime_error("Empty Annex B NAL unit");
        result.push_back(bytes.subspan(begin, end - begin));
        position = next.first;
        prefix = next.second;
    }
    return result;
}
std::vector<Bytes> length_prefixed(Bytes bytes, unsigned length_size) {
    std::vector<Bytes> result;
    while (!bytes.empty()) {
        if (bytes.size() < length_size) throw std::runtime_error("Truncated NAL length");
        std::size_t size = 0;
        for (unsigned i = 0; i < length_size; ++i) size = (size << 8) | bytes[i];
        bytes = bytes.subspan(length_size);
        if (!size || size > bytes.size()) throw std::runtime_error("Invalid compressed NAL length");
        result.push_back(bytes.first(size));
        bytes = bytes.subspan(size);
    }
    return result;
}
CFRef<CFMutableDictionaryRef> metadata(const AVCodecParameters* p) {
    CFRef result(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto set = [&](CFStringRef key, CFTypeRef value) { if (value) CFDictionarySetValue(result.get(), key, value); };
    const auto number = [&](CFStringRef key, int value) {
        CFRef n(CFNumberCreate(nullptr, kCFNumberIntType, &value)); set(key, n.get());
    };
    if (p->color_primaries != AVCOL_PRI_UNSPECIFIED) set(kCVImageBufferColorPrimariesKey, CVColorPrimariesGetStringForIntegerCodePoint(p->color_primaries));
    if (p->color_trc != AVCOL_TRC_UNSPECIFIED) set(kCVImageBufferTransferFunctionKey, CVTransferFunctionGetStringForIntegerCodePoint(p->color_trc));
    if (p->color_space != AVCOL_SPC_UNSPECIFIED) set(kCVImageBufferYCbCrMatrixKey, CVYCbCrMatrixGetStringForIntegerCodePoint(p->color_space));
    if (p->color_range != AVCOL_RANGE_UNSPECIFIED) set(kCMFormatDescriptionExtension_FullRangeVideo, p->color_range == AVCOL_RANGE_JPEG ? kCFBooleanTrue : kCFBooleanFalse);
    CFStringRef chroma = nullptr;
    switch (p->chroma_location) {
    case AVCHROMA_LOC_LEFT: chroma = kCVImageBufferChromaLocation_Left; break;
    case AVCHROMA_LOC_CENTER: chroma = kCVImageBufferChromaLocation_Center; break;
    case AVCHROMA_LOC_TOPLEFT: chroma = kCVImageBufferChromaLocation_TopLeft; break;
    case AVCHROMA_LOC_TOP: chroma = kCVImageBufferChromaLocation_Top; break;
    case AVCHROMA_LOC_BOTTOMLEFT: chroma = kCVImageBufferChromaLocation_BottomLeft; break;
    case AVCHROMA_LOC_BOTTOM: chroma = kCVImageBufferChromaLocation_Bottom; break;
    default: break;
    }
    set(kCVImageBufferChromaLocationTopFieldKey, chroma);
    set(kCVImageBufferChromaLocationBottomFieldKey, chroma);
    if (p->field_order != AV_FIELD_UNKNOWN) {
        number(kCMFormatDescriptionExtension_FieldCount, p->field_order == AV_FIELD_PROGRESSIVE ? 1 : 2);
        if (p->field_order != AV_FIELD_PROGRESSIVE) {
            set(kCMFormatDescriptionExtension_FieldDetail, p->field_order == AV_FIELD_TT || p->field_order == AV_FIELD_BT
                ? kCVImageBufferFieldDetailTemporalTopFirst : kCVImageBufferFieldDetailTemporalBottomFirst);
        }
    }
    if (p->sample_aspect_ratio.num > 0 && p->sample_aspect_ratio.den > 0) {
        CFRef ratio(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CFRef x(CFNumberCreate(nullptr, kCFNumberIntType, &p->sample_aspect_ratio.num));
        CFRef y(CFNumberCreate(nullptr, kCFNumberIntType, &p->sample_aspect_ratio.den));
        CFDictionarySetValue(ratio.get(), kCVImageBufferPixelAspectRatioHorizontalSpacingKey, x.get());
        CFDictionarySetValue(ratio.get(), kCVImageBufferPixelAspectRatioVerticalSpacingKey, y.get());
        set(kCVImageBufferPixelAspectRatioKey, ratio.get());
    }
    return result;
}
}

struct ContainerInput::Impl {
    AVFormatContext* demux = nullptr;
    int video = -1, audio = -1;
    bool eof = false;
    std::deque<PacketPtr> videos, audios;
    std::size_t queued_bytes = 0;
    const volatile std::sig_atomic_t& cancelled;
    MediaInfo info;
    std::optional<CFRef<CMVideoFormatDescriptionRef>> format;
    bool hardware_format_ready = false;
    bool annex = false;
    unsigned nal_length = 4;
    explicit Impl(const volatile std::sig_atomic_t& stop) : cancelled(stop) {}
    ~Impl() { avformat_close_input(&demux); }
    static int interrupt(void* opaque) noexcept { return static_cast<Impl*>(opaque)->cancelled != 0; }
    void check(int code, const char* context) const {
        if (cancelled) throw MediaCancelled{};
        av_check(code, context);
    }
    PacketPtr read(int track) {
        if (track < 0) return {};
        auto& queue = track == video ? videos : audios;
        while (queue.empty() && !eof) {
            if (cancelled) throw MediaCancelled{};
            auto packet = make_packet();
            const auto status = av_read_frame(demux, packet.get());
            if (status == AVERROR_EOF) { eof = true; break; }
            check(status, "av_read_frame");
            if (packet->stream_index != video && packet->stream_index != audio) continue;
            if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("avformat returned a corrupt input packet");
            if (packet->size <= 0) throw std::runtime_error("avformat returned an empty input packet");
            if (av_packet_get_side_data(packet.get(), AV_PKT_DATA_PARAM_CHANGE, nullptr)) {
                throw std::runtime_error("Input stream parameters changed mid-stream");
            }
            // Bounded interleaving: decoding can read ahead for B frames, but
            // arbitrarily separated tracks must not grow memory without limit.
            queued_bytes += static_cast<std::size_t>(packet->size);
            if (queued_bytes > 64U * 1024U * 1024U || videos.size() + audios.size() >= 4096) {
                throw std::runtime_error("Input interleaving exceeds the 64 MiB / 4096 packet queue limit");
            }
            (packet->stream_index == video ? videos : audios).push_back(std::move(packet));
        }
        if (queue.empty()) return {};
        auto result = std::move(queue.front());
        queue.pop_front();
        queued_bytes -= static_cast<std::size_t>(result->size);
        return result;
    }
    void describe_hardware() {
        if (hardware_format_ready) return;
        const auto* p = demux->streams[video]->codecpar;
        auto extensions = metadata(p);
        const Bytes extra(p->extradata, static_cast<std::size_t>(p->extradata_size));
        CMVideoFormatDescriptionRef raw = nullptr;
        if (p->codec_id == AV_CODEC_ID_AV1) {
            if (extra.size() < 4 || extra[0] != 0x81) throw HardwareDecodeError("Missing AV1 av1C configuration for native hardware decoder");
            CFRef data(CFDataCreate(nullptr, extra.data(), static_cast<CFIndex>(extra.size())));
            const void* keys[] = {CFSTR("av1C")}; const void* values[] = {data.get()};
            CFRef atoms(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            CFDictionarySetValue(extensions.get(), kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms.get());
            check_status(CMVideoFormatDescriptionCreate(nullptr, kCMVideoCodecType_AV1, p->width, p->height, extensions.get(), &raw), "Describe AV1 hardware input");
        } else if (p->codec_id == AV_CODEC_ID_H264 || p->codec_id == AV_CODEC_ID_HEVC) {
            std::vector<Bytes> sets;
            annex = extra.size() >= 3 && extra[0] == 0 && extra[1] == 0;
            if (annex) {
                for (auto unit : annex_b(extra)) {
                    const auto type = p->codec_id == AV_CODEC_ID_H264 ? unit[0] & 31 : (unit[0] >> 1) & 63;
                    if ((p->codec_id == AV_CODEC_ID_H264 && (type == 7 || type == 8)) ||
                        (p->codec_id == AV_CODEC_ID_HEVC && (type == 32 || type == 33 || type == 34))) sets.push_back(unit);
                }
            } else {
                std::size_t position = 0;
                const auto byte = [&]() { if (position >= extra.size()) throw std::runtime_error("Truncated codec configuration"); return extra[position++]; };
                const auto take_set = [&]() {
                    const auto high = byte(); const auto low = byte();
                    const auto count = (static_cast<std::size_t>(high) << 8) | low;
                    if (!count || count > extra.size() - position) throw std::runtime_error("Invalid parameter-set length");
                    sets.push_back(extra.subspan(position, count)); position += count;
                };
                if (p->codec_id == AV_CODEC_ID_H264) {
                    if (extra.size() < 7 || extra[0] != 1) throw std::runtime_error("Missing H.264 avcC configuration");
                    nal_length = (extra[4] & 3U) + 1; position = 5;
                    const auto sps = byte() & 31;
                    for (int i = 0; i < sps; ++i) take_set();
                    const auto pps = byte();
                    for (int i = 0; i < pps; ++i) take_set();
                } else {
                    if (extra.size() < 23 || extra[0] != 1) throw std::runtime_error("Missing HEVC hvcC configuration");
                    nal_length = (extra[21] & 3U) + 1; position = 22;
                    const auto arrays = byte();
                    for (int i = 0; i < arrays; ++i) {
                        const auto type = byte() & 63;
                        const auto high = byte(); const auto low = byte();
                        const auto count = (static_cast<unsigned>(high) << 8) | low;
                        for (unsigned j = 0; j < count; ++j) {
                            take_set();
                            if (type != 32 && type != 33 && type != 34) sets.pop_back();
                        }
                    }
                }
            }
            std::vector<const std::uint8_t*> pointers;
            std::vector<std::size_t> sizes;
            for (auto set : sets) { pointers.push_back(set.data()); sizes.push_back(set.size()); }
            if (sets.empty()) throw std::runtime_error("No video parameter sets for hardware decoding");
            if (p->codec_id == AV_CODEC_ID_H264) {
                check_status(CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, sets.size(), pointers.data(), sizes.data(), 4, &raw), "Describe H.264 hardware input");
            } else {
                check_status(CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, sets.size(), pointers.data(), sizes.data(), 4, extensions.get(), &raw), "Describe HEVC hardware input");
            }
            CFRef parsed(raw);
            // Keep codec-derived atoms/SPS data while applying container metadata.
            CFRef merged(CFDictionaryCreateMutableCopy(nullptr, 0, CMFormatDescriptionGetExtensions(parsed.get())));
            CFDictionaryApplyFunction(extensions.get(), [](const void* key, const void* value, void* context) {
                CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(context), key, value);
            }, merged.get());
            const auto dimensions = CMVideoFormatDescriptionGetDimensions(parsed.get());
            check_status(CMVideoFormatDescriptionCreate(nullptr, CMFormatDescriptionGetMediaSubType(parsed.get()),
                dimensions.width, dimensions.height, merged.get(), &raw), "Merge hardware video metadata");
        } else {
            throw HardwareDecodeError("No native hardware decode adapter for codec " + std::string(avcodec_get_name(p->codec_id)));
        }
        format.emplace(raw);
        info.video_format = raw;
        hardware_format_ready = true;
    }
};

ContainerInput::ContainerInput(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(cancelled)) {
    auto& s = *impl_;
    s.demux = avformat_alloc_context();
    if (!s.demux) throw std::bad_alloc();
    s.demux->interrupt_callback = {&Impl::interrupt, &s};
    // No format/extension whitelist and no AVFoundation input dependency.
    s.check(avformat_open_input(&s.demux, config.input == "-" ? "pipe:0" : config.input.c_str(), nullptr, nullptr), "avformat_open_input");
    s.check(avformat_find_stream_info(s.demux, nullptr), "avformat_find_stream_info");
    s.info.replayable = s.demux->pb && (s.demux->pb->seekable & AVIO_SEEKABLE_NORMAL);
    for (unsigned i = 0; i < s.demux->nb_streams; ++i) {
        const auto* p = s.demux->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (s.video >= 0) throw std::runtime_error("Input requires exactly one video track");
            s.video = static_cast<int>(i);
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO && s.audio < 0 && config.copy_audio) s.audio = static_cast<int>(i);
    }
    if (s.video < 0) throw std::runtime_error("avformat found no video track");
    const auto* stream = s.demux->streams[s.video];
    const auto* p = stream->codecpar;
    if (p->width <= 0 || p->height <= 0 || stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        throw std::runtime_error("avformat returned invalid video dimensions/time base");
    }
    if (av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF)) {
        throw std::runtime_error("Dolby Vision input is not supported by the SDR pipeline");
    }
    s.info.video_timescale = stream->time_base.den;
    const auto start = stream->start_time == AV_NOPTS_VALUE ? kCMTimeInvalid : media_time(stream->start_time, stream->time_base, "video start");
    const auto duration = stream->duration == AV_NOPTS_VALUE ? kCMTimeInvalid : media_time(stream->duration, stream->time_base, "video duration");
    s.info.video_range = CMTimeRangeMake(start, duration);
    auto extensions = metadata(p);
    CMVideoFormatDescriptionRef raw = nullptr;
    // This describes the source raster; compressed configuration is prepared only
    // for the hardware attempt so unsupported hardware never blocks --avsw.
    check_status(CMVideoFormatDescriptionCreate(nullptr, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        p->width, p->height, extensions.get(), &raw), "Describe avformat source raster");
    s.format.emplace(raw); s.info.video_format = raw;
    if (const auto* display = av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
        if (display->size != 9 * sizeof(std::int32_t)) throw std::runtime_error("Invalid input display matrix");
        std::array<std::int32_t, 9> matrix{};
        std::memcpy(matrix.data(), display->data, display->size);
        if (matrix[2] || matrix[5] || matrix[8] != (1 << 30)) throw std::runtime_error("Perspective display transforms are unsupported");
        s.info.transform = {matrix[0] / 65536.0, matrix[1] / 65536.0, matrix[3] / 65536.0,
                            matrix[4] / 65536.0, matrix[6] / 65536.0, matrix[7] / 65536.0};
    }
    if (s.audio >= 0) {
        const auto* audio = s.demux->streams[s.audio];
        if (audio->time_base.num <= 0 || audio->time_base.den <= 0) throw std::runtime_error("Invalid audio time base");
        s.info.audio_parameters = audio->codecpar;
        s.info.audio_timescale = audio->time_base.den;
        s.info.audio_timebase_num = audio->time_base.num;
    }
}
ContainerInput::~ContainerInput() = default;
const MediaInfo& ContainerInput::info() const { return impl_->info; }
const AVCodecParameters* ContainerInput::video_parameters() const { return impl_->demux->streams[impl_->video]->codecpar; }
AVRational ContainerInput::video_time_base() const { return impl_->demux->streams[impl_->video]->time_base; }
PacketPtr ContainerInput::read_audio() { return impl_->read(impl_->audio); }
PacketPtr ContainerInput::read_video_packet() { return impl_->read(impl_->video); }
CMVideoFormatDescriptionRef ContainerInput::hardware_video_format() try {
    impl_->describe_hardware();
    return impl_->info.video_format;
} catch (const std::runtime_error& error) { throw HardwareDecodeError(error.what()); }
CMSampleBufferRef ContainerInput::read_video() {
    auto packet = read_video_packet();
    if (!packet) return nullptr;
    auto format = hardware_video_format();
    const auto codec = video_parameters()->codec_id;
    std::vector<std::uint8_t> converted;
    Bytes bytes(packet->data, static_cast<std::size_t>(packet->size));
    if (codec == AV_CODEC_ID_H264 || codec == AV_CODEC_ID_HEVC) {
        const auto nals = impl_->annex ? annex_b(bytes) : length_prefixed(bytes, impl_->nal_length);
        for (auto nal : nals) {
            if (nal.size() > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("NAL is too large for VideoToolbox");
            const auto length = static_cast<std::uint32_t>(nal.size());
            for (int shift : {24, 16, 8, 0}) converted.push_back(static_cast<std::uint8_t>(length >> shift));
            converted.insert(converted.end(), nal.begin(), nal.end());
        }
        bytes = converted;
    }
    CMBlockBufferRef raw_block = nullptr;
    check_status(CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes.size(), nullptr, nullptr, 0, bytes.size(), 0, &raw_block), "Allocate demuxed video block");
    CFRef block(raw_block);
    check_status(CMBlockBufferReplaceDataBytes(bytes.data(), block.get(), 0, bytes.size()), "Copy demuxed video packet");
    const auto base = video_time_base();
    const CMSampleTimingInfo timing{media_time(packet->duration, base, "video duration"),
        media_time(packet->pts, base, "video PTS"), packet->dts == AV_NOPTS_VALUE ? kCMTimeInvalid : media_time(packet->dts, base, "video DTS")};
    const auto size = bytes.size();
    CMSampleBufferRef raw = nullptr;
    check_status(CMSampleBufferCreateReady(nullptr, block.get(), format, 1, 1, &timing, 1, &size, &raw), "Wrap avformat video packet");
    CFRef sample(raw);
    const auto array = CMSampleBufferGetSampleAttachmentsArray(raw, true);
    if (!array || !CFArrayGetCount(array)) throw std::runtime_error("Missing demuxed sample attachments");
    auto entry = static_cast<CFMutableDictionaryRef>(const_cast<void*>(CFArrayGetValueAtIndex(array, 0)));
    if (!(packet->flags & AV_PKT_FLAG_KEY)) CFDictionarySetValue(entry, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
    if (packet->flags & AV_PKT_FLAG_DISCARD) CFDictionarySetValue(entry, kCMSampleAttachmentKey_DoNotDisplay, kCFBooleanTrue);
    CFRetain(raw);
    return raw;
}
} // namespace vtenc
