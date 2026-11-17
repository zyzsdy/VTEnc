#include "decoded_frames.hpp"
#include "encoder_session.hpp"
#include "gpu_filter.hpp"
#include "ffmpeg_utils.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>
#include <limits>
#include <optional>

#if LIBAVCODEC_VERSION_MAJOR < 61 || LIBAVFORMAT_VERSION_MAJOR < 61
#error VTEnc requires FFmpeg 7.0 or newer
#endif

namespace vtenc {
namespace {
AVPixelFormat software_format(AVCodecContext*, const AVPixelFormat* formats) noexcept {
    for (auto p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const auto* description = av_pix_fmt_desc_get(*p);
        if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}
bool software_codec(const AVCodec* codec) {
    return codec && av_codec_is_decoder(codec) &&
        !(codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID));
}
const AVCodec* find_software_decoder(AVCodecID id) {
    const auto* codec = avcodec_find_decoder(id);
    if (software_codec(codec)) return codec;
    void* iterator = nullptr;
    while ((codec = av_codec_iterate(&iterator))) {
        if (codec->id == id && software_codec(codec)) return codec;
    }
    throw std::runtime_error("No FFmpeg software decoder for codec " + std::string(avcodec_get_name(id)));
}
class PixelLock {
public:
    explicit PixelLock(CVPixelBufferRef pixel) : pixel_(pixel) {
        check_status(CVPixelBufferLockBaseAddress(pixel_, 0), "Lock software decoded pixel buffer");
    }
    ~PixelLock() { if (locked_) CVPixelBufferUnlockBaseAddress(pixel_, 0); }
    void finish() {
        const auto status = CVPixelBufferUnlockBaseAddress(pixel_, 0);
        locked_ = false;
        check_status(status, "Unlock software decoded pixel buffer");
    }
private:
    CVPixelBufferRef pixel_;
    bool locked_ = true;
};
}

struct SoftwareFrames::Impl {
    ContainerInput& input;
    AVCodecContext* decoder = nullptr;
    AVFrame* frame = nullptr;
    bool draining = false;
    bool ended = false;
    bool deinterlace;
    bool ivtc;
    bool packet_cadence = false;
    bool colorspace;
    FieldOrder field_order;
    const MediaInfo& info;
    const volatile std::sig_atomic_t& cancelled;
    CMTime last_pts = kCMTimeInvalid;
    std::optional<CFRef<CVPixelBufferPoolRef>> pool;
    std::optional<CFRef<CMVideoFormatDescriptionRef>> frame_format;
    AVPixelFormat layout = AV_PIX_FMT_NONE;
    OSType pixel_type = 0;

