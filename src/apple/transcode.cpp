#include "vtenc/encoder.hpp"
#include "encoder_session.hpp"
#include "media_io.hpp"
#include "source_format.hpp"
#include "gpu_filter.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace vtenc {
namespace {
void require_time(CMTime time, const char* name) {
    if (!CMTIME_IS_NUMERIC(time) || time.epoch != 0 || CMTIME_HAS_BEEN_ROUNDED(time)) {
        throw std::runtime_error(std::string("Invalid or inexact source ") + name);
    }
}

bool sample_flag(CMSampleBufferRef sample, CFStringRef key) {
    const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return false;
    const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    const auto value = CFDictionaryGetValue(dictionary, key);
    return value && CFEqual(value, kCFBooleanTrue);
}

void validate_source(CMFormatDescriptionRef format) {
    const auto codec = CMFormatDescriptionGetMediaSubType(format);
    if (codec == 0x64766831U || codec == 0x64766865U) throw std::runtime_error("Dolby Vision input is not supported by the P2 SDR pipeline");
    if (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC && codec != 0x68657631U && codec != kCMVideoCodecType_AV1) {
        throw std::runtime_error("P2 accepts H.264, HEVC or AV1 video when a hardware decoder is available");
    }
    const auto fields = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FieldCount);
    if (fields) {
        int count = 0;
        if (CFGetTypeID(fields) != CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(fields), kCFNumberIntType, &count)) {
            throw std::runtime_error("Invalid video field count");
        }
        if (count != 1) throw std::runtime_error("Interlaced video requires P3 deinterlacing; P2 accepts progressive input only");
    }
    const auto transfer = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_TransferFunction);
    if (transfer && (CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ) ||
                     CFEqual(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG))) {
        throw std::runtime_error("HDR input requires an HDR metadata pipeline; P2 accepts SDR only");
    }
    const auto atoms = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID()) {
        for (const auto key : {CFSTR("dvcC"), CFSTR("dvvC"), CFSTR("dvwC")}) {
            if (CFDictionaryContainsKey(static_cast<CFDictionaryRef>(atoms), key)) {
                throw std::runtime_error("Dolby Vision input is not supported by the P2 SDR pipeline");
            }
        }
    }
}

struct Frame {
    CFRef<CVPixelBufferRef> pixel;
    CMTime pts;
    CMTime duration;
};

