#include "gpu_filter.hpp"
#include "ivtc.hpp"
#include "encoder_session.hpp"
#include "filter_shader.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CVMetalTextureCache.h>
#include <array>

namespace vtenc {
namespace {
[[noreturn]] void metal_error(const char* operation, NSError* error) {
    throw std::runtime_error(std::string(operation) + ": " + (error ? error.description.UTF8String : "Metal returned nil"));
}
CFRef<CVMetalTextureRef> texture(CVMetalTextureCacheRef cache, CVPixelBufferRef pixel, std::size_t plane, bool write) {
    const auto type = CVPixelBufferGetPixelFormatType(pixel);
    const bool ten = type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    const auto format = ten ? (plane ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
                            : (plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
    NSDictionary* attributes = @{(__bridge NSString*)kCVMetalTextureUsage: @(write ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead)};
    CVMetalTextureRef raw = nullptr;
    check_status(CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pixel,
        (__bridge CFDictionaryRef)attributes, format, CVPixelBufferGetWidthOfPlane(pixel, plane),
        CVPixelBufferGetHeightOfPlane(pixel, plane), plane, &raw), "Map IOSurface plane to Metal texture");
    CFRef result(raw);
    if (!CVMetalTextureGetTexture(raw)) throw std::runtime_error("CoreVideo returned no Metal plane texture");
    return result;
}
}
struct GpuFilter::Impl {
    FilterPlan plan;
    CFRef<CVPixelBufferPoolRef> pool;
    std::optional<CFRef<CVMetalTextureCacheRef>> cache;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    explicit Impl(FilterPlan value) : plan(std::move(value)), pool(make_filter_pool(plan)) {}
};
GpuFilter::GpuFilter(FilterPlan plan) : impl_(std::make_unique<Impl>(std::move(plan))) {
    @autoreleasepool {
        auto& s = *impl_;
        s.device = MTLCreateSystemDefaultDevice();
        if (!s.device) metal_error("Create Metal device", nil);
        s.queue = [s.device newCommandQueue];
        if (!s.queue) metal_error("Create Metal command queue", nil);
        NSError* error = nil;
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion2_4;
        options.fastMathEnabled = NO;
        auto library = [s.device newLibraryWithSource:[NSString stringWithUTF8String:filter_shader_source] options:options error:&error];
        if (!library) metal_error("Compile Metal filter", error);
        auto function = [library newFunctionWithName:@"filter_frame"];
        if (!function) metal_error("Load Metal filter kernel", nil);
        s.pipeline = [s.device newComputePipelineStateWithFunction:function error:&error];
        if (!s.pipeline) metal_error("Create Metal filter pipeline", error);
        CVMetalTextureCacheRef raw = nullptr;
        check_status(CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, s.device, nullptr, &raw), "Create Metal texture cache");
        s.cache.emplace(raw);
    }
}
GpuFilter::~GpuFilter() = default;
CFRef<CVPixelBufferRef> GpuFilter::process(CVPixelBufferRef source, int field) {
    @autoreleasepool {
        auto& s = *impl_;
        validate_filter_frame(s.plan, source);
        if ((s.plan.parameters.bob && (field < 0 || field > 1)) || (!s.plan.parameters.bob && field != -1)) {
            throw std::runtime_error("GPU Bob needs spatial field parity 0 or 1; progressive processing needs -1");
        }
        auto parameters = s.plan.parameters;
        if (parameters.bob) {
            parameters.field = static_cast<unsigned>(field);
            if (field == 1) { parameters.chroma_x = s.plan.bottom_chroma_x; parameters.chroma_y = s.plan.bottom_chroma_y; }
        }
        auto output = make_filter_buffer(s.pool.get(), s.plan);
        // Texture wrappers and both pixel buffers remain alive until command completion.
        std::array textures{texture(s.cache->get(), source, 0, false), texture(s.cache->get(), source, 1, false),
                            texture(s.cache->get(), output.get(), 0, true), texture(s.cache->get(), output.get(), 1, true)};
        auto command = [s.queue commandBuffer];
        if (!command) metal_error("Create Metal command buffer", nil);
        command.label = @"VTEnc Bob / crop / bilinear scale / colorspace";
        auto encoder = [command computeCommandEncoder];
        if (!encoder) metal_error("Create Metal compute encoder", nil);
        [encoder setComputePipelineState:s.pipeline];
        for (std::size_t index = 0; index < textures.size(); ++index) [encoder setTexture:CVMetalTextureGetTexture(textures[index].get()) atIndex:index];
        [encoder setBytes:&parameters length:sizeof(FilterParameters) atIndex:0];
        const auto x = s.pipeline.threadExecutionWidth;
        const auto y = std::min<NSUInteger>(8, s.pipeline.maxTotalThreadsPerThreadgroup / x);
        [encoder dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(s.plan.width), static_cast<NSUInteger>(s.plan.height), 1)
            threadsPerThreadgroup:MTLSizeMake(x, y, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) metal_error("Execute Metal filter", command.error);
        return output;
    }
}

struct GpuIvtc::Impl {
    struct Parameters { unsigned first, match, repair, nt, threshold, combpel, dthresh, depth; };
    Parameters parameters{};
    int width, height;
    OSType format;
    CFRef<CVPixelBufferPoolRef> pool;
    std::optional<CFRef<CFDictionaryRef>> metadata;
    std::optional<CFRef<CVMetalTextureCacheRef>> cache;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> score, synthesize, difference;
    id<MTLBuffer> metrics;
    std::size_t groups;
    Impl(CVPixelBufferRef source, IvtcConfig config)
        : width(static_cast<int>(CVPixelBufferGetWidth(source))), height(static_cast<int>(CVPixelBufferGetHeight(source))),
          format(CVPixelBufferGetPixelFormatType(source)), pool(make_metal_pixel_pool(width, height, format)) {
        if (width <= 0 || width % 2 || height < 8 || height % 4 || CVPixelBufferGetPlaneCount(source) != 2)
            throw std::runtime_error("IVTC requires even width and height >= 8 divisible by four (interlaced 4:2:0)");
        switch (format) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: parameters.depth = 8; break;
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: parameters.depth = 10; break;
        default: throw std::runtime_error("Metal IVTC requires NV12/P010; pixel format=" + std::to_string(format));
        }
        const auto maximum = (1U << parameters.depth) - 1;
        parameters.nt = std::max(1U, maximum * static_cast<unsigned>(config.nt) / 255);
        parameters.threshold = std::max(1U, maximum * static_cast<unsigned>(config.cthresh) / 255);
        parameters.combpel = static_cast<unsigned>(config.combpel);
        parameters.dthresh = static_cast<unsigned>(config.dthresh) << (parameters.depth - 8);
        if (auto raw = CVBufferCopyAttachments(source, kCVAttachmentMode_ShouldPropagate)) metadata.emplace(raw);
        groups = static_cast<std::size_t>((width + 31) / 32) * static_cast<std::size_t>((height + 7) / 8);
    }
    void validate(CVPixelBufferRef pixel) const {
        if (CVPixelBufferGetWidth(pixel) != static_cast<std::size_t>(width) ||
            CVPixelBufferGetHeight(pixel) != static_cast<std::size_t>(height) ||
            CVPixelBufferGetPixelFormatType(pixel) != format || CVPixelBufferGetPlaneCount(pixel) != 2)
            throw std::runtime_error("Decoded pixel layout changed during Metal IVTC");
        // Field flags may change in hard telecine. Color/geometry may not.
        for (const auto key : {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey,
             kCVImageBufferYCbCrMatrixKey, kCVImageBufferChromaLocationTopFieldKey,
             kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferPixelAspectRatioKey, kCVImageBufferCleanApertureKey}) {
            const auto expected = metadata ? CFDictionaryGetValue(metadata->get(), key) : nullptr;
            auto raw = CVBufferCopyAttachment(pixel, key, nullptr);
            if (raw) {
                CFRef actual(raw);
                if (!expected || !CFEqual(expected, actual.get())) throw std::runtime_error("Decoded color/geometry metadata changed during IVTC");
            } else if (expected) throw std::runtime_error("Decoded color/geometry metadata disappeared during IVTC");
        }
    }
    void dispatch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline,
                  const Parameters& params, std::size_t w, std::size_t h) {
        if (!encoder) metal_error("Create IVTC compute encoder", nil);
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:&params length:sizeof(params) atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake((w + 31) / 32, (h + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
        [encoder endEncoding];
    }
    void finish(id<MTLCommandBuffer> command) {
        [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) metal_error("Execute Metal IVTC", command.error);
    }
};
GpuIvtc::GpuIvtc(CVPixelBufferRef source, IvtcConfig config) : impl_(std::make_unique<Impl>(source, config)) {
    @autoreleasepool {
        auto& s = *impl_;
        s.device = MTLCreateSystemDefaultDevice();
        if (!s.device) metal_error("Create IVTC Metal device", nil);
        s.queue = [s.device newCommandQueue];
        if (!s.queue) metal_error("Create IVTC command queue", nil);
        NSError* error = nil;
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion2_4; options.fastMathEnabled = NO;
        auto library = [s.device newLibraryWithSource:[NSString stringWithUTF8String:filter_shader_source] options:options error:&error];
        if (!library) metal_error("Compile Metal IVTC", error);
        const auto pipeline = [&](NSString* name) {
            auto function = [library newFunctionWithName:name];
            if (!function) metal_error("Load IVTC kernel", nil);
            auto result = [s.device newComputePipelineStateWithFunction:function error:&error];
            if (!result) metal_error("Create IVTC pipeline", error);
            if (result.maxTotalThreadsPerThreadgroup < 256) throw std::runtime_error("Metal IVTC needs 256 threads per group");
            return result;
        };
        s.score = pipeline(@"ivtc_score"); s.synthesize = pipeline(@"ivtc_synthesize"); s.difference = pipeline(@"ivtc_diff");
        s.metrics = [s.device newBufferWithLength:s.groups * 9 * sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
        if (!s.metrics) metal_error("Allocate IVTC block metrics", nil);
        CVMetalTextureCacheRef raw = nullptr;
        check_status(CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, s.device, nullptr, &raw), "Create IVTC texture cache");
        s.cache.emplace(raw);
    }
}
GpuIvtc::~GpuIvtc() = default;
IvtcScores GpuIvtc::score(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef next, unsigned first) {
    @autoreleasepool {
        auto& s = *impl_;
        if (first > 1) throw std::runtime_error("IVTC field parity must be 0 or 1");
        for (auto frame : {prev, cur, next}) s.validate(frame);
        std::array textures{texture(s.cache->get(), prev, 0, false), texture(s.cache->get(), cur, 0, false), texture(s.cache->get(), next, 0, false)};
        auto command = [s.queue commandBuffer];
        if (!command) metal_error("Create IVTC score command", nil);
        auto encoder = [command computeCommandEncoder];
        if (!encoder) metal_error("Create IVTC score encoder", nil);
        for (std::size_t i = 0; i < textures.size(); ++i) [encoder setTexture:CVMetalTextureGetTexture(textures[i].get()) atIndex:i];
        [encoder setBuffer:s.metrics offset:0 atIndex:1];
        auto p = s.parameters; p.first = first;
        s.dispatch(encoder, s.score, p, static_cast<std::size_t>(s.width), static_cast<std::size_t>(s.height));
        s.finish(command);
        // Upstream's clipped block means, maximum and combed-block count.
        const auto data = static_cast<const std::uint32_t*>(s.metrics.contents);
        const auto clip = std::max(4U, p.nt * 4);
        std::array<std::uint64_t, 6> sum{}, count{};
        IvtcScores result{};
        for (std::size_t g = 0; g < s.groups; ++g) {
            const auto e = data + g * 9;
            for (unsigned k = 0; k < 6; ++k) if (e[k] > clip) { sum[k] += e[k]; ++count[k]; }
            for (unsigned k = 0; k < 3; ++k) {
                result[k].maximum = std::max(result[k].maximum, static_cast<std::uint64_t>(e[k + 3]));
                result[k].blocks += e[k + 6];
            }
        }
        for (unsigned k = 0; k < 3; ++k) {
            result[k].match = count[k] >= 2 ? sum[k] / count[k] : 0;
            result[k].comb = count[k + 3] >= 2 ? sum[k + 3] / count[k + 3] : 0;
        }
        return result;
    }
}
std::uint64_t GpuIvtc::difference(CVPixelBufferRef a, CVPixelBufferRef b) {
    @autoreleasepool {
        auto& s = *impl_;
        // Matched frames have progressive attachments; validate the storage here.
        for (auto frame : {a, b}) s.validate(frame);
        std::array textures{texture(s.cache->get(), a, 0, false), texture(s.cache->get(), b, 0, false)};
        auto command = [s.queue commandBuffer];
        if (!command) metal_error("Create IVTC SAD command", nil);
        auto encoder = [command computeCommandEncoder];
        if (!encoder) metal_error("Create IVTC SAD encoder", nil);
        for (std::size_t i = 0; i < textures.size(); ++i) [encoder setTexture:CVMetalTextureGetTexture(textures[i].get()) atIndex:i];
        [encoder setBuffer:s.metrics offset:0 atIndex:1];
        s.dispatch(encoder, s.difference, s.parameters, static_cast<std::size_t>(s.width), static_cast<std::size_t>(s.height));
        s.finish(command);
        const auto data = static_cast<const std::uint32_t*>(s.metrics.contents);
        std::uint64_t result = 0;
        for (std::size_t g = 0; g < s.groups; ++g) result += data[g];
        return result;
    }
}
CFRef<CVPixelBufferRef> GpuIvtc::synthesize(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef next,
                                         unsigned first, unsigned match, bool repair) {
    @autoreleasepool {
        auto& s = *impl_;
        if (first > 1 || match > 2) throw std::runtime_error("Invalid IVTC field parity or match");
        for (auto frame : {prev, cur, next}) s.validate(frame);
        const int limit = 10;
        CFRef number(CFNumberCreate(nullptr, kCFNumberIntType, &limit));
        const void* keys[] = {kCVPixelBufferPoolAllocationThresholdKey}; const void* values[] = {number.get()};
        CFRef options(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CVPixelBufferRef raw = nullptr;
        check_status(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, s.pool.get(), options.get(), &raw), "Allocate bounded IVTC output");
        CFRef output(raw);
        CVBufferRemoveAllAttachments(raw);
        if (s.metadata) CVBufferSetAttachments(raw, s.metadata->get(), kCVAttachmentMode_ShouldPropagate);
        CVBufferRemoveAttachment(raw, kCVImageBufferFieldDetailKey);
        const int count = 1;
        CFRef fields(CFNumberCreate(nullptr, kCFNumberIntType, &count));
        CVBufferSetAttachment(raw, kCVImageBufferFieldCountKey, fields.get(), kCVAttachmentMode_ShouldPropagate);
        // Keep every plane wrapper alive until GPU completion.
        std::array textures{texture(s.cache->get(), prev, 0, false), texture(s.cache->get(), cur, 0, false),
            texture(s.cache->get(), next, 0, false), texture(s.cache->get(), raw, 0, true),
            texture(s.cache->get(), prev, 1, false), texture(s.cache->get(), cur, 1, false),
            texture(s.cache->get(), next, 1, false), texture(s.cache->get(), raw, 1, true)};
        auto command = [s.queue commandBuffer];
        if (!command) metal_error("Create IVTC synthesis command", nil);
        auto p = s.parameters; p.first = first; p.match = match; p.repair = repair;
        for (unsigned plane = 0; plane < 2; ++plane) {
            auto encoder = [command computeCommandEncoder];
            if (!encoder) metal_error("Create IVTC synthesis encoder", nil);
            for (unsigned i = 0; i < 4; ++i) [encoder setTexture:CVMetalTextureGetTexture(textures[plane * 4 + i].get()) atIndex:i];
            s.dispatch(encoder, s.synthesize, p, CVPixelBufferGetWidthOfPlane(raw, plane), CVPixelBufferGetHeightOfPlane(raw, plane));
        }
        s.finish(command);
        return output;
    }
}
} // namespace vtenc