    Impl(ContainerInput& source, const FilterConfig& filter, const volatile std::sig_atomic_t& stop)
        : input(source), deinterlace(filter.bob || filter.ivtc.enabled), ivtc(filter.ivtc.enabled), colorspace(filter.colorspace.enabled), field_order(filter.field_order), info(source.info()), cancelled(stop) {}
    ~Impl() {
        av_frame_free(&frame);
        avcodec_free_context(&decoder);
    }
    void check(int code, const char* context) const {
        if (cancelled) throw MediaCancelled{};
        av_check(code, context);
    }
    void attach_metadata(CVPixelBufferRef pixel) const {
        const auto attach = [&](CFStringRef key, CFTypeRef value) {
            if (value) CVBufferSetAttachment(pixel, key, value, kCVAttachmentMode_ShouldPropagate);
        };
        // Retain track-level metadata when the bitstream omits it. Never carry
        // compressed codec atoms, stale decoder attachments or display transforms.
        for (const auto key : {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey,
                              kCVImageBufferYCbCrMatrixKey, kCVImageBufferChromaLocationTopFieldKey,
                              kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferPixelAspectRatioKey,
                              kCVImageBufferCleanApertureKey}) {
            attach(key, CMFormatDescriptionGetExtension(info.video_format, key));
        }
        if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED) {
            const auto value = CVColorPrimariesGetStringForIntegerCodePoint(frame->color_primaries);
            if (!value) throw std::runtime_error("Unsupported FFmpeg color primaries");
            attach(kCVImageBufferColorPrimariesKey, value);
        }
        if (frame->color_trc != AVCOL_TRC_UNSPECIFIED) {
            const auto value = CVTransferFunctionGetStringForIntegerCodePoint(frame->color_trc);
            if (!value) throw std::runtime_error("Unsupported FFmpeg transfer function");
            attach(kCVImageBufferTransferFunctionKey, value);
        }
        if (frame->colorspace != AVCOL_SPC_UNSPECIFIED) {
            const auto value = CVYCbCrMatrixGetStringForIntegerCodePoint(frame->colorspace);
            if (!value) throw std::runtime_error("Unsupported FFmpeg YCbCr matrix");
            attach(kCVImageBufferYCbCrMatrixKey, value);
        }
        if (frame->chroma_location != AVCHROMA_LOC_UNSPECIFIED) {
            CFStringRef chroma = nullptr;
            switch (frame->chroma_location) {
            case AVCHROMA_LOC_LEFT: chroma = kCVImageBufferChromaLocation_Left; break;
            case AVCHROMA_LOC_CENTER: chroma = kCVImageBufferChromaLocation_Center; break;
            case AVCHROMA_LOC_TOPLEFT: chroma = kCVImageBufferChromaLocation_TopLeft; break;
            case AVCHROMA_LOC_TOP: chroma = kCVImageBufferChromaLocation_Top; break;
            case AVCHROMA_LOC_BOTTOMLEFT: chroma = kCVImageBufferChromaLocation_BottomLeft; break;
            case AVCHROMA_LOC_BOTTOM: chroma = kCVImageBufferChromaLocation_Bottom; break;
            default: throw std::runtime_error("Unsupported FFmpeg chroma location");
            }
            attach(kCVImageBufferChromaLocationTopFieldKey, chroma);
            attach(kCVImageBufferChromaLocationBottomFieldKey, chroma);
        }
        // Bob and IVTC interpret every raster as a field pair. Hard telecine can mix
        // progressive-coded and interlaced-coded frames in that same track.
        const int fields = deinterlace || (frame->flags & AV_FRAME_FLAG_INTERLACED) ? 2 : 1;
        CFRef count(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fields));
        attach(kCVImageBufferFieldCountKey, count.get());
        if (deinterlace && field_order != FieldOrder::automatic) {
            // The explicit source interpretation also applies to progressive
            // pictures and mixed field-order flags inside hard telecine streams.
            attach(kCVImageBufferFieldDetailKey, field_order == FieldOrder::tff
                ? kCVImageBufferFieldDetailTemporalTopFirst : kCVImageBufferFieldDetailTemporalBottomFirst);
        } else if (frame->flags & AV_FRAME_FLAG_INTERLACED) {
            attach(kCVImageBufferFieldDetailKey, (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)
                ? kCVImageBufferFieldDetailTemporalTopFirst : kCVImageBufferFieldDetailTemporalBottomFirst);
        } else if (deinterlace) {
            auto detail = CMFormatDescriptionGetExtension(info.video_format, kCMFormatDescriptionExtension_FieldDetail);
            if (detail && CFEqual(detail, kCVImageBufferFieldDetailSpatialFirstLineEarly)) detail = kCVImageBufferFieldDetailTemporalTopFirst;
            if (detail && CFEqual(detail, kCVImageBufferFieldDetailSpatialFirstLineLate)) detail = kCVImageBufferFieldDetailTemporalBottomFirst;
            attach(kCVImageBufferFieldDetailKey, detail);
        }
        const auto sar = frame->sample_aspect_ratio;
        if (sar.num > 0 && sar.den > 0) {
            CFRef horizontal(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &sar.num));
            CFRef vertical(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &sar.den));
            const void* keys[] = {kCVImageBufferPixelAspectRatioHorizontalSpacingKey, kCVImageBufferPixelAspectRatioVerticalSpacingKey};
            const void* values[] = {horizontal.get(), vertical.get()};
            CFRef ratio(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            attach(kCVImageBufferPixelAspectRatioKey, ratio.get());
        }
    }
    CFRef<CVPixelBufferRef> pixels() {
        if (frame->flags & AV_FRAME_FLAG_CORRUPT || frame->decode_error_flags) {
            throw std::runtime_error("FFmpeg returned a corrupt/concealed decoded frame");
        }
        if ((!colorspace && (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67)) ||
            av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA) || av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER)) {
            throw std::runtime_error("HDR requires --vpp-colorspace; Dolby Vision input is not supported");
        }
        if ((frame->flags & AV_FRAME_FLAG_INTERLACED) && !deinterlace) {
            throw std::runtime_error("Interlaced video requires --deinterlace bob or --vpp-ivtc");
        }
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(info.video_format);
        if (frame->width <= 0 || frame->height <= 0 || frame->width % 2 || frame->height % 2 ||
            frame->width != dimensions.width || frame->height != dimensions.height) {
            throw std::runtime_error("Software decoded dimensions differ from the track or are not even 4:2:0 dimensions");
        }
        const auto format = static_cast<AVPixelFormat>(frame->format);
        const bool ten = format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_P010LE;
        const bool planar = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUV420P10LE;
        if (!planar && format != AV_PIX_FMT_NV12 && format != AV_PIX_FMT_P010LE) {
            const auto name = av_get_pix_fmt_name(format);
            throw std::runtime_error("Software decode requires 8/10-bit 4:2:0; got " + std::string(name ? name : "unknown"));
        }
        const bool full = frame->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P;
        const OSType type = ten
            ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
            : (full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        if (!pool) {
            pool.emplace(make_metal_pixel_pool(frame->width, frame->height, type));
            layout = format;
            pixel_type = type;
        } else if (layout != format || pixel_type != type) {
            throw std::runtime_error("Software decoded pixel format/range changed mid-stream");
        }
        // RFF lookahead and a pending repeated field may retain extra sources.
        const int limit = ivtc ? 8 : 3;
        CFRef number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &limit));
        const void* keys[] = {kCVPixelBufferPoolAllocationThresholdKey};
        const void* values[] = {number.get()};
        CFRef options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CVPixelBufferRef raw = nullptr;
        check_status(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool->get(), options.get(), &raw),
            "Allocate software decoded pixel buffer");
        CFRef pixel(raw);
        CVBufferRemoveAllAttachments(raw);
        PixelLock lock(raw);
        const int bytes = ten ? 2 : 1;
        for (int plane = 0; plane < (planar ? 3 : 2); ++plane) {
            const int row_bytes = (plane && planar ? frame->width / 2 : frame->width) * bytes;
            const auto stride = static_cast<std::int64_t>(frame->linesize[plane]);
            if (!frame->data[plane] || (stride < 0 ? -stride : stride) < row_bytes) {
                throw std::runtime_error("Invalid FFmpeg decoded plane stride");
            }
        }
        for (int plane = 0; plane < 2; ++plane) {
            auto* destination = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(raw, static_cast<std::size_t>(plane)));
            const auto stride = CVPixelBufferGetBytesPerRowOfPlane(raw, static_cast<std::size_t>(plane));
            const int height = plane ? frame->height / 2 : frame->height;
            for (int y = 0; y < height; ++y) {
                auto* row = destination + static_cast<std::size_t>(y) * stride;
                const auto* source = frame->data[plane] + static_cast<std::ptrdiff_t>(y) * frame->linesize[plane];
                if (!planar || (plane == 0 && !ten)) {
                    std::memcpy(row, source, static_cast<std::size_t>(frame->width) * static_cast<std::size_t>(bytes));
                } else if (plane == 0) {
                    for (int x = 0; x < frame->width; ++x) {
                        std::uint16_t value;
                        std::memcpy(&value, source + x * 2, 2);
                        value = static_cast<std::uint16_t>(value << 6);
                        std::memcpy(row + x * 2, &value, 2);
                    }
                } else {
                    const auto* v = frame->data[2] + static_cast<std::ptrdiff_t>(y) * frame->linesize[2];
                    for (int x = 0; x < frame->width / 2; ++x) {
                        if (ten) {
                            std::uint16_t u_value, v_value;
                            std::memcpy(&u_value, source + x * 2, 2);
                            std::memcpy(&v_value, v + x * 2, 2);
                            u_value = static_cast<std::uint16_t>(u_value << 6);
                            v_value = static_cast<std::uint16_t>(v_value << 6);
                            std::memcpy(row + x * 4, &u_value, 2);
                            std::memcpy(row + x * 4 + 2, &v_value, 2);
                        } else { row[x * 2] = source[x]; row[x * 2 + 1] = v[x]; }
                    }
                }
            }
        }
        lock.finish();
        attach_metadata(raw);
        CMVideoFormatDescriptionRef description = nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, raw, &description), "Describe software decoded frame");
        CFRef current(description);
        if (!frame_format) frame_format.emplace(std::move(current));
        else if (!ivtc && !CMFormatDescriptionEqual(current.get(), frame_format->get())) {
            throw std::runtime_error("Software decoded video metadata changed mid-stream at PTS=" + std::to_string(frame->pts));
        }
        return pixel;
    }
};

