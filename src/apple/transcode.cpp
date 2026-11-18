#include "vtenc/encoder.hpp"
#include "vtenc/status.hpp"
#include "encoder_session.hpp"
#include "media_io.hpp"
#include "source_format.hpp"
#include "gpu_filter.hpp"
#include "bob.hpp"
#include "ivtc.hpp"
#include "yadif.hpp"
#include "decoded_frames.hpp"
#include "decode_policy.hpp"
#include "ffmpeg_utils.hpp"
#include "timeline.hpp"
#include "audio_transcode.hpp"
#include "subtitle.hpp"
#include "output_metadata.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
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

void validate_source(CMFormatDescriptionRef format, bool allow_interlaced, bool colorspace) {
    const auto codec = CMFormatDescriptionGetMediaSubType(format);
    if (codec == 0x64766831U || codec == 0x64766865U) throw std::runtime_error("Dolby Vision input is not supported by the P2 SDR pipeline");
    const auto fields = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FieldCount);
    if (fields) {
        int count = 0;
        if (CFGetTypeID(fields) != CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(fields), kCFNumberIntType, &count)) {
            throw std::runtime_error("Invalid video field count");
        }
        if (count != 1 && count != 2) throw std::runtime_error("Unsupported video field count");
        if (count == 2 && !allow_interlaced) throw std::runtime_error("Interlaced video requires --deinterlace bob, --vpp-ivtc or --vpp-yadif");
    }
    const auto transfer = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_TransferFunction);
    if (!colorspace && transfer && (CFEqual(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ) ||
                     CFEqual(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG))) {
        throw std::runtime_error("HDR input requires explicit --vpp-colorspace conversion");
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

using Frame = DecodedFrame;

class HardwareDecoder {
public:
    HardwareDecoder(CMFormatDescriptionRef format, bool allow_interlaced, bool pair_fields) try : format_(hardware_decode_format(format)) {
        const auto codec = CMFormatDescriptionGetMediaSubType(format);
        if (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC && codec != 0x68657631U && codec != kCMVideoCodecType_AV1) {
            throw std::runtime_error("Native hardware decoder supports H.264, HEVC and AV1 only");
        }
        const void* keys[] = {kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder};
        const void* values[] = {kCFBooleanTrue};
        CFRef specification(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        bool interlaced_coding = false;
        const auto pixel_format = static_cast<std::int32_t>(source_pixel_format(format, allow_interlaced, &interlaced_coding));
        packed_fields_ = pair_fields && interlaced_coding && codec != kCMVideoCodecType_H264;
        CFRef pixel_number(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format));
        CFRef surface(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const void* pixel_keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferMetalCompatibilityKey, kCVPixelBufferIOSurfacePropertiesKey};
        const void* pixel_values[] = {pixel_number.get(), kCFBooleanTrue, surface.get()};
        CFRef attributes(CFDictionaryCreate(kCFAllocatorDefault, pixel_keys, pixel_values, 3,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        VTDecompressionOutputCallbackRecord callback{&receive, this};
        check_status(VTDecompressionSessionCreate(kCFAllocatorDefault, format_.get(), specification.get(), attributes.get(),
            &callback, &session_), allow_interlaced ? "VTDecompressionSessionCreate [hardware required; woven source]"
                                      : "VTDecompressionSessionCreate [hardware required]");
        if (!session_) throw std::runtime_error("No hardware decompression session returned");
        // Guard creation's remaining operations, because a throwing constructor has no destructor.
        try {
            // Progressive-coded rasters can contain woven fields when explicitly
            // overridden by the user; no decoder field handling applies to those.
            // HEVC field pictures must remain one packed raster per sample.
            // H.264 PAFF/MBAFF continues to request woven decoder output.
            if (allow_interlaced && interlaced_coding && !packed_fields_) {
                check_status(VTSessionSetProperty(session_, kVTDecompressionPropertyKey_FieldMode,
                    kVTDecompressionProperty_FieldMode_BothFields), "Set decoder FieldMode=BothFields [Metal deinterlacing requires woven fields]");
                CFTypeRef raw_mode = nullptr;
                check_status(VTSessionCopyProperty(session_, kVTDecompressionPropertyKey_FieldMode, nullptr, &raw_mode), "Read decoder FieldMode");
                CFRef mode(raw_mode);
                if (!CFEqual(mode.get(), kVTDecompressionProperty_FieldMode_BothFields)) throw std::runtime_error("Decoder did not confirm woven BothFields output");
            }
            verify();
        }
        catch (...) { VTDecompressionSessionInvalidate(session_); CFRelease(session_); session_ = nullptr; throw; }
    }
    catch (const std::runtime_error& error) { throw HardwareDecodeError(error); }
    ~HardwareDecoder() {
        if (session_) { VTDecompressionSessionInvalidate(session_); CFRelease(session_); }
    }
    std::optional<Frame> decode(CMSampleBufferRef sample) try {
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
    catch (const std::runtime_error& error) { throw HardwareDecodeError(error); }
    void finish() try {
        check_status(VTDecompressionSessionWaitForAsynchronousFrames(session_), "VTDecompressionSessionWaitForAsynchronousFrames");
        if (error_) std::rethrow_exception(error_);
        verify();
    } catch (const std::runtime_error& error) { throw HardwareDecodeError(error); }
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
            if (self.packed_fields_) {
                const auto size = CMVideoFormatDescriptionGetDimensions(self.format_.get());
                if (CVPixelBufferGetWidth(image) != static_cast<std::size_t>(size.width) ||
                    CVPixelBufferGetHeight(image) != static_cast<std::size_t>(size.height))
                    throw std::runtime_error("Hardware HEVC decoder did not preserve the packed single-field raster");
            }
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
    bool packed_fields_ = false;
};

class DisplayFrames final : public DecodedFrames {
public:
    DisplayFrames(ContainerInput& input, const volatile std::sig_atomic_t& cancelled, bool allow_interlaced, bool read_cadence)
        : input_(input), cancelled_(cancelled), decoder_(input.hardware_video_format(), allow_interlaced, read_cadence), read_cadence_(read_cadence) {}
    std::unique_ptr<Frame> next() override {
        while (!cancelled_) {
            if (!frames_.empty() && (eof_ || frames_.size() >= 32)) {
                auto frame = std::move(frames_.front());
                frames_.erase(frames_.begin());
                if (CMTIME_IS_NUMERIC(last_pts_) && CMTimeCompare(frame->pts, last_pts_) <= 0) {
                    throw std::runtime_error("Video PTS is not strictly increasing after reordering");
                }
                last_pts_ = frame->pts;
                return frame;
            }
            if (eof_) return nullptr;
            FrameCadence cadence;
            auto raw = input_.read_video(read_cadence_ ? &cadence : nullptr);
            if (!raw) { decoder_.finish(); eof_ = true; continue; }
            CFRef sample(raw);
            if (CMSampleBufferGetNumSamples(sample.get()) != 1) throw std::runtime_error("Expected one compressed video frame per sample buffer; got " + std::to_string(CMSampleBufferGetNumSamples(sample.get())));
            if (!CMFormatDescriptionEqual(CMSampleBufferGetFormatDescription(sample.get()), input_.info().video_format)) {
                throw std::runtime_error("Video format changes mid-stream are not supported in P2");
            }
            const auto pts = CMSampleBufferGetOutputPresentationTimeStamp(sample.get());
            require_time(pts, "video PTS");
            // avformat supplies decode order. Some containers (notably Matroska)
            // omit initial DTS; never substitute reordered PTS for missing DTS.
            collect_hdr_sei(sample.get(),hdr_.get());
            auto frame = decoder_.decode(sample.get());
            if (frame) {
                CVBufferSetAttachments(frame->pixel.get(),hdr_.get(),kCVAttachmentMode_ShouldPropagate);
                frame->cadence = cadence;
                if (CMTIME_IS_NUMERIC(input_.info().video_range.start) && CMTimeCompare(frame->pts, input_.info().video_range.start) < 0) continue;
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
    CFRef<CFMutableDictionaryRef> hdr_{CFDictionaryCreateMutable(nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks)};
    bool read_cadence_;
    std::vector<std::unique_ptr<Frame>> frames_;
    CMTime last_pts_ = kCMTimeInvalid;
    bool eof_ = false;
};

class ProcessedFrames final : public DecodedFrames {
public:
    ProcessedFrames(ContainerInput& input, EncodeConfig config, const volatile std::sig_atomic_t& cancelled)
        : input_(input), config_(std::move(config)), cancelled_(cancelled) {
        validate_source(input.info().video_format, config_.filter.bob || config_.filter.ivtc.enabled || config_.filter.yadif.enabled, config_.filter.colorspace.enabled);
        if (config_.decode == DecodeMode::software) frames_ = std::make_unique<SoftwareFrames>(config_, input, cancelled);
        else frames_ = std::make_unique<DisplayFrames>(input, cancelled, config_.filter.bob || config_.filter.ivtc.enabled || config_.filter.yadif.enabled,
            config_.filter.yadif.enabled || (config_.filter.ivtc.enabled && (input.video_parameters()->codec_id == AV_CODEC_ID_HEVC ||
                config_.filter.ivtc.expand != RffExpansion::off || config_.filter.ivtc.mixed)));
        if (config_.filter.ivtc.enabled) frames_ = std::make_unique<IvtcFrames>(std::move(frames_), input.info().video_format, config_.filter, cancelled);
        if (config_.filter.yadif.enabled) frames_ = std::make_unique<YadifFrames>(std::move(frames_), input.info().video_format, config_.filter, cancelled);
        current_ = frames_->next();
        if (!current_) {
            if (!cancelled_) throw std::runtime_error("Input contains no displayable video frames");
            return;
        }
        auto pixel = current_->pixel.get();
        std::optional<CFRef<CMVideoFormatDescriptionRef>> assembled_format;
        auto source_format = input_.info().video_format;
        if (config_.filter.ivtc.enabled || config_.filter.yadif.enabled) {
            CMVideoFormatDescriptionRef raw = nullptr;
            check_status(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel, &raw), "Describe deinterlaced display raster");
            assembled_format.emplace(raw);
            source_format = raw;
        }
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(source_format);
        config_.video.width = dimensions.width;
        config_.video.height = dimensions.height;
        if (needs_gpu_filter(config_, pixel)) {
            filter_ = std::make_unique<GpuFilter>(make_filter_plan(config_, source_format, pixel));
            if (config_.filter.bob) first_field_ = bob_first_field(config_.filter.field_order, input_.info().video_format, pixel);
            filtered_.emplace(filter_->process(pixel, config_.filter.bob ? static_cast<int>(first_field_) : -1));
            pixel = filtered_->get();
            config_.video.width = static_cast<int>(CVPixelBufferGetWidth(pixel));
            config_.video.height = static_cast<int>(CVPixelBufferGetHeight(pixel));
        }
    }
    std::unique_ptr<Frame> next() override {
        if (!current_ || cancelled_) return nullptr;
        if (field_index_ == 0) {
            following_ = frames_->next();
            if (cancelled_) return nullptr;
            frame_duration_ = following_ ? CMTimeSubtract(following_->pts, current_->pts) : current_->duration;
            if (!following_ && (!CMTIME_IS_NUMERIC(frame_duration_) || frame_duration_.value == 0) && CMTIME_IS_NUMERIC(previous_interval_)) {
                // TS commonly provides PTS but no packet durations. There is no
                // next PTS for the last frame: explicitly reuse the last observed
                // interval, never invent a nominal frame rate or shift timestamps.
                frame_duration_ = previous_interval_;
                std::cerr << "vtenc: input omits final frame duration; using preceding PTS interval "
                          << frame_duration_.value << '/' << frame_duration_.timescale << " seconds\n";
            }
            if (following_) previous_interval_ = frame_duration_;
            require_time(frame_duration_, "video frame duration");
            if (CMTimeCompare(frame_duration_, kCMTimeZero) <= 0) throw std::runtime_error("Video frame duration must be positive");
            if (config_.filter.bob) first_field_ = bob_first_field(config_.filter.field_order, input_.info().video_format, current_->pixel.get());
        }
        if (cancelled_) return nullptr;
        const auto timing = config_.filter.bob ? bob_time(current_->pts, frame_duration_, field_index_) : BobTime{current_->pts, frame_duration_};
        const auto duration = timing.duration;
        const auto pts = timing.pts;
        auto pixel = current_->pixel.get();
        if (filter_) {
            if (!filtered_) filtered_.emplace(filter_->process(pixel, config_.filter.bob ? static_cast<int>(first_field_ ^ field_index_) : -1));
            pixel = filtered_->get();
            ++gpu_frames_;
        }
        if (CVPixelBufferGetWidth(pixel) != static_cast<std::size_t>(config_.video.width) ||
            CVPixelBufferGetHeight(pixel) != static_cast<std::size_t>(config_.video.height)) {
            throw std::runtime_error("Decoder changed video dimensions");
        }
        CFRetain(pixel);
        auto result = std::make_unique<Frame>(Frame{CFRef(pixel), pts, duration, current_->cadence});
        filtered_.reset();
        if (config_.filter.bob && field_index_ == 0) field_index_ = 1;
        else { current_ = std::move(following_); field_index_ = 0; }
        return result;
    }
    [[nodiscard]] std::int64_t gpu_frames() const { return gpu_frames_; }
    [[nodiscard]] bool filtered() const { return static_cast<bool>(filter_); }
private:
    std::unique_ptr<GpuFilter> filter_;
    std::optional<CFRef<CVPixelBufferRef>> filtered_;
    std::int64_t gpu_frames_ = 0;
    ContainerInput& input_;
    EncodeConfig config_;
    std::unique_ptr<DecodedFrames> frames_;
    const volatile std::sig_atomic_t& cancelled_;
    std::unique_ptr<Frame> current_, following_;
    CMTime frame_duration_ = kCMTimeInvalid, previous_interval_ = kCMTimeInvalid;
    unsigned field_index_ = 0, first_field_ = 0;
};

class EncodedVideo {
public:
    EncodedVideo(ContainerInput& input, EncodeConfig config, const volatile std::sig_atomic_t& cancelled)
        : config_(std::move(config)), cancelled_(cancelled), rate_verifier_(config_) {
        auto processed = std::make_unique<ProcessedFrames>(input, config_, cancelled);
        processed_ = processed.get();
        frames_ = std::move(processed);
        if (config_.sync != SyncMode::vfr)
            frames_ = std::make_unique<CfrFrames>(std::move(frames_), config_.sync, config_.fps_num, config_.fps_den, cancelled);
        current_ = frames_->next();
        if (!current_) { if (!cancelled_) throw std::runtime_error("No processed video frames"); return; }
        const auto pixel = current_->pixel.get();
        config_.video.width = static_cast<int>(CVPixelBufferGetWidth(pixel));
        config_.video.height = static_cast<int>(CVPixelBufferGetHeight(pixel));
        CMVideoFormatDescriptionRef raw = nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, pixel, &raw), "Describe processed encoder input");
        CFRef format(raw);
        CFRetain(raw); pixel_metadata_.emplace(raw);
        // Preserve track-level aperture/SAR for the existing unfiltered native
        // path; processed/software frames describe their own output raster.
        const auto metadata_source=processed_->filtered() || config_.decode==DecodeMode::software || config_.filter.ivtc.enabled
            ? format.get() : input.info().video_format;
        metadata_.emplace(output_format(config_,metadata_source,input.info().video_format,pixel));
        if (!config_.subtitles.empty()) {
            double sar=1.0;
            if (config_.dar) {
                const auto ratio=display_sar(*config_.dar,config_.video.width,config_.video.height,input.info().transform);
                sar=static_cast<double>(ratio[0])/ratio[1];
            } else if (auto value=CMFormatDescriptionGetExtension(raw,kCVImageBufferPixelAspectRatioKey)) {
                if (CFGetTypeID(value)!=CFDictionaryGetTypeID()) throw std::runtime_error("Invalid subtitle SAR");
                auto d=static_cast<CFDictionaryRef>(value); int x=0,y=0;
                auto a=CFDictionaryGetValue(d,kCVImageBufferPixelAspectRatioHorizontalSpacingKey);
                auto b=CFDictionaryGetValue(d,kCVImageBufferPixelAspectRatioVerticalSpacingKey);
                if (!a||!b||CFGetTypeID(a)!=CFNumberGetTypeID()||CFGetTypeID(b)!=CFNumberGetTypeID()||
                    !CFNumberGetValue(static_cast<CFNumberRef>(a),kCFNumberIntType,&x)||
                    !CFNumberGetValue(static_cast<CFNumberRef>(b),kCFNumberIntType,&y)||x<=0||y<=0) throw std::runtime_error("Invalid subtitle SAR");
                sar=static_cast<double>(x)/y;
            }
            subtitles_=std::make_unique<SubtitleRenderer>(config_.subtitles,config_.video.width,config_.video.height,sar);
            compositor_=std::make_unique<GpuSubtitles>(pixel);
        }
        create_hardware_session(session_, config_.video, &receive, this, CVPixelBufferGetPixelFormatType(pixel));
        configure_hardware_encoder(session_.value, config_, metadata_->get());
        if (config_.dar) {
            sar_ = display_sar(*config_.dar, config_.video.width, config_.video.height, input.info().transform);
            set_display_sar(session_.value, *sar_);
        }
        check_status(VTCompressionSessionPrepareToEncodeFrames(session_.value), "Prepare container video encoder");
        verify_hardware(session_.value);
        report_encoder_controls(session_.value, config_);
        if (config_.status) config_.status->session(config_, hardware_encoder_id(session_.value), true, CVPixelBufferGetPixelFormatType(pixel));
    }
    ~EncodedVideo() {
        if (session_.value) VTCompressionSessionInvalidate(session_.value);
    }
    CMSampleBufferRef next() {
        if (advance_) { current_.reset(); current_ = frames_->next(); }
        advance_ = true;
        if (!current_ || cancelled_) return nullptr;
        auto pixel = current_->pixel.get();
        CMVideoFormatDescriptionRef current_format=nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,pixel,&current_format),"Describe current output frame");
        CFRef current_metadata(current_format);
        verify_output_metadata(pixel_metadata_->get(),current_format);
        std::optional<CFRef<CVPixelBufferRef>> composed;
        const auto pts = current_->pts, duration = current_->duration;
        if (subtitles_) {
            auto images=subtitles_->render(pts);
            composed.emplace(compositor_->process(pixel,images,subtitles_->changed()));
            pixel=composed->get();
        }
        error_ = nullptr;
        output_.reset();
        const auto options = keyframe_options(config_, encoded_frames_);
        VTEncodeInfoFlags flags = 0;
        check_status(VTCompressionSessionEncodeFrame(session_.value, pixel, pts, duration,
            options.get(), nullptr, &flags), "Encode transcoded frame");
        check_status(VTCompressionSessionCompleteFrames(session_.value, kCMTimeInvalid), "Drain transcoded frame");
        if (error_) std::rethrow_exception(error_);
        if (!output_ || (flags & kVTEncodeInfo_FrameDropped)) throw std::runtime_error("Encoder dropped a transcoded frame");
        if (CMTimeCompare(CMSampleBufferGetPresentationTimeStamp(output_->get()), pts) != 0 ||
            CMTimeCompare(CMSampleBufferGetDuration(output_->get()), duration) != 0) {
            throw std::runtime_error("Encoder changed the source timeline");
        }
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(CMSampleBufferGetFormatDescription(output_->get()));
        if (dimensions.width != config_.video.width || dimensions.height != config_.video.height) {
            throw std::runtime_error("Encoder changed source dimensions");
        }
        if (processed_->filtered()) {
            const auto format = CMSampleBufferGetFormatDescription(output_->get());
            if (source_pixel_format(format) != CVPixelBufferGetPixelFormatType(pixel)) {
                throw std::runtime_error("Encoder changed filtered bit depth or color range");
            }
            // H.264/HEVC default chroma siting is left when the location is omitted.
            if (const auto location = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_ChromaLocationTopField)) {
                if (!CFEqual(location, kCMFormatDescriptionChromaLocation_Left)) throw std::runtime_error("Encoder changed filtered chroma location");
            }
        }
        verify_output_metadata(metadata_->get(),CMSampleBufferGetFormatDescription(output_->get()));
        if (sar_) verify_display_sar(CMSampleBufferGetFormatDescription(output_->get()), *sar_);
        verify_keyframe(config_, encoded_frames_++, output_->get());
        rate_verifier_.append(output_->get());
        auto normalized=normalize_hdr_sei(output_->get(),metadata_->get());
        CFRetain(normalized.get());
        return normalized.get();
    }
    [[nodiscard]] std::int64_t gpu_frames() const { return processed_->gpu_frames(); }
    void finish() {
        if (session_.value) {
            check_status(VTCompressionSessionCompleteFrames(session_.value, kCMTimeInvalid), "Drain final transcoded frames");
            if (error_) std::rethrow_exception(error_);
            verify_hardware(session_.value);
            if (!cancelled_) verify_keyframe_count(config_, encoded_frames_);
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
    std::optional<CFRef<CMVideoFormatDescriptionRef>> metadata_, pixel_metadata_;
    std::unique_ptr<SubtitleRenderer> subtitles_;
    std::unique_ptr<GpuSubtitles> compositor_;
    EncodeConfig config_;
    const volatile std::sig_atomic_t& cancelled_;
    std::unique_ptr<DecodedFrames> frames_;
    ProcessedFrames* processed_ = nullptr; // Owned by frames_ or its CFR decorator.
    std::unique_ptr<Frame> current_;
    EncoderRateVerifier rate_verifier_;
    std::int64_t encoded_frames_ = 0;
    bool advance_ = false;
    std::optional<std::array<int, 2>> sar_;
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

static EncodeResult transcode_attempt(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled, bool& replayable, bool& stdout_started) {
    if (cancelled) throw MediaCancelled{};
    ContainerInput input(config, cancelled);
    replayable = input.info().replayable;
    EncodedVideo encoder(input, config, cancelled);
    Sample video;
    adopt(video, encoder.next());
    if (cancelled) { encoder.finish(); return {0, 0, true}; }
    if (!video) throw std::runtime_error("No encoded video output");
    // After muxing starts, even the buffered header belongs to this attempt.
    // A retry must never append another stream to irreversible stdout output.
    auto output_info = input.info();
    std::unique_ptr<AudioTranscoder> audio_encoder;
    if (config.aac_audio) {
        audio_encoder = std::make_unique<AudioTranscoder>(input, config, cancelled);
        output_info = audio_encoder->output_info();
    }
    const auto read_audio = [&]() { return audio_encoder ? audio_encoder->next() : input.read_audio(); };
    auto audio = read_audio();
    stdout_started = config.output == "-";
    ContainerOutput output(config, output_info, CMSampleBufferGetFormatDescription(video->get()), cancelled);
    const auto origin = CMSampleBufferGetPresentationTimeStamp(video->get());
    const auto declared_total = input.info().video_range.duration;
    const auto total = CMTIME_IS_NUMERIC(declared_total) && declared_total.value > 0
        ? StatusTime{declared_total.value, declared_total.timescale} : StatusTime{};
    std::uint64_t video_bytes = 0;
    std::int64_t video_count = 0;
    std::int64_t audio_count = 0;
    CMTime last_audio = kCMTimeInvalid;
    while ((video || audio) && !cancelled) {
        if (audio && (!video || CMTimeCompare(media_time(audio->pts, {output_info.audio_timebase_num, output_info.audio_timescale}, "audio PTS"),
                                             CMSampleBufferGetPresentationTimeStamp(video->get())) <= 0)) {
            const auto pts = media_time(audio->pts, {output_info.audio_timebase_num, output_info.audio_timescale}, "audio PTS");
            require_time(pts, "audio PTS");
            if (CMTIME_IS_NUMERIC(last_audio) && CMTimeCompare(pts, last_audio) <= 0) {
                throw std::runtime_error("Audio PTS must strictly increase; duplicate source timestamps or overlapping packets (use --no-audio)");
            }
            last_audio = pts;
            output.append_audio(audio.get());
            ++audio_count;
            audio = read_audio();
        } else {
            output.append_video(video->get());
            ++video_count;
            video_bytes += CMSampleBufferGetTotalSampleSize(video->get());
            const auto pts = CMSampleBufferGetPresentationTimeStamp(video->get());
            const auto media = CMTimeSubtract(CMTimeAdd(pts, CMSampleBufferGetDuration(video->get())), origin);
            if (config.status) config.status->advance(video_count, {pts.value, pts.timescale}, {media.value, media.timescale}, total, video_bytes);
            adopt(video, encoder.next());
        }
    }
    encoder.finish();
    if (cancelled) return {video_count, 0, true, audio_count};
    if (config.status) config.status->check_diagnostics();
    const auto bytes = output.finish(cancelled);
    return {video_count, bytes, bytes == 0, audio_count, encoder.gpu_frames(), config.decode};
}

EncodeResult transcode_file(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled) try {
    if (config.audio_only) {
        if (config.status) config.status->attempt(DecodeMode::software);
        ContainerInput input(config, cancelled);
        AudioTranscoder encoder(input, config, cancelled);
        const auto info = encoder.output_info();
        ContainerOutput output(config, info, nullptr, cancelled);
        std::int64_t packets = 0;
        while (auto packet = encoder.next()) {
            output.append_audio(packet.get());
            ++packets;
            if (config.status) config.status->advance(0, {packet->pts, info.audio_timescale},
                {std::max<std::int64_t>(0, packet->pts + packet->duration), info.audio_timescale});
        }
        if (config.status) config.status->check_diagnostics();
        const auto bytes = output.finish(cancelled);
        return {0, bytes, bytes == 0, packets};
    }
    bool replayable = false;
    bool stdout_started = false;
    return with_decode_policy(config.decode, [&](DecodeMode mode) {
        if (config.status) config.status->attempt(mode);
        auto attempt = config;
        attempt.decode = mode;
        return transcode_attempt(attempt, cancelled, replayable, stdout_started);
    }, [&](const HardwareDecodeError& error) {
        if (cancelled) throw MediaCancelled{};
        if (stdout_started) throw MediaError(std::string(error.what()) +
            "; stdout muxing has started: cannot restart without corrupting the output; select --avsw in advance", error.os_status);
        if (!replayable) throw MediaError(std::string(error.what()) +
            "; input is not seekable: cannot restart from the beginning; select --avsw before consuming it", error.os_status);
        const auto message = "vtenc: hardware decode failed: " + std::string(error.what()) +
            "; restarting from the beginning with software decoding (one retry)";
        if (config.status) config.status->diagnostic(message, error.os_status);
        else std::cerr << message << '\n';
    });
} catch (const MediaCancelled&) {
    return {0, 0, true};
}

} // namespace vtenc
