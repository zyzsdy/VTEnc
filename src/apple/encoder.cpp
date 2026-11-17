#include "vtenc/encoder.hpp"
#include "encoder_session.hpp"
#include "bitstream_output.hpp"
#include "gpu_filter.hpp"
#include "bob.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace vtenc {
namespace {

// Submit at most this many frames, complete that batch, then consume on the caller thread.
// The callback never blocks on I/O or queue capacity; outstanding frames bound both
// encoder input retention and retained compressed samples, even with delayed output.
constexpr std::size_t batch_size = 8;
class OutputQueue {
public:
    ~OutputQueue() { for (auto sample : samples_) if (sample) CFRelease(sample); }
    static void callback(void* context, void*, OSStatus status, VTEncodeInfoFlags flags,
                         CMSampleBufferRef sample) noexcept {
        auto& queue = *static_cast<OutputQueue*>(context);
        try {
            std::lock_guard lock(queue.mutex_);
            if (queue.error_ != 0) return;
            if (status != noErr) { queue.status_ = status; queue.error_ = 1; return; }
            if (flags & kVTEncodeInfo_FrameDropped) { queue.error_ = 2; return; }
            if (!sample) { queue.error_ = 3; return; }
            if (queue.size_ == batch_size) { queue.error_ = 4; return; }
            CFRetain(sample);
            queue.samples_[queue.size_++] = sample;
        } catch (...) {
            queue.error_ = 5; // No exception may cross the C callback boundary.
        }
    }
    void check() {
        const auto error = error_.load();
        switch (error) {
        case 0: return;
        case 1: check_status(status_.load(), "VideoToolbox compression callback"); break;
        case 2: throw std::runtime_error("VideoToolbox dropped a submitted frame");
        case 3: throw std::runtime_error("VideoToolbox callback returned no sample");
        case 4: throw std::runtime_error("VideoToolbox callback exceeded the bounded output queue");
        default: throw std::runtime_error("Exception inside VideoToolbox callback");
        }
    }
    void drain(BitstreamOutput& output) {
        // CompleteFrames guarantees all pending callbacks have returned.
        check();
        for (std::size_t i = 0; i < size_; ++i) {
            CFRef sample(std::exchange(samples_[i], nullptr));
            output.append(sample.get());
        }
        size_ = 0;
    }
private:
    std::mutex mutex_;
    std::array<CMSampleBufferRef, batch_size> samples_{};
    std::size_t size_ = 0;
    std::atomic<int> error_{0};
    std::atomic<OSStatus> status_{noErr};
};

class RawSource {
public:
    explicit RawSource(const EncodeConfig& config) {
        if (config.input.empty()) return;
        // O_NONBLOCK lets us reject FIFOs instead of waiting forever for a writer.
        const int fd = open(config.input.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) throw std::runtime_error("Open raw input [" + config.input + "]: " + std::strerror(errno));
        file_.reset(fdopen(fd, "rb"));
        if (!file_) {
            const auto error = errno;
            ::close(fd);
            throw std::runtime_error("fdopen raw input [" + config.input + "]: " + std::strerror(error));
        }
        struct stat info{};
        if (fstat(fileno(file_.get()), &info) != 0) {
            throw std::runtime_error("Stat raw input [" + config.input + "]: " + std::strerror(errno));
        }
        if (!S_ISREG(info.st_mode)) throw std::runtime_error("Raw input must be a regular file: " + config.input);
        const auto frame_bytes = static_cast<std::uint64_t>(config.video.width) *
            static_cast<std::uint64_t>(config.video.height) * 3U /
            (config.video.bit_depth == 10 ? 1U : 2U);
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) / frame_bytes <
            static_cast<std::uint64_t>(config.frames)) {
            throw std::runtime_error("Raw input is too short for --frames: " + config.input +
                " (bytes/frame=" + std::to_string(frame_bytes) + ", frames=" + std::to_string(config.frames) + ")");
        }
    }
    void fill(CVPixelBufferRef pixel, const EncodeConfig& config, std::int64_t frame) {
        const auto width = static_cast<std::size_t>(config.video.width);
        const auto height = static_cast<std::size_t>(config.video.height);
        const bool ten_bit = config.video.bit_depth == 10;
        const auto expected_format = ten_bit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        if (CVPixelBufferGetPixelFormatType(pixel) != expected_format ||
            CVPixelBufferGetWidth(pixel) != width || CVPixelBufferGetHeight(pixel) != height ||
            CVPixelBufferGetPlaneCount(pixel) != 2) {
            throw std::runtime_error("Encoder pixel pool changed the requested NV12/P010 layout");
        }
        check_status(CVPixelBufferLockBaseAddress(pixel, 0), "CVPixelBufferLockBaseAddress");
        try {
            for (std::size_t plane = 0; plane < 2; ++plane) {
                auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(pixel, plane));
                const auto stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
                const auto rows = plane == 0 ? height : height / 2;
                const auto row_bytes = width * (ten_bit ? 2U : 1U);
                if (!base || stride < row_bytes || CVPixelBufferGetHeightOfPlane(pixel, plane) != rows) {
                    throw std::runtime_error("Invalid encoder pixel buffer plane layout");
                }
                for (std::size_t y = 0; y < rows; ++y) {
                    auto* row = base + y * stride;
                    if (file_) {
                        const auto read = std::fread(row, 1, row_bytes, file_.get());
                        if (read != row_bytes) {
                            throw std::runtime_error("Read raw input failed at frame=" + std::to_string(frame) +
                                ", plane=" + std::to_string(plane) + ", row=" + std::to_string(y) +
                                (std::ferror(file_.get()) ? ": " + std::string(std::strerror(errno)) : ": unexpected EOF"));
                        }
                    } else {
                        for (std::size_t x = 0; x < width; ++x) {
                            const auto phase = (x + static_cast<std::size_t>(frame) * 3) % width;
                            const auto value = plane == 0
                                ? (ten_bit ? 64U : 16U) + phase * (ten_bit ? 876U : 219U) / (width - 1)
                                : (ten_bit ? 512U : 128U);
                            if (ten_bit) {
                                const auto word = static_cast<std::uint16_t>(value << 6);
                                std::memcpy(row + x * 2, &word, sizeof(word));
                            } else row[x] = static_cast<unsigned char>(value);
                        }
                    }
                }
            }
        } catch (...) {
            CVPixelBufferUnlockBaseAddress(pixel, 0);
            throw;
        }
        check_status(CVPixelBufferUnlockBaseAddress(pixel, 0), "CVPixelBufferUnlockBaseAddress");
        CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
            kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
            kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
            kCVAttachmentMode_ShouldPropagate);
    }