class HardwareDecoder {
public:
    explicit HardwareDecoder(CMFormatDescriptionRef format) : format_(hardware_decode_format(format)) {
        validate_source(format);
        const void* keys[] = {kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder};
        const void* values[] = {kCFBooleanTrue};
        CFRef specification(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const auto pixel_format = static_cast<std::int32_t>(source_pixel_format(format));
        CFRef pixel_number(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format));
        CFRef surface(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const void* pixel_keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferMetalCompatibilityKey, kCVPixelBufferIOSurfacePropertiesKey};
        const void* pixel_values[] = {pixel_number.get(), kCFBooleanTrue, surface.get()};
        CFRef attributes(CFDictionaryCreate(kCFAllocatorDefault, pixel_keys, pixel_values, 3,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        VTDecompressionOutputCallbackRecord callback{&receive, this};
        check_status(VTDecompressionSessionCreate(kCFAllocatorDefault, format_.get(), specification.get(), attributes.get(),
            &callback, &session_), "VTDecompressionSessionCreate [hardware required]");
        if (!session_) throw std::runtime_error("No hardware decompression session returned");
        // Guard creation's remaining operations, because a throwing constructor has no destructor.
        try { verify(); }
        catch (...) { VTDecompressionSessionInvalidate(session_); CFRelease(session_); session_ = nullptr; throw; }
    }
    ~HardwareDecoder() {
        if (session_) { VTDecompressionSessionInvalidate(session_); CFRelease(session_); }
    }
    std::optional<Frame> decode(CMSampleBufferRef sample) {
        frame_.reset();
        error_ = nullptr;
        called_ = false;
        suppressed_ = sample_flag(sample, kCMSampleAttachmentKey_DoNotDisplay);
        std::optional<CFRef<CMSampleBufferRef>> adapted;
        if (CMFormatDescriptionGetMediaSubType(CMSampleBufferGetFormatDescription(sample)) == 0x68657631U) {
            const CMSampleTimingInfo timing{CMSampleBufferGetOutputDuration(sample),
                CMSampleBufferGetOutputPresentationTimeStamp(sample), CMSampleBufferGetOutputDecodeTimeStamp(sample)};
            const auto size = CMSampleBufferGetTotalSampleSize(sample);
            CMSampleBufferRef raw = nullptr;
            check_status(CMSampleBufferCreateReady(kCFAllocatorDefault, CMSampleBufferGetDataBuffer(sample), format_.get(),
                1, 1, &timing, 1, &size, &raw), "Adapt hev1 compressed sample");
            adapted.emplace(raw);
            CMPropagateAttachments(sample, raw);
            if (sample_flag(sample, kCMSampleAttachmentKey_NotSync)) {
                const auto array = CMSampleBufferGetSampleAttachmentsArray(raw, true);
                auto entry = static_cast<CFMutableDictionaryRef>(const_cast<void*>(CFArrayGetValueAtIndex(array, 0)));
                CFDictionarySetValue(entry, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
            }
            sample = raw;
        }
        VTDecodeInfoFlags flags = 0;
        // No async/temporal flags: callback finishes before DecodeFrame returns.
        check_status(VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr, &flags),
            "VTDecompressionSessionDecodeFrame [hardware required]");
        if (error_) std::rethrow_exception(error_);
        if (!suppressed_ && (!called_ || !frame_ || (flags & kVTDecodeInfo_FrameDropped))) {
            throw std::runtime_error("Hardware decoder did not return the submitted display frame");
        }
        return std::move(frame_);
    }
    void finish() {
        check_status(VTDecompressionSessionWaitForAsynchronousFrames(session_), "VTDecompressionSessionWaitForAsynchronousFrames");
        if (error_) std::rethrow_exception(error_);
        verify();
    }
private:
    void verify() {
        CFTypeRef raw = nullptr;
        check_status(VTSessionCopyProperty(session_, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
            kCFAllocatorDefault, &raw), "VTSessionCopyProperty(UsingHardwareAcceleratedVideoDecoder)");
        CFRef hardware(raw);
        if (!CFEqual(hardware.get(), kCFBooleanTrue)) throw std::runtime_error("Hardware decoder not confirmed; software fallback is forbidden");
    }
    static void receive(void* context, void*, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                        CMTime pts, CMTime duration) noexcept {
        auto& self = *static_cast<HardwareDecoder*>(context);
        try {
            check_status(status, "VideoToolbox decode callback");
            if (self.called_) throw std::runtime_error("Multiple decoded frames for one input sample are unsupported");
            self.called_ = true;
            if (self.suppressed_) return;
            if (!image || (flags & kVTDecodeInfo_FrameDropped)) throw std::runtime_error("Hardware decoder dropped a display frame");
            require_time(pts, "decoded PTS");
            CFRetain(image);
            self.frame_.emplace(Frame{CFRef(image), pts, duration});
        } catch (...) { self.error_ = std::current_exception(); }
    }
    CFRef<CMVideoFormatDescriptionRef> format_;
    VTDecompressionSessionRef session_ = nullptr;
    std::optional<Frame> frame_;
    std::exception_ptr error_;
    bool called_ = false;
    bool suppressed_ = false;
};

class DisplayFrames {
public:
    DisplayFrames(ContainerInput& input, const volatile std::sig_atomic_t& cancelled)
        : input_(input), cancelled_(cancelled), decoder_(input.info().video_format) {}
    std::unique_ptr<Frame> next() {
        while (!cancelled_) {
            if (!frames_.empty() && (eof_ || (CMTIME_IS_NUMERIC(watermark_) && CMTimeCompare(frames_.front()->pts, watermark_) <= 0))) {
                auto frame = std::move(frames_.front());
                frames_.erase(frames_.begin());
                if (CMTIME_IS_NUMERIC(last_pts_) && CMTimeCompare(frame->pts, last_pts_) <= 0) {
                    throw std::runtime_error("Video PTS is not strictly increasing after reordering");
                }
                last_pts_ = frame->pts;
                return frame;
            }
            if (eof_) return nullptr;
            auto raw = input_.read_video();
            if (!raw) { decoder_.finish(); eof_ = true; continue; }
            CFRef sample(raw);
            if (CMSampleBufferGetNumSamples(sample.get()) != 1) throw std::runtime_error("Expected one compressed video frame per sample buffer; got " + std::to_string(CMSampleBufferGetNumSamples(sample.get())));
            if (!CMFormatDescriptionEqual(CMSampleBufferGetFormatDescription(sample.get()), input_.info().video_format)) {
                throw std::runtime_error("Video format changes mid-stream are not supported in P2");
            }
            const auto pts = CMSampleBufferGetOutputPresentationTimeStamp(sample.get());
            auto dts = CMSampleBufferGetOutputDecodeTimeStamp(sample.get());
            require_time(pts, "video PTS");
            if (!CMTIME_IS_VALID(dts)) dts = pts;
            require_time(dts, "video DTS");
            if (CMTimeCompare(pts, dts) < 0 || (CMTIME_IS_NUMERIC(watermark_) && CMTimeCompare(dts, watermark_) <= 0)) {
                throw std::runtime_error("Unsupported video timing: DTS must increase and must not exceed PTS");
            }
            watermark_ = dts;
            auto frame = decoder_.decode(sample.get());
            if (frame) {
                if (CMTimeCompare(frame->pts, input_.info().video_range.start) < 0) continue;
                if (frames_.size() >= 32) throw std::runtime_error("Video reorder queue exceeds P2's 32-frame limit");
                frames_.push_back(std::make_unique<Frame>(std::move(*frame)));
                std::ranges::sort(frames_, [](const auto& a, const auto& b) { return CMTimeCompare(a->pts, b->pts) < 0; });
            }
        }
        decoder_.finish();
        return nullptr;
    }
private:
    ContainerInput& input_;
    const volatile std::sig_atomic_t& cancelled_;
    HardwareDecoder decoder_;
    std::vector<std::unique_ptr<Frame>> frames_;
    CMTime watermark_ = kCMTimeInvalid;
    CMTime last_pts_ = kCMTimeInvalid;
    bool eof_ = false;
};

class EncodedVideo {
public:
    EncodedVideo(ContainerInput& input, EncodeConfig config, const volatile std::sig_atomic_t& cancelled)
        : input_(input), config_(std::move(config)), frames_(input, cancelled), cancelled_(cancelled) {
        current_ = frames_.next();
        if (!current_) {
            if (!cancelled_) throw std::runtime_error("Input contains no displayable video frames");
            return;
        }
        auto pixel = current_->pixel.get();
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(input_.info().video_format);
        config_.video.width = dimensions.width;
        config_.video.height = dimensions.height;
        std::optional<CFRef<CMVideoFormatDescriptionRef>> filtered_format;
        if (needs_gpu_filter(config_, pixel)) {
            filter_ = std::make_unique<GpuFilter>(make_filter_plan(config_, input_.info().video_format, pixel));
            filtered_.emplace(filter_->process(pixel));
            pixel = filtered_->get();
            config_.video.width = static_cast<int>(CVPixelBufferGetWidth(pixel));
            config_.video.height = static_cast<int>(CVPixelBufferGetHeight(pixel));
            CMVideoFormatDescriptionRef raw = nullptr;
            check_status(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel, &raw), "Describe filtered frame");
            filtered_format.emplace(raw);
        }
        create_hardware_session(session_, config_.video, &receive, this, CVPixelBufferGetPixelFormatType(pixel));
        configure_hardware_encoder(session_.value, config_, filtered_format ? filtered_format->get() : input_.info().video_format);
        check_status(VTCompressionSessionPrepareToEncodeFrames(session_.value), "Prepare container video encoder");
        verify_hardware(session_.value);
    }
    ~EncodedVideo() {
        // Invalidate before destroying callback state (session_ is otherwise destroyed last).
        if (session_.value) VTCompressionSessionInvalidate(session_.value);
    }
    CMSampleBufferRef next() {
        if (!current_ || cancelled_) return nullptr;
        auto following = frames_.next();
        if (cancelled_) return nullptr;
        const auto duration = following ? CMTimeSubtract(following->pts, current_->pts) : current_->duration;
        require_time(duration, "video frame duration");
        if (CMTimeCompare(duration, kCMTimeZero) <= 0) throw std::runtime_error("Video frame duration must be positive");
        auto pixel = current_->pixel.get();
        if (filter_) {
            if (!filtered_) filtered_.emplace(filter_->process(pixel));
            pixel = filtered_->get();
            ++gpu_frames_;
        }
        if (CVPixelBufferGetWidth(pixel) != static_cast<std::size_t>(config_.video.width) ||
            CVPixelBufferGetHeight(pixel) != static_cast<std::size_t>(config_.video.height)) {
            throw std::runtime_error("Decoder changed video dimensions");
        }
        error_ = nullptr;
        output_.reset();
        VTEncodeInfoFlags flags = 0;
        check_status(VTCompressionSessionEncodeFrame(session_.value, pixel, current_->pts, duration,
            nullptr, nullptr, &flags), "Encode transcoded frame");
        check_status(VTCompressionSessionCompleteFrames(session_.value, kCMTimeInvalid), "Drain transcoded frame");
        if (error_) std::rethrow_exception(error_);
        if (!output_ || (flags & kVTEncodeInfo_FrameDropped)) throw std::runtime_error("Encoder dropped a transcoded frame");
        if (CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(output_->get()), current_->pts) != 0 ||
            CMTimeCompare(CMSampleBufferGetDuration(output_->get()), duration) != 0) {
            throw std::runtime_error("Encoder changed the source timeline");
        }
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(CMSampleBufferGetFormatDescription(output_->get()));
        if (dimensions.width != config_.video.width || dimensions.height != config_.video.height) {
            throw std::runtime_error("Encoder changed source dimensions");
        }
        if (filter_) {
            const auto format = CMSampleBufferGetFormatDescription(output_->get());
            if (source_pixel_format(format) != CVPixelBufferGetPixelFormatType(pixel)) {
                throw std::runtime_error("Encoder changed filtered bit depth or color range");
            }
            for (const auto key : {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey, kCVImageBufferYCbCrMatrixKey}) {
                if (const auto raw = CVBufferCopyAttachment(pixel, key, nullptr)) {
                    CFRef expected(raw);
                    const auto actual = CMFormatDescriptionGetExtension(format, key);
                    if (!actual || !CFEqual(actual, expected.get())) throw std::runtime_error("Encoder changed filtered color metadata");
                }
            }
            // H.264/HEVC default chroma siting is left when the location is omitted.
            if (const auto location = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_ChromaLocationTopField)) {
                if (!CFEqual(location, kCMFormatDescriptionChromaLocation_Left)) throw std::runtime_error("Encoder changed filtered chroma location");
            }
        }
        filtered_.reset();
        current_ = std::move(following);
        CFRetain(output_->get());
        return output_->get();
    }
    [[nodiscard]] std::int64_t gpu_frames() const { return gpu_frames_; }
    void finish() {
        if (session_.value) {
            check_status(VTCompressionSessionCompleteFrames(session_.value, kCMTimeInvalid), "Drain final transcoded frames");
            if (error_) std::rethrow_exception(error_);
            verify_hardware(session_.value);
        }
    }
