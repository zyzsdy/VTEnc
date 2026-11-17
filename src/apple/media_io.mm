#include "media_io.hpp"
#include "cf_ref.hpp"

#import <AVFoundation/AVFoundation.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace vtenc {
namespace {
std::string text(NSString* value) { return value.UTF8String ? value.UTF8String : "(no description)"; }
template <typename Operation>
auto native(Operation operation) -> decltype(operation()) {
    @try { return operation(); }
    @catch (NSException* exception) {
        throw std::runtime_error("AVFoundation exception [" + text(exception.name) + "]: " + text(exception.reason));
    }
}
[[noreturn]] void fail(const char* operation, NSError* error) {
    throw std::runtime_error(std::string(operation) + ": " + text(error.description));
}
NSURL* url(const std::string& path) {
    NSString* string = [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
    if (!string) throw std::runtime_error("Path is not valid UTF-8");
    return [NSURL fileURLWithPath:string];
}
void numeric_time(CMTime time, const char* context) {
    if (!CMTIME_IS_NUMERIC(time) || time.epoch != 0) throw std::runtime_error(std::string("Invalid ") + context);
}
CMSampleBufferRef read_audio_sample(AVAssetReaderTrackOutput* output) {
    auto sample = [output copyNextSampleBuffer];
    // End-of-edit notifications carry neither a format nor media samples.
    while (sample && CMSampleBufferGetNumSamples(sample) == 0 &&
           !CMSampleBufferGetFormatDescription(sample) && !CMSampleBufferGetDataBuffer(sample)) {
        CFRelease(sample);
        sample = [output copyNextSampleBuffer];
    }
    return sample;
}
}

struct ContainerInput::Impl {
    struct Track {
        AVSampleCursor* cursor = nil;
        CMTimeMapping mapping = kCMTimeMappingInvalid;
        bool ended = false;
    };
    AVURLAsset* asset = nil;
    AVSampleBufferGenerator* generator = nil;
    Track video;
    AVAssetReader* audio_reader = nil;
    AVAssetReaderTrackOutput* audio_output = nil;
    std::optional<CFRef<CMSampleBufferRef>> first_audio;
    std::optional<CFRef<CMFormatDescriptionRef>> audio_format;
    NSArray* video_formats = nil;
    NSArray* audio_formats = nil;
    MediaInfo info;
    ~Impl() { [audio_reader cancelReading]; }
    CMSampleBufferRef read(Track& track) {
        if (!track.cursor || track.ended) return nullptr;
        return native([&]() -> CMSampleBufferRef { @autoreleasepool {
            auto request = [[AVSampleBufferRequest alloc] initWithStartCursor:track.cursor];
            NSError* error = nil;
            auto sample = [generator createSampleBufferForRequest:request error:&error];
            if (!sample) fail("AVSampleBufferGenerator read compressed sample", error);
            CFRef owned(sample);
            const auto step = [track.cursor stepInDecodeOrderByCount:1];
            track.ended = step == 0;
            return map_container_sample(owned.get(), track.mapping);
        }});
    }
};

ContainerInput::ContainerInput(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled)
    : impl_(std::make_unique<Impl>()) {
    native([&] { @autoreleasepool {
        struct stat file{};
        if (stat(config.input.c_str(), &file) != 0) {
            throw std::runtime_error("Open container input [" + config.input + "]: " + std::strerror(errno));
        }
        if (!S_ISREG(file.st_mode)) throw std::runtime_error("Container input must be a regular file");
        auto& state = *impl_;
        state.asset = [AVURLAsset URLAssetWithURL:url(config.input)
            options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
        // Objective-C uses the synchronous properties; Swift's load(...) replacements
        // are not available as equivalent Objective-C selectors on the macOS 13 baseline.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (state.asset.hasProtectedContent) throw std::runtime_error("Protected media cannot be transcoded");
        auto videos = [state.asset tracksWithMediaType:AVMediaTypeVideo];
        auto audios = [state.asset tracksWithMediaType:AVMediaTypeAudio];
        if (videos.count != 1) throw std::runtime_error("P2 requires exactly one video track");
        AVAssetTrack* video_track = videos[0];
        state.video_formats = video_track.formatDescriptions;
        if (state.video_formats.count != 1) throw std::runtime_error("P2 requires a constant video format description");
        state.info.video_format = (__bridge CMFormatDescriptionRef)state.video_formats[0];
        state.info.video_range = video_track.timeRange;
        state.info.video_timescale = video_track.naturalTimeScale;
        const auto transform = video_track.preferredTransform;
        state.info.transform = {transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty};
        numeric_time(state.info.video_range.start, "video track start");
        numeric_time(state.info.video_range.duration, "video track duration");
        if (state.info.video_timescale <= 0 || CMTimeCompare(state.info.video_range.start, kCMTimeZero) < 0 ||
            CMTimeCompare(state.info.video_range.duration, kCMTimeZero) <= 0) {
            throw std::runtime_error("Unsupported video track timeline");
        }
        const auto configure_track = [](AVAssetTrack* track, Impl::Track& selected) {
            selected.cursor = [track makeSampleCursorAtFirstSampleInDecodeOrder];
            if (!selected.cursor) throw std::runtime_error("Cannot read compressed sample cursor");
            bool found = false;
            for (AVAssetTrackSegment* segment in track.segments) {
                if (segment.empty) continue;
                if (found) throw std::runtime_error("P2 supports one continuous media edit per track");
                selected.mapping = segment.timeMapping;
                if (CMTimeCompare(selected.mapping.source.duration, selected.mapping.target.duration) != 0) {
                    throw std::runtime_error("Retimed track edits are not supported in P2");
                }
                found = true;
            }
            if (!found) throw std::runtime_error("No media timeline mapping in track");
        };
        state.generator = [[AVSampleBufferGenerator alloc] initWithAsset:state.asset timebase:nullptr];
        configure_track(video_track, state.video);
        auto last_video = [video_track makeSampleCursorAtLastSampleInDecodeOrder];
        if (!last_video) throw std::runtime_error("Cannot inspect final video sample");
        [last_video stepInPresentationOrderByCount:std::numeric_limits<std::int64_t>::max()];
        validate_video_edit(state.video.mapping, state.video.cursor.presentationTimeStamp,
            last_video.presentationTimeStamp, last_video.currentSampleDuration);
        if (config.copy_audio && audios.count > 0) {
            AVAssetTrack* audio_track = audios[0];
            state.audio_formats = audio_track.formatDescriptions;
            if (state.audio_formats.count != 1) throw std::runtime_error("P2 requires a constant audio format description");
            state.info.audio_format = (__bridge CMFormatDescriptionRef)state.audio_formats[0];
            state.info.audio_timescale = audio_track.naturalTimeScale;
            if (state.info.audio_timescale <= 0) throw std::runtime_error("Invalid audio track timescale");
            // Validate stored packet PTS before AVAssetReader groups them. Grouped
            // timing cannot represent malformed duplicate PTS (seen in legacy AAC).
            auto cursor = [audio_track makeSampleCursorAtFirstSampleInDecodeOrder];
            if (!cursor) throw std::runtime_error("Cannot inspect audio packet timeline");
            CMTime previous = kCMTimeInvalid;
            do {
                if (cancelled) throw MediaCancelled{};
                const auto pts = cursor.presentationTimeStamp;
                numeric_time(pts, "audio packet PTS");
                if (CMTIME_IS_NUMERIC(previous) && CMTimeCompare(pts, previous) <= 0) {
                    throw std::runtime_error("Audio PTS must strictly increase; duplicate source timestamps (use --no-audio)");
                }
                previous = pts;
            } while ([cursor stepInDecodeOrderByCount:1] == 1);
            NSError* error = nil;
            state.audio_reader = [[AVAssetReader alloc] initWithAsset:state.asset error:&error];
            if (!state.audio_reader) fail("AVAssetReader audio init", error);
            state.audio_output = [[AVAssetReaderTrackOutput alloc] initWithTrack:audio_track outputSettings:nil];
            state.audio_output.alwaysCopiesSampleData = NO;
            if (![state.audio_reader canAddOutput:state.audio_output]) throw std::runtime_error("Cannot read compressed audio");
            [state.audio_reader addOutput:state.audio_output];
            if (![state.audio_reader startReading]) fail("AVAssetReader audio start", state.audio_reader.error);
            auto first = read_audio_sample(state.audio_output);
            if (!first) fail("Read first compressed audio sample", state.audio_reader.error);
            state.first_audio.emplace(first);
            auto format = CMSampleBufferGetFormatDescription(first);
            if (!format) throw std::runtime_error("First audio sample has no format description");
            CFRetain(format);
            state.audio_format.emplace(format);
            state.info.audio_format = format;
        }
#pragma clang diagnostic pop
    }});
}
ContainerInput::~ContainerInput() = default;
const MediaInfo& ContainerInput::info() const { return impl_->info; }
CMSampleBufferRef ContainerInput::read_video() { return impl_->read(impl_->video); }
CMSampleBufferRef ContainerInput::read_audio() {
    return native([&]() -> CMSampleBufferRef { @autoreleasepool {
        if (impl_->first_audio) {
            auto sample = impl_->first_audio->get();
            CFRetain(sample);
            impl_->first_audio.reset();
            return sample;
        }
        auto sample = read_audio_sample(impl_->audio_output);
        if (!sample && (impl_->audio_reader.status == AVAssetReaderStatusFailed || impl_->audio_reader.status == AVAssetReaderStatusCancelled)) {
            fail("Read compressed audio", impl_->audio_reader.error);
        }
        return sample; // Preserve AVFoundation's complete priming/trim attachments unchanged.
    }});
}

struct ContainerOutput::Impl {
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* video = nil;
    AVAssetWriterInput* audio = nil;
    std::string directory;
    std::string temporary;
    std::string destination;
    ~Impl() {
        if (writer.status == AVAssetWriterStatusWriting) [writer cancelWriting];
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }
    void check() const {
        if (writer.status == AVAssetWriterStatusFailed || writer.status == AVAssetWriterStatusCancelled) {
            fail("AVAssetWriter", writer.error);
        }
    }
};

ContainerOutput::ContainerOutput(const EncodeConfig& config, const MediaInfo& info, CMFormatDescriptionRef format)
    : impl_(std::make_unique<Impl>()) {
    native([&] { @autoreleasepool {
        auto& state = *impl_;
        state.destination = config.output;
        struct stat existing{};
        if (lstat(config.output.c_str(), &existing) == 0) throw std::runtime_error("Output already exists: " + config.output);
        if (errno != ENOENT) throw std::runtime_error("Inspect output: " + std::string(std::strerror(errno)));
        auto pattern = config.output + ".part.XXXXXX";
        if (!mkdtemp(pattern.data())) throw std::runtime_error("Create output directory: " + std::string(std::strerror(errno)));
        state.directory = std::move(pattern);
        state.temporary = state.directory + "/media";
        auto extension = std::filesystem::path(config.output).extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        NSError* error = nil;
        state.writer = [[AVAssetWriter alloc] initWithURL:url(state.temporary)
            fileType:extension == ".mov" ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4 error:&error];
        if (!state.writer) fail("AVAssetWriter init", error);
        state.video = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:nil sourceFormatHint:format];
        state.video.expectsMediaDataInRealTime = NO;
        state.video.mediaTimeScale = info.video_timescale;
        const auto& t = info.transform;
        const auto source_size = CMVideoFormatDescriptionGetDimensions(info.video_format);
        const auto output_size = CMVideoFormatDescriptionGetDimensions(format);
        // Keep the transformed bounding-box origin when the stored raster changes size.
        const auto min_x = [&](CMVideoDimensions size) { return std::min(0.0, t[0] * size.width) + std::min(0.0, t[2] * size.height); };
        const auto min_y = [&](CMVideoDimensions size) { return std::min(0.0, t[1] * size.width) + std::min(0.0, t[3] * size.height); };
        state.video.transform = CGAffineTransformMake(t[0], t[1], t[2], t[3],
            t[4] + min_x(source_size) - min_x(output_size), t[5] + min_y(source_size) - min_y(output_size));
        if (![state.writer canAddInput:state.video]) throw std::runtime_error("Container does not accept encoded video format");
        [state.writer addInput:state.video];
        auto movie_scale = static_cast<std::int64_t>(info.video_timescale);
        if (info.audio_format) {
            state.audio = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio outputSettings:nil sourceFormatHint:info.audio_format];
            state.audio.expectsMediaDataInRealTime = NO;
            if (![state.writer canAddInput:state.audio]) throw std::runtime_error("Container does not accept audio copy; use --no-audio");
            [state.writer addInput:state.audio];
            movie_scale = std::lcm(movie_scale, static_cast<std::int64_t>(info.audio_timescale));
        }
        if (movie_scale > std::numeric_limits<std::int32_t>::max()) {
            throw std::runtime_error("Track timescales cannot be represented exactly by the movie timescale");
        }
        state.writer.movieTimeScale = static_cast<std::int32_t>(movie_scale);
        if (![state.writer startWriting]) fail("AVAssetWriter startWriting", state.writer.error);
        [state.writer startSessionAtSourceTime:kCMTimeZero]; // Shared source origin, never per-track normalization.
    }});
}
ContainerOutput::~ContainerOutput() = default;
bool ContainerOutput::video_ready() const { return native([&] { impl_->check(); return static_cast<bool>(impl_->video.readyForMoreMediaData); }); }
bool ContainerOutput::audio_ready() const { return native([&] { impl_->check(); return static_cast<bool>(impl_->audio.readyForMoreMediaData); }); }
void ContainerOutput::append_video(CMSampleBufferRef sample) {
    native([&] { @autoreleasepool {
        if (![impl_->video appendSampleBuffer:sample]) fail("AVAssetWriter append video", impl_->writer.error);
    }});
}
void ContainerOutput::append_audio(CMSampleBufferRef sample) {
    native([&] { @autoreleasepool {
        if (![impl_->audio appendSampleBuffer:sample]) fail("AVAssetWriter append audio copy", impl_->writer.error);
    }});
}
void ContainerOutput::end_video() { native([&] { [impl_->video markAsFinished]; }); }
void ContainerOutput::end_audio() { native([&] { [impl_->audio markAsFinished]; }); }
std::uint64_t ContainerOutput::finish(const volatile std::sig_atomic_t& cancelled) {
    return native([&]() -> std::uint64_t { @autoreleasepool {
        auto done = dispatch_semaphore_create(0);
        [impl_->writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
            if (cancelled) { [impl_->writer cancelWriting]; return 0; }
            impl_->check();
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("AVAssetWriter finish timed out");
        }
        impl_->check();
        if (impl_->writer.status != AVAssetWriterStatusCompleted) throw std::runtime_error("AVAssetWriter did not complete");
        if (cancelled) return 0;
        const auto bytes = std::filesystem::file_size(impl_->temporary);
        if (!bytes) throw std::runtime_error("Container output is empty");
        if (link(impl_->temporary.c_str(), impl_->destination.c_str()) != 0) {
            throw std::runtime_error("Publish container [" + impl_->destination + "]: " + std::strerror(errno));
        }
        return bytes;
    }});
}

} // namespace vtenc