private:
    struct Closer { void operator()(FILE* file) const noexcept { std::fclose(file); } };
    std::unique_ptr<FILE, Closer> file_;
};


EncodeResult encode_raw_bob(const EncodeConfig& config, RawSource& source, const volatile std::sig_atomic_t& cancelled) {
    const auto output_config = raw_bob_output_config(config);
    BitstreamOutput output(output_config);
    OutputQueue queue;
    CompressionSession session;
    create_hardware_session(session, output_config.video, &OutputQueue::callback, &queue);
    configure_hardware_encoder(session.value, output_config);
    check_status(VTCompressionSessionPrepareToEncodeFrames(session.value), "Prepare raw Bob hardware encoder");
    verify_hardware(session.value);
    const auto type = config.video.bit_depth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    auto pool = make_metal_pixel_pool(config.video.width, config.video.height, type);
    std::unique_ptr<GpuFilter> filter;
    std::int64_t submitted = 0;
    const unsigned first = config.filter.field_order == FieldOrder::tff ? 0U : 1U;
    for (std::int64_t index = 0; index < config.frames && !cancelled; ++index) {
        CVPixelBufferRef raw = nullptr;
        check_status(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool.get(), &raw), "Allocate raw Bob source");
        CFRef pixel(raw);
        source.fill(pixel.get(), config, index);
        CVBufferSetAttachment(raw, kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocation_Left, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(raw, kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferChromaLocation_Left, kCVAttachmentMode_ShouldPropagate);
        if (!filter) {
            CMVideoFormatDescriptionRef description = nullptr;
            check_status(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, raw, &description), "Describe raw Bob source");
            CFRef format(description);
            filter = std::make_unique<GpuFilter>(make_filter_plan(config, format.get(), raw));
        }
        for (unsigned field = 0; field < 2 && !cancelled; ++field) {
            auto processed = filter->process(raw, static_cast<int>(first ^ field));
            const auto timing = bob_time(CMTimeMake(index * config.fps_den, config.fps_num), CMTimeMake(config.fps_den, config.fps_num), field);
            VTEncodeInfoFlags flags = 0;
            check_status(VTCompressionSessionEncodeFrame(session.value, processed.get(), timing.pts, timing.duration,
                nullptr, nullptr, &flags), "Encode raw Bob field=" + std::to_string(field));
            if (flags & kVTEncodeInfo_FrameDropped) throw std::runtime_error("Hardware encoder dropped a Bob field");
            ++submitted;
            check_status(VTCompressionSessionCompleteFrames(session.value, kCMTimeInvalid), "Drain raw Bob field");
            queue.drain(output);
            if (output.frames() != submitted) throw std::runtime_error("Missing encoded Bob field");
        }
    }
    verify_hardware(session.value);
    if (cancelled) return {output.frames(), output.bytes(), true, 0, submitted};
    if (submitted != output_config.frames) throw std::runtime_error("Wrong Bob output count");
    output.publish();
    return {output.frames(), output.bytes(), false, 0, submitted};
}


} // namespace