SoftwareFrames::SoftwareFrames(const EncodeConfig& config, ContainerInput& input,
                               const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(input, config.filter, cancelled)) {
    auto& s = *impl_;
    const auto* parameters = input.video_parameters();
    const auto* codec = find_software_decoder(parameters->codec_id);
    s.decoder = avcodec_alloc_context3(codec);
    if (!s.decoder) throw std::bad_alloc();
    s.check(avcodec_parameters_to_context(s.decoder, parameters), "avcodec_parameters_to_context");
    s.decoder->pkt_timebase = input.video_time_base();
    s.packet_cadence = parameters->codec_id == AV_CODEC_ID_HEVC && config.filter.ivtc.enabled &&
        (config.filter.ivtc.expand != RffExpansion::off || config.filter.ivtc.mixed);
    // Carry metadata with its picture through frame threading and B-frame
    // reordering; packet order and a PTS lookup are not frame ownership.
    if (s.packet_cadence) s.decoder->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    s.decoder->get_format = &software_format;
    s.decoder->thread_count = 2; // Bounded frame threading, independent of host core count.
    s.decoder->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    s.check(avcodec_open2(s.decoder, codec, nullptr), "avcodec_open2 [software only]");
    s.frame = av_frame_alloc();
    if (!s.frame) throw std::bad_alloc();
}
SoftwareFrames::~SoftwareFrames() = default;