private:
    static void receive(void* context, void*, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) noexcept {
        auto& self = *static_cast<EncodedVideo*>(context);
        try {
            check_status(status, "VideoToolbox transcode encode callback");
            if (!sample || (flags & kVTEncodeInfo_FrameDropped)) throw std::runtime_error("VideoToolbox dropped transcoded frame");
            if (self.output_) throw std::runtime_error("Encoder output queue exceeded one outstanding frame");
            CFRetain(sample);
            self.output_.emplace(sample);
        } catch (...) { self.error_ = std::current_exception(); }
    }
    CompressionSession session_;
    std::unique_ptr<GpuFilter> filter_;
    std::optional<CFRef<CVPixelBufferRef>> filtered_;
    std::int64_t gpu_frames_ = 0;
    ContainerInput& input_;
    EncodeConfig config_;
    DisplayFrames frames_;
    const volatile std::sig_atomic_t& cancelled_;
    std::unique_ptr<Frame> current_;
    std::optional<CFRef<CMSampleBufferRef>> output_;
    std::exception_ptr error_;
};

using Sample = std::optional<CFRef<CMSampleBufferRef>>;
void adopt(Sample& sample, CMSampleBufferRef raw) { sample.reset(); if (raw) sample.emplace(raw); }
}

CMSampleBufferRef map_container_sample(CMSampleBufferRef sample, CMTimeMapping mapping) {
    if (CMSampleBufferGetNumSamples(sample) != 1) throw std::runtime_error("Sample cursor returned more than one compressed packet");
    CMSampleTimingInfo timing{};
    check_status(CMSampleBufferGetSampleTimingInfo(sample, 0, &timing), "Read container sample timing");
    require_time(timing.presentationTimeStamp, "sample PTS");
    require_time(mapping.source.start, "edit source start");
    require_time(mapping.target.start, "edit target start");
    require_time(mapping.source.duration, "edit source duration");
    require_time(mapping.target.duration, "edit target duration");
    if (CMTimeCompare(mapping.source.duration, mapping.target.duration) != 0) throw std::runtime_error("Retimed edits are not supported");
    const auto offset = CMTimeSubtract(mapping.target.start, mapping.source.start);
    const auto movie_pts = CMTimeAdd(timing.presentationTimeStamp, offset);
    require_time(movie_pts, "mapped video PTS");
    CMSampleBufferRef raw = nullptr;
    check_status(CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sample, 1, &timing, &raw), "Map sample to movie timeline");
    CFRef mapped(raw);
    check_status(CMSampleBufferSetOutputPresentationTimeStamp(mapped.get(), movie_pts), "Set movie presentation timestamp");
    CFRetain(mapped.get());
    return mapped.get();
}