EncodeResult encode_video(const EncodeConfig& config, const volatile std::sig_atomic_t& cancelled) {
    if (config.frames <= 0 || config.fps_num <= 0 || config.fps_den <= 0 || config.gop <= 0 ||
        config.bitrate <= 0 || config.output.empty() || config.video.width <= 0 || config.video.height <= 0 ||
        config.video.width % 2 || config.video.height % 2 ||
        (config.quality && (!std::isfinite(*config.quality) || *config.quality < 0 || *config.quality > 1))) {
        throw std::invalid_argument("Invalid encoding configuration");
    }
    RawSource source(config);
    if (config.filter.bob) return encode_raw_bob(config, source, cancelled);
    BitstreamOutput output(config);
    OutputQueue queue; // Must outlive the session, including error/early-return teardown.
    CompressionSession session;
    create_hardware_session(session, config.video, &OutputQueue::callback, &queue);
    configure_hardware_encoder(session.value, config);
    check_status(VTCompressionSessionPrepareToEncodeFrames(session.value), "VTCompressionSessionPrepareToEncodeFrames");
    verify_hardware(session.value);
    std::int64_t submitted = 0;
    while (submitted < config.frames && !cancelled) {
        for (std::size_t i = 0; i < batch_size && submitted < config.frames && !cancelled; ++i) {
            queue.check();
            auto pool = VTCompressionSessionGetPixelBufferPool(session.value);
            if (!pool) throw std::runtime_error("VideoToolbox returned no input pixel buffer pool");
            CVPixelBufferRef raw = nullptr;
            check_status(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &raw),
                "CVPixelBufferPoolCreatePixelBuffer [frame=" + std::to_string(submitted) + "]");
            CFRef pixel(raw);
            source.fill(pixel.get(), config, submitted);
            VTEncodeInfoFlags flags = 0;
            check_status(VTCompressionSessionEncodeFrame(session.value, pixel.get(),
                CMTimeMake(submitted * config.fps_den, config.fps_num), CMTimeMake(config.fps_den, config.fps_num),
                nullptr, nullptr, &flags), "VTCompressionSessionEncodeFrame [frame=" + std::to_string(submitted) + "]");
            if (flags & kVTEncodeInfo_FrameDropped) throw std::runtime_error("VideoToolbox dropped input frame=" + std::to_string(submitted));
            ++submitted;
        }
        check_status(VTCompressionSessionCompleteFrames(session.value, kCMTimeInvalid), "VTCompressionSessionCompleteFrames [batch]");
        queue.drain(output);
        if (output.frames() != submitted) throw std::runtime_error("Encoder did not output every submitted frame after draining");
    }
    check_status(VTCompressionSessionCompleteFrames(session.value, kCMTimeInvalid), "VTCompressionSessionCompleteFrames [final]");
    queue.drain(output);
    verify_hardware(session.value); // Verify again after actual frame encoding.
    if (cancelled) return {output.frames(), output.bytes(), true};
    if (output.frames() != config.frames) throw std::runtime_error("Encoded frame count differs from requested count");
    output.publish();
    return {output.frames(), output.bytes(), false};
}

} // namespace vtenc