std::unique_ptr<DecodedFrame> SoftwareFrames::next() {
    auto& s = *impl_;
    while (!s.ended) {
        if (s.cancelled) throw MediaCancelled{};
        av_frame_unref(s.frame);
        const int result = avcodec_receive_frame(s.decoder, s.frame);
        if (result == AVERROR_EOF) { s.ended = true; return nullptr; }
        if (result == 0) {
            if (s.frame->flags & AV_FRAME_FLAG_DISCARD) continue;
            const auto base = s.input.video_time_base();
            const auto pts = media_time(s.frame->pts, base, "video PTS");
            if (CMTIME_IS_NUMERIC(s.info.video_range.start) && CMTimeCompare(pts, s.info.video_range.start) < 0) continue;
            if (CMTIME_IS_NUMERIC(s.last_pts) && CMTimeCompare(pts, s.last_pts) <= 0) {
                throw std::runtime_error("Software decoded video PTS must strictly increase");
            }
            s.last_pts = pts;
            auto duration = media_time(s.frame->duration, base, "frame duration");
            // The scheduler derives all but the final duration from adjacent PTS.
            // A missing final duration is rejected there, never guessed from FPS.
            auto pixel = s.pixels();
            FrameCadence cadence{true, (s.frame->flags & AV_FRAME_FLAG_INTERLACED) != 0,
                s.frame->repeat_pict, (s.frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? FieldOrder::tff : FieldOrder::bff};
            if (s.packet_cadence) {
                if (!s.frame->opaque_ref || s.frame->opaque_ref->size != sizeof(FrameCadence))
                    throw std::runtime_error("HEVC decoder did not preserve per-picture cadence metadata");
                std::memcpy(&cadence, s.frame->opaque_ref->data, sizeof(cadence));
                if (!cadence.known) throw std::runtime_error("Missing parsed HEVC frame cadence");
            }
            return std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel), pts, duration, cadence});
        }
        s.check(result == AVERROR(EAGAIN) ? 0 : result, "avcodec_receive_frame [software]");
        if (s.draining) throw std::runtime_error("FFmpeg requested more input after decoder drain");
        FrameCadence cadence;
        auto packet = s.input.read_video_packet(s.packet_cadence ? &cadence : nullptr);
        if (!packet) {
            s.check(avcodec_send_packet(s.decoder, nullptr), "avcodec_send_packet [drain]");
            s.draining = true;
        } else {
            if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("FFmpeg demux returned a corrupt video packet");
            if (s.packet_cadence) {
                if (!cadence.known) throw std::runtime_error("Missing parsed HEVC packet cadence");
                av_buffer_unref(&packet->opaque_ref);
                packet->opaque_ref = av_buffer_alloc(sizeof(cadence));
                if (!packet->opaque_ref) throw std::bad_alloc();
                std::memcpy(packet->opaque_ref->data, &cadence, sizeof(cadence));
            }
            s.check(avcodec_send_packet(s.decoder, packet.get()), "avcodec_send_packet [software]");
        }
    }
    return nullptr;
}

} // namespace vtenc
