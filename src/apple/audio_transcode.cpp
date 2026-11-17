#include "audio_transcode.hpp"
#include "aac_edit.hpp"
#include "ffmpeg_utils.hpp"
#include "encoder_session.hpp"
#include <AudioToolbox/AudioToolbox.h>
extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/intreadwrite.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <vector>

namespace vtenc {
namespace {
constexpr int max_pcm_samples = 262144;
constexpr OSStatus need_input = 1;
template<class T> T property(AudioConverterRef converter, AudioConverterPropertyID key, const char* context) {
    T value{};
    UInt32 size = sizeof(value);
    check_status(AudioConverterGetProperty(converter, key, &size, &value), context);
    if (size != sizeof(value)) throw std::runtime_error(std::string(context) + ": unexpected property size");
    return value;
}
void setting(AudioConverterRef converter, AudioConverterPropertyID key, UInt32 value, const char* context) {
    check_status(AudioConverterSetProperty(converter, key, sizeof(value), &value), context);
    if (property<UInt32>(converter, key, context) != value) throw std::runtime_error(std::string(context) + ": requested value was changed");
}
}
struct AudioTranscoder::Impl {
    ContainerInput& input;
    const volatile std::sig_atomic_t& cancelled;
    AVCodecContext* decoder = nullptr;
    AVCodecParameters* parameters = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    AudioConverterRef converter = nullptr;
    AVRational input_base{};
    int source_rate = 0, rate = 0, channels = 0, source_format = -1;
    AVChannelLayout layout{};
    bool decoder_flushed = false, pcm_eof = false, encoded_eof = false;
    bool have_origin = false, first_packet = true;
    CMTime source_origin = kCMTimeInvalid;
    std::int64_t origin = 0, source_samples = 0, output_samples = 0, packet_index = 0;
    std::optional<std::int64_t> source_limit;
    UInt32 priming = 0, packet_capacity = 0;
    std::vector<float> converted, callback_pcm;
    std::exception_ptr callback_error;
    PacketPtr pending;
    Impl(ContainerInput& i, const volatile std::sig_atomic_t& c) : input(i), cancelled(c) {}
    ~Impl() {
        if (converter) AudioConverterDispose(converter);
        if (fifo) av_audio_fifo_free(fifo);
        swr_free(&resampler); av_frame_free(&frame); avcodec_free_context(&decoder);
        avcodec_parameters_free(&parameters); av_channel_layout_uninit(&layout);
    }
    void check_cancel() const { if (cancelled) throw MediaCancelled{}; }
    bool decode() {
        while (true) {
            check_cancel();
            av_frame_unref(frame);
            const int status = avcodec_receive_frame(decoder, frame);
            if (status == AVERROR_EOF) return false;
            if (status != AVERROR(EAGAIN)) {
                av_check(status, "Decode selected audio track");
                if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags)
                    throw std::runtime_error("Audio decoder returned corrupt/concealed samples");
                return true;
            }
            if (decoder_flushed) throw std::runtime_error("Audio decoder requested input after drain");
            auto packet = input.read_audio();
            if (packet && (packet->flags & AV_PKT_FLAG_CORRUPT)) throw std::runtime_error("Corrupt input audio packet");
            av_check(avcodec_send_packet(decoder, packet.get()), "Submit selected audio packet / drain");
            if (!packet) decoder_flushed = true;
        }
    }
    void convert(const AVFrame* decoded) {
        check_cancel();
        int count = 0;
        if (decoded) {
            if (decoded->sample_rate != source_rate || decoded->format != source_format ||
                av_channel_layout_compare(&decoded->ch_layout, &layout) != 0)
                throw std::runtime_error("Audio format changed mid-stream");
            count = decoded->nb_samples;
            if (count <= 0 || count > max_pcm_samples) throw std::runtime_error("Invalid or excessive decoded audio frame size");
            const auto pts = media_time(decoded->pts, input_base, "decoded audio PTS");
            if (!have_origin) {
                have_origin = true; source_origin = pts;
                origin = av_rescale_q(decoded->pts, input_base, {1, rate});
                if (CMTimeCompare(pts, CMTimeMake(origin, rate)) != 0)
                    std::cerr << "vtenc: AAC start rounded to nearest output sample (at most half a sample)\n";
            }
            const auto expected = CMTimeAdd(source_origin, CMTimeMake(source_samples, source_rate));
            const auto difference = CMTimeAbsoluteValue(CMTimeSubtract(pts, expected));
            // Quantized container PTS may differ by half a tick; never accumulate this rounding.
            const auto tick = CMTimeMake(input_base.num, input_base.den);
            if (!CMTIME_IS_NUMERIC(expected) || CMTIME_HAS_BEEN_ROUNDED(expected) ||
                CMTimeCompare(CMTimeMultiply(difference, 2), tick) > 0)
                throw std::runtime_error("Discontinuous decoded audio PTS: AAC requires a continuous sample timeline; use --audio-copy or --no-audio");
            if (source_limit) {
                count = static_cast<int>(std::min<std::int64_t>(count, *source_limit - source_samples));
                if (!count) return; // Still drain decoder; these are explicitly declared trailing samples.
            }
            if (source_samples > INT64_MAX - count) throw std::runtime_error("Audio sample counter overflow");
            source_samples += count;
        }
        const auto capacity = swr_get_out_samples(resampler, count);
        av_check(capacity, "Size audio resampling buffer");
        if (capacity > max_pcm_samples) throw std::runtime_error("Audio resampler exceeds bounded PCM queue");
        converted.resize(static_cast<std::size_t>(std::max(capacity, 1)) * static_cast<std::size_t>(channels));
        std::uint8_t* buffer = reinterpret_cast<std::uint8_t*>(converted.data());
        const int produced = swr_convert(resampler, &buffer, capacity,
            decoded ? const_cast<const std::uint8_t**>(decoded->extended_data) : nullptr, count);
        av_check(produced, "Resample selected audio / drain");
        if (av_audio_fifo_size(fifo) > max_pcm_samples - produced) throw std::runtime_error("AAC PCM queue limit exceeded");
        void* data = converted.data();
        if (av_audio_fifo_write(fifo, &data, produced) != produced) throw std::runtime_error("Write AAC PCM FIFO failed");
        if (output_samples > INT64_MAX - produced) throw std::runtime_error("Resampled audio count overflow");
        output_samples += produced;
        if (!decoded && produced == 0) pcm_eof = true;
    }
    void fill_pcm() {
        if (decode()) convert(frame);
        else {
            if (source_limit && source_samples != *source_limit) throw std::runtime_error("Decoded AAC ended before its declared valid sample count");
            convert(nullptr);
        }
    }
    static OSStatus supply(AudioConverterRef, UInt32* count, AudioBufferList* data,
                           AudioStreamPacketDescription**, void* context) noexcept {
        auto& s = *static_cast<Impl*>(context);
        try {
            s.check_cancel();
            const int available = av_audio_fifo_size(s.fifo);
            const auto n = std::min(*count, static_cast<UInt32>(available));
            *count = n;
            data->mNumberBuffers = 1;
            data->mBuffers[0].mNumberChannels = static_cast<UInt32>(s.channels);
            data->mBuffers[0].mDataByteSize = n * static_cast<UInt32>(static_cast<unsigned>(s.channels) * sizeof(float));
            data->mBuffers[0].mData = nullptr;
            if (!n) return s.pcm_eof ? noErr : need_input;
            s.callback_pcm.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(s.channels));
            void* buffer = s.callback_pcm.data();
            if (av_audio_fifo_read(s.fifo, &buffer, static_cast<int>(n)) != static_cast<int>(n))
                throw std::runtime_error("Read AAC PCM FIFO failed");
            data->mBuffers[0].mData = buffer;
            return noErr;
        } catch (...) { *count = 0; s.callback_error = std::current_exception(); return kAudio_ParamError; }
    }
    PacketPtr encode() {
        while (!encoded_eof) {
            check_cancel();
            while (!pcm_eof && av_audio_fifo_size(fifo) < 4096) fill_pcm();
            auto packet = make_packet();
            av_check(av_new_packet(packet.get(), static_cast<int>(packet_capacity)), "Allocate AAC packet");
            AudioBufferList output{};
            output.mNumberBuffers = 1;
            output.mBuffers[0] = {static_cast<UInt32>(channels), packet_capacity, packet->data};
            AudioStreamPacketDescription description{};
            UInt32 packets = 1;
            callback_error = nullptr;
            const auto status = AudioConverterFillComplexBuffer(converter, supply, this, &packets, &output, &description);
            if (callback_error) std::rethrow_exception(callback_error);
            if (status != need_input) check_status(status, "AudioConverterFillComplexBuffer [AAC-LC]");
            if (packets) {
                if (packets != 1 || description.mStartOffset != 0 || !description.mDataByteSize ||
                    description.mDataByteSize > packet_capacity ||
                    (description.mVariableFramesInPacket && description.mVariableFramesInPacket != 1024))
                    throw std::runtime_error("Unexpected AudioToolbox AAC packet layout");
                av_shrink_packet(packet.get(), static_cast<int>(description.mDataByteSize));
                if (packet_index > (INT64_MAX - 1024) / 1024) throw std::runtime_error("AAC packet timestamp overflow");
                const auto relative = packet_index++ * 1024 - priming;
                packet->pts = exact_ticks(CMTimeAdd(CMTimeMake(origin, rate), CMTimeMake(relative, rate)), {1, rate});
                packet->dts = packet->pts; packet->duration = 1024; packet->flags |= AV_PKT_FLAG_KEY;
                return packet;
            }
            if (pcm_eof && status == noErr) { encoded_eof = true; return nullptr; }
            if (status != need_input) throw std::runtime_error("AudioToolbox made no AAC encoding progress");
        }
        return nullptr;
    }
};
AudioTranscoder::AudioTranscoder(ContainerInput& input, const EncodeConfig& config,
                               const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>(input, cancelled)) {
    auto& s = *impl_;
    const auto* p = input.info().audio_parameters;
    if (!p) throw std::runtime_error("--audio-codec aac requires an input audio track");
    if (p->codec_type != AVMEDIA_TYPE_AUDIO) throw std::runtime_error("Selected AAC source is not audio");
    const auto* decoder = avcodec_find_decoder(p->codec_id);
    if (!decoder || decoder->type != AVMEDIA_TYPE_AUDIO) throw std::runtime_error("No libavcodec decoder for selected audio track");
    s.decoder = avcodec_alloc_context3(decoder); s.frame = av_frame_alloc();
    s.parameters = avcodec_parameters_alloc();
    if (!s.decoder || !s.frame || !s.parameters) throw std::bad_alloc();
    av_check(avcodec_parameters_to_context(s.decoder, p), "Configure audio decoder");
    s.input_base = {input.info().audio_timebase_num, input.info().audio_timescale};
    s.decoder->pkt_timebase = s.input_base;
    s.decoder->err_recognition = AV_EF_EXPLODE;
    av_check(avcodec_open2(s.decoder, decoder, nullptr), "Open selected audio decoder");
    if (!s.decode()) throw std::runtime_error("Selected audio track contains no decoded samples");
    s.source_rate = s.frame->sample_rate; s.source_format = s.frame->format;
    s.rate = config.audio_sample_rate ? config.audio_sample_rate : s.source_rate;
    s.channels = s.frame->ch_layout.nb_channels;
    const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO, stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (av_channel_layout_compare(&s.frame->ch_layout, &mono) && av_channel_layout_compare(&s.frame->ch_layout, &stereo))
        throw std::runtime_error("AAC currently requires an explicit mono/stereo channel layout; no implicit downmix");
    if (s.rate != 44100 && s.rate != 48000)
        throw std::runtime_error("AAC currently supports 44100/48000 Hz; use --audio-samplerate explicitly");
    if (s.source_rate <= 0) throw std::runtime_error("Invalid decoded audio sample rate");
    av_check(av_channel_layout_copy(&s.layout, &s.frame->ch_layout), "Copy audio channel layout");
    av_check(swr_alloc_set_opts2(&s.resampler, &s.layout, AV_SAMPLE_FMT_FLT, s.rate,
        &s.layout, static_cast<AVSampleFormat>(s.source_format), s.source_rate, 0, nullptr), "Create audio resampler");
    av_check(swr_init(s.resampler), "Initialize audio resampler");
    s.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, s.channels, 4096);
    if (!s.fifo) throw std::bad_alloc();
    AudioStreamBasicDescription pcm{}, aac{};
    pcm.mSampleRate = s.rate; pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame = static_cast<UInt32>(static_cast<unsigned>(s.channels) * sizeof(float));
    pcm.mFramesPerPacket = 1; pcm.mChannelsPerFrame = static_cast<UInt32>(s.channels); pcm.mBitsPerChannel = 32;
    aac.mSampleRate = s.rate; aac.mFormatID = kAudioFormatMPEG4AAC;
    aac.mFormatFlags = kMPEG4Object_AAC_LC; aac.mChannelsPerFrame = static_cast<UInt32>(s.channels);
    check_status(AudioConverterNew(&pcm, &aac, &s.converter), "AudioConverterNew [AAC-LC]");
    if (!s.converter) throw std::runtime_error("AudioToolbox returned no AAC converter");
    // The compressed AAC converter does not expose the PCM resampler's writable
    // PrimeMethod property. Query its actual codec delay below instead.
    setting(s.converter, kAudioCodecPropertyBitRateControlMode, kAudioCodecBitRateControlMode_LongTermAverage, "Set AAC ABR mode");
    setting(s.converter, kAudioConverterCodecQuality, kAudioConverterQuality_High, "Set AAC codec quality");
    setting(s.converter, kAudioConverterEncodeBitRate, static_cast<UInt32>(config.audio_bitrate), "Set AAC bitrate");
    const auto actual = property<AudioStreamBasicDescription>(s.converter, kAudioConverterCurrentOutputStreamDescription, "Read AAC output format");
    if (actual.mSampleRate != s.rate || actual.mChannelsPerFrame != static_cast<UInt32>(s.channels) ||
        actual.mFormatID != kAudioFormatMPEG4AAC || actual.mFramesPerPacket != 1024)
        throw std::runtime_error("AudioToolbox changed requested AAC output format");
    s.priming = property<AudioConverterPrimeInfo>(s.converter, kAudioConverterPrimeInfo, "Read AAC priming").leadingFrames;
    s.packet_capacity = property<UInt32>(s.converter, kAudioConverterPropertyMaximumOutputPacketSize, "Read AAC maximum packet size");
    if (s.priming > 65536 || !s.packet_capacity || s.packet_capacity > 1024 * 1024) throw std::runtime_error("Unexpected AAC priming/packet bound");
    auto& out = *s.parameters;
    out.codec_type = AVMEDIA_TYPE_AUDIO; out.codec_id = AV_CODEC_ID_AAC; out.profile = AV_PROFILE_AAC_LOW;
    out.sample_rate = s.rate; out.frame_size = 1024; out.initial_padding = static_cast<int>(s.priming);
    out.bit_rate = config.audio_bitrate;
    av_check(av_channel_layout_copy(&out.ch_layout, &s.layout), "Set AAC output channel layout");
    // MPEG-4 AudioSpecificConfig for verified AAC-LC / 44.1 or 48 kHz / mono or stereo.
    const unsigned asc = (2U << 11) | ((s.rate == 44100 ? 4U : 3U) << 7) | (static_cast<unsigned>(s.channels) << 3);
    out.extradata = static_cast<std::uint8_t*>(av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!out.extradata) throw std::bad_alloc();
    out.extradata_size = 2; AV_WB16(out.extradata, asc);
    if (p->codec_id == AV_CODEC_ID_AAC && input.info().audio_edit_list)
        s.source_limit = movie_aac_valid_samples(config.input, s.source_rate, s.channels);
    s.convert(s.frame);
    std::cerr << "vtenc: audio backend=AudioToolbox AAC-LC, ABR=" << config.audio_bitrate << " bit/s, rate=" << s.rate
              << ", channels=" << s.channels << ", priming=" << s.priming << " samples\n";
}
AudioTranscoder::~AudioTranscoder() = default;
const AVCodecParameters* AudioTranscoder::parameters() const { return impl_->parameters; }
PacketPtr AudioTranscoder::next() {
    auto& s = *impl_;
    s.check_cancel();
    if (!s.pending) s.pending = s.encode();
    if (!s.pending) return nullptr;
    auto next = s.encode(); // One-packet lookahead identifies the final padded packet.
    auto result = std::move(s.pending); s.pending = std::move(next);
    std::uint32_t skip = s.first_packet ? s.priming : 0, discard = 0;
    s.first_packet = false;
    if (!s.pending) {
        const auto valid_end = exact_ticks(CMTimeAdd(CMTimeMake(s.origin, s.rate), CMTimeMake(s.output_samples, s.rate)), {1, s.rate});
        const auto valid = valid_end - result->pts;
        if (valid <= 0 || valid > 1024) throw std::runtime_error("AudioToolbox drain did not match valid AAC sample count");
        discard = static_cast<std::uint32_t>(1024 - valid);
        result->duration = valid;
        s.parameters->trailing_padding = static_cast<int>(discard);
        std::cerr << "vtenc: AAC drained input=" << s.source_samples << " samples, resampled=" << s.output_samples
                  << ", trailing_padding=" << discard << " samples\n";
    }
    if (skip || discard) {
        auto* side = av_packet_new_side_data(result.get(), AV_PKT_DATA_SKIP_SAMPLES, 10);
        if (!side) throw std::bad_alloc();
        AV_WL32(side, skip); AV_WL32(side + 4, discard); side[8] = side[9] = 0;
    }
    return result;
}
} // namespace vtenc