void validate_video_edit(CMTimeMapping mapping, CMTime first_pts, CMTime last_pts, CMTime last_duration) {
    require_time(first_pts, "first video PTS");
    require_time(last_pts, "last video PTS");
    require_time(last_duration, "last video duration");
    if (CMTimeCompare(mapping.source.start, first_pts) > 0 ||
        CMTimeCompare(CMTimeRangeGetEnd(mapping.source), CMTimeSubtract(last_pts, last_duration)) < 0) {
        throw std::runtime_error("P2 copies the complete video track; edits that trim stored video frames are unsupported");
    }
    // Some source containers declare a rounded duration before the final frame's
    // PTS. Preserve that whole final frame, but reject material trailing trims.
}

EncodeResult transcode_file(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled) try {
    ContainerInput input(config, cancelled);
    EncodedVideo encoder(input, config, cancelled);
    Sample video;
    adopt(video, encoder.next());
    if (cancelled) { encoder.finish(); return {0, 0, true}; }
    if (!video) throw std::runtime_error("No encoded video output");
    ContainerOutput output(config, input.info(), CMSampleBufferGetFormatDescription(video->get()));
    Sample audio;
    adopt(audio, input.read_audio());
    bool video_ended = false;
    bool audio_ended = false;
    std::int64_t video_count = 0;
    std::int64_t audio_count = 0;
    CMTime last_audio = kCMTimeInvalid;
    auto last_progress = std::chrono::steady_clock::now();
    while ((video || audio) && !cancelled) {
        if (!video && !video_ended) { output.end_video(); video_ended = true; }
        if (!audio && !audio_ended) { output.end_audio(); audio_ended = true; }
        const bool video_ready = video && output.video_ready();
        const bool audio_ready = audio && output.audio_ready();
        if (audio_ready && (!video_ready || CMTimeCompare(CMSampleBufferGetOutputPresentationTimeStamp(audio->get()),
                                                        CMSampleBufferGetPresentationTimeStamp(video->get())) <= 0)) {
            if (!CMAudioFormatDescriptionEqual(CMSampleBufferGetFormatDescription(audio->get()), input.info().audio_format,
                kCMAudioFormatDescriptionMask_StreamBasicDescription | kCMAudioFormatDescriptionMask_MagicCookie |
                kCMAudioFormatDescriptionMask_ChannelLayout, nullptr)) {
                throw std::runtime_error("Audio format changes mid-stream are not supported");
            }
            const auto pts = CMSampleBufferGetPresentationTimeStamp(audio->get());
            require_time(pts, "audio PTS");
            if (CMTIME_IS_NUMERIC(last_audio) && CMTimeCompare(pts, last_audio) <= 0) {
                throw std::runtime_error("Audio PTS must strictly increase; duplicate/overlapping source timestamps (use --no-audio)");
            }
            last_audio = pts;
            output.append_audio(audio->get());
            audio_count += CMSampleBufferGetNumSamples(audio->get());
            adopt(audio, input.read_audio());
        } else if (video_ready) {
            output.append_video(video->get());
            ++video_count;
            adopt(video, encoder.next());
        } else {
            if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(30)) {
                throw std::runtime_error("Container writer made no progress for 30 seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        last_progress = std::chrono::steady_clock::now();
    }
    encoder.finish();
    if (cancelled) return {video_count, 0, true, audio_count};
    if (!video_ended) output.end_video();
    if (!audio_ended) output.end_audio();
    const auto bytes = output.finish(cancelled);
    return {video_count, bytes, bytes == 0, audio_count, encoder.gpu_frames()};
} catch (const MediaCancelled&) {
    return {0, 0, true};
}

} // namespace vtenc
