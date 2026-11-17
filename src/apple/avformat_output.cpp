#include "ffmpeg_utils.hpp"
#include "cf_ref.hpp"
#include "encoder_session.hpp"
#include "bob.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace vtenc {
namespace {
void copy_extradata(AVCodecParameters* parameters, CMFormatDescriptionRef format, CFStringRef key) {
    const auto atoms = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (!atoms || CFGetTypeID(atoms) != CFDictionaryGetTypeID()) throw std::runtime_error("Missing encoder configuration atoms");
    const auto value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(atoms), key);
    if (!value || CFGetTypeID(value) != CFDataGetTypeID()) throw std::runtime_error("Missing encoded codec configuration");
    const auto data = static_cast<CFDataRef>(value);
    const auto size = CFDataGetLength(data);
    if (size <= 0 || size > std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE) throw std::runtime_error("Invalid encoded codec configuration size");
    parameters->extradata = static_cast<std::uint8_t*>(av_mallocz(static_cast<std::size_t>(size) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!parameters->extradata) throw std::bad_alloc();
    parameters->extradata_size = static_cast<int>(size);
    std::memcpy(parameters->extradata, CFDataGetBytePtr(data), static_cast<std::size_t>(size));
}
bool keyframe(CMSampleBufferRef sample) {
    const auto array = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!array || !CFArrayGetCount(array)) return true;
    const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(array, 0));
    const auto not_sync = CFDictionaryGetValue(entry, kCMSampleAttachmentKey_NotSync);
    return !not_sync || !CFEqual(not_sync, kCFBooleanTrue);
}
int integer(CFDictionaryRef dictionary, CFStringRef key) {
    const auto value = CFDictionaryGetValue(dictionary, key);
    int result = 0;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &result)) {
        throw std::runtime_error("Invalid pixel aspect ratio metadata");
    }
    return result;
}
}
struct ContainerOutput::Impl {
    AVFormatContext* mux = nullptr;
    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    AVRational source_audio_base{};
    std::string directory, temporary, destination;
    bool finished = false;
    bool reported_audio_rescale = false;
    ~Impl() {
        if (mux) {
            if (mux->pb) avio_closep(&mux->pb);
            avformat_free_context(mux);
        }
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }
    void check() const {
        if (finished) throw std::runtime_error("Write after container finish");
        if (mux && mux->pb) av_check(mux->pb->error, "avformat output I/O");
    }
};
ContainerOutput::ContainerOutput(const EncodeConfig& config, const MediaInfo& info, CMFormatDescriptionRef format)
    : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    s.destination = config.output;
    struct stat existing{};
    if (lstat(config.output.c_str(), &existing) == 0) throw std::runtime_error("Output already exists: " + config.output);
    if (errno != ENOENT) throw std::runtime_error("Inspect output: " + std::string(std::strerror(errno)));
    auto pattern = config.output + ".part.XXXXXX";
    if (!mkdtemp(pattern.data())) throw std::runtime_error("Create output directory: " + std::string(std::strerror(errno)));
    s.directory = std::move(pattern); s.temporary = s.directory + "/media";
    // The output contract remains MP4/MOV; only input format selection is automatic.
    auto extension = std::filesystem::path(config.output).extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    av_check(avformat_alloc_output_context2(&s.mux, nullptr, extension == ".mov" ? "mov" : "mp4", s.temporary.c_str()), "avformat_alloc_output_context2");
    if (!s.mux) throw std::runtime_error("No output mux context");
    s.mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    s.video = avformat_new_stream(s.mux, nullptr);
    if (!s.video) throw std::bad_alloc();
    const int scale = config.filter.bob ? bob_timescale(info.video_timescale) : info.video_timescale;
    s.video->time_base = {1, scale};
    auto* p = s.video->codecpar;
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(format);
    p->codec_type = AVMEDIA_TYPE_VIDEO;
    p->codec_id = config.video.codec == Codec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    p->codec_tag = config.video.codec == Codec::h264 ? MKTAG('a','v','c','1') : MKTAG('h','v','c','1');
    p->width = dimensions.width; p->height = dimensions.height;
    p->format = config.video.bit_depth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    p->field_order = AV_FIELD_PROGRESSIVE;
    p->chroma_location = AVCHROMA_LOC_LEFT;
    copy_extradata(p, format, config.video.codec == Codec::h264 ? CFSTR("avcC") : CFSTR("hvcC"));
    const auto get = [&](CFStringRef key) { return CMFormatDescriptionGetExtension(format, key); };
    if (const auto value = get(kCVImageBufferColorPrimariesKey)) p->color_primaries = static_cast<AVColorPrimaries>(CVColorPrimariesGetIntegerCodePointForString(static_cast<CFStringRef>(value)));
    if (const auto value = get(kCVImageBufferTransferFunctionKey)) p->color_trc = static_cast<AVColorTransferCharacteristic>(CVTransferFunctionGetIntegerCodePointForString(static_cast<CFStringRef>(value)));
    if (const auto value = get(kCVImageBufferYCbCrMatrixKey)) p->color_space = static_cast<AVColorSpace>(CVYCbCrMatrixGetIntegerCodePointForString(static_cast<CFStringRef>(value)));
    const auto full = get(kCMFormatDescriptionExtension_FullRangeVideo);
    p->color_range = full && CFEqual(full, kCFBooleanTrue) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (const auto ratio = get(kCVImageBufferPixelAspectRatioKey)) {
        if (CFGetTypeID(ratio) != CFDictionaryGetTypeID()) throw std::runtime_error("Invalid pixel aspect ratio dictionary");
        p->sample_aspect_ratio = {integer(static_cast<CFDictionaryRef>(ratio), kCVImageBufferPixelAspectRatioHorizontalSpacingKey),
                                  integer(static_cast<CFDictionaryRef>(ratio), kCVImageBufferPixelAspectRatioVerticalSpacingKey)};
        s.video->sample_aspect_ratio = p->sample_aspect_ratio;
    }
    if (info.transform != std::array<double, 6>{1, 0, 0, 1, 0, 0}) {
        auto t = info.transform;
        const auto source_size = CMVideoFormatDescriptionGetDimensions(info.video_format);
        const auto min_x = [&](CMVideoDimensions size) { return std::min(0.0, t[0] * size.width) + std::min(0.0, t[2] * size.height); };
        const auto min_y = [&](CMVideoDimensions size) { return std::min(0.0, t[1] * size.width) + std::min(0.0, t[3] * size.height); };
        t[4] += min_x(source_size) - min_x(dimensions);
        t[5] += min_y(source_size) - min_y(dimensions);
        std::array<std::int32_t, 9> matrix{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const auto value = t[i] * 65536.0;
            if (!std::isfinite(value) || value != std::trunc(value) || value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
                throw std::runtime_error("Display transform cannot be represented exactly");
            }
            constexpr std::array<std::size_t, 6> indices{0, 1, 3, 4, 6, 7};
            matrix[indices[i]] = static_cast<std::int32_t>(value);
        }
        matrix[8] = 1 << 30;
        auto* side = av_packet_side_data_new(&p->coded_side_data, &p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX, sizeof(matrix), 0);
        if (!side) throw std::bad_alloc();
        std::memcpy(side->data, matrix.data(), sizeof(matrix));
    }
    std::int64_t movie_scale = scale;
    if (info.audio_parameters) {
        s.audio = avformat_new_stream(s.mux, nullptr);
        if (!s.audio) throw std::bad_alloc();
        av_check(avcodec_parameters_copy(s.audio->codecpar, info.audio_parameters), "Copy compressed audio parameters");
        s.audio->codecpar->codec_tag = 0;
        s.source_audio_base = {info.audio_timebase_num, info.audio_timescale};
        s.audio->time_base = s.source_audio_base;
        movie_scale = std::lcm(movie_scale, static_cast<std::int64_t>(info.audio_timescale));
        // The MOV muxer uses the audio sampling clock. Include it so priming,
        // track offsets and edit durations need not be rounded to milliseconds.
        if (info.audio_parameters->sample_rate > 0) {
            const auto rate = info.audio_parameters->sample_rate;
            const auto factor = rate / std::gcd(movie_scale, static_cast<std::int64_t>(rate));
            if (movie_scale > std::numeric_limits<std::int32_t>::max() / factor) throw std::runtime_error("Movie timescale exceeds 32-bit range");
            movie_scale *= factor;
        }
    }
    if (movie_scale > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("Movie timescale exceeds 32-bit range");
    av_check(avio_open(&s.mux->pb, s.temporary.c_str(), AVIO_FLAG_WRITE), "avio_open output");
    AVDictionary* options = nullptr;
    av_dict_set_int(&options, "video_track_timescale", scale, 0);
    av_dict_set_int(&options, "movie_timescale", movie_scale, 0);
    const int status = avformat_write_header(s.mux, &options);
    av_dict_free(&options);
    av_check(status, "avformat_write_header [audio must be copy-compatible; use --no-audio]");
}
ContainerOutput::~ContainerOutput() = default;
void ContainerOutput::append_video(CMSampleBufferRef sample) {
    auto& s = *impl_; s.check();
    const auto size = CMSampleBufferGetTotalSampleSize(sample);
    if (CMSampleBufferGetNumSamples(sample) != 1 || !size || size > std::numeric_limits<int>::max()) throw std::runtime_error("Invalid encoded video sample");
    auto packet = make_packet();
    av_check(av_new_packet(packet.get(), static_cast<int>(size)), "Allocate muxed video packet");
    check_status(CMBlockBufferCopyDataBytes(CMSampleBufferGetDataBuffer(sample), 0, size, packet->data), "Copy hardware encoded video packet");
    packet->pts = exact_ticks(CMSampleBufferGetPresentationTimeStamp(sample), s.video->time_base);
    const auto dts = CMSampleBufferGetDecodeTimeStamp(sample);
    packet->dts = CMTIME_IS_VALID(dts) ? exact_ticks(dts, s.video->time_base) : packet->pts;
    packet->duration = exact_ticks(CMSampleBufferGetDuration(sample), s.video->time_base);
    packet->stream_index = s.video->index;
    if (keyframe(sample)) packet->flags |= AV_PKT_FLAG_KEY;
    av_check(av_interleaved_write_frame(s.mux, packet.get()), "av_interleaved_write_frame [hardware video]");
    s.check();
}
void ContainerOutput::append_audio(const AVPacket* source) {
    auto& s = *impl_; s.check();
    if (!s.audio) throw std::runtime_error("No audio stream in muxer");
    auto packet = make_packet();
    av_check(av_packet_ref(packet.get(), source), "Reference copied audio packet");
    const auto audio_ticks = [&](std::int64_t value, const char* context) {
        const auto time = media_time(value, s.source_audio_base, context);
        const auto ticks = av_rescale_q(value, s.source_audio_base, s.audio->time_base);
        if (!s.reported_audio_rescale && CMTimeCompare(media_time(ticks, s.audio->time_base, context), time) != 0) {
            std::cerr << "vtenc: audio timestamps rescaled from " << s.source_audio_base.num << '/' << s.source_audio_base.den
                      << " to " << s.audio->time_base.num << '/' << s.audio->time_base.den
                      << " (nearest output audio tick; MP4/MOV sampling clock)\n";
            s.reported_audio_rescale = true;
        }
        return ticks;
    };
    packet->pts = audio_ticks(source->pts, "audio PTS");
    packet->dts = source->dts == AV_NOPTS_VALUE ? packet->pts : audio_ticks(source->dts, "audio DTS");
    packet->duration = audio_ticks(source->duration, "audio duration");
    packet->pos = -1; packet->stream_index = s.audio->index;
    av_check(av_interleaved_write_frame(s.mux, packet.get()), "av_interleaved_write_frame [audio copy; use --no-audio if unsupported]");
    s.check();
}
std::uint64_t ContainerOutput::finish(const volatile std::sig_atomic_t& cancelled) {
    auto& s = *impl_; s.check();
    if (cancelled) return 0;
    av_check(av_write_trailer(s.mux), "av_write_trailer");
    avio_flush(s.mux->pb);
    s.check();
    av_check(avio_closep(&s.mux->pb), "avio_close output");
    s.finished = true;
    if (cancelled) return 0;
    const auto bytes = std::filesystem::file_size(s.temporary);
    if (!bytes) throw std::runtime_error("Container output is empty");
    if (link(s.temporary.c_str(), s.destination.c_str()) != 0) throw std::runtime_error("Publish container [" + s.destination + "]: " + std::strerror(errno));
    return bytes;
}
} // namespace vtenc
