#include "gpu_filter.hpp"
#include "subtitle.hpp"
#include "output_metadata.hpp"
#include "vtenc/colorspace.hpp"
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
             kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferPixelAspectRatioKey, kCVImageBufferCleanApertureKey,
             kCMFormatDescriptionExtension_MasteringDisplayColorVolume, kCMFormatDescriptionExtension_ContentLightLevelInfo, source_master_key(), source_light_key()}) {
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

struct GpuFieldPair::Impl {
    int width, height;
    OSType format;
    unsigned depth;
    CFRef<CVPixelBufferPoolRef> pool;
    std::optional<CFRef<CFDictionaryRef>> metadata;
    std::optional<CFRef<CVMetalTextureCacheRef>> cache;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    explicit Impl(CVPixelBufferRef source)
        : width(static_cast<int>(CVPixelBufferGetWidth(source))), height(static_cast<int>(CVPixelBufferGetHeight(source))),
          format(CVPixelBufferGetPixelFormatType(source)), pool(make_metal_pixel_pool(width, height * 2, format)) {
        if (width <= 0 || width % 2 || height < 4 || height % 2 || CVPixelBufferGetPlaneCount(source) != 2)
            throw std::runtime_error("HEVC field pairing requires even NV12/P010 field dimensions and height >= 4");
        switch (format) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: depth = 8; break;
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: depth = 10; break;
        default: throw std::runtime_error("HEVC field pairing requires NV12/P010");
        }
        if (auto raw = CVBufferCopyAttachments(source, kCVAttachmentMode_ShouldPropagate)) metadata.emplace(raw);
    }
    void validate(CVPixelBufferRef pixel) const {
        if (CVPixelBufferGetWidth(pixel) != static_cast<std::size_t>(width) ||
            CVPixelBufferGetHeight(pixel) != static_cast<std::size_t>(height) ||
            CVPixelBufferGetPixelFormatType(pixel) != format || CVPixelBufferGetPlaneCount(pixel) != 2)
            throw std::runtime_error("HEVC single-field pixel layout changed during pairing");
        for (const auto key : {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey,
             kCVImageBufferYCbCrMatrixKey, kCVImageBufferChromaLocationTopFieldKey,
             kCVImageBufferChromaLocationBottomFieldKey, kCVImageBufferPixelAspectRatioKey, kCVImageBufferCleanApertureKey,
             kCMFormatDescriptionExtension_MasteringDisplayColorVolume, kCMFormatDescriptionExtension_ContentLightLevelInfo, source_master_key(), source_light_key()}) {
            const auto expected = metadata ? CFDictionaryGetValue(metadata->get(), key) : nullptr;
            auto raw = CVBufferCopyAttachment(pixel, key, nullptr);
            if (raw) {
                CFRef actual(raw);
                if (!expected || !CFEqual(expected, actual.get())) throw std::runtime_error("HEVC field color/geometry changed during pairing");
            } else if (expected) throw std::runtime_error("HEVC field color/geometry disappeared during pairing");
        }
    }
};
GpuFieldPair::GpuFieldPair(CVPixelBufferRef source) : impl_(std::make_unique<Impl>(source)) {
    @autoreleasepool {
        auto& s = *impl_;
        s.device = MTLCreateSystemDefaultDevice();
        if (!s.device) metal_error("Create HEVC field Metal device", nil);
        s.queue = [s.device newCommandQueue];
        if (!s.queue) metal_error("Create HEVC field command queue", nil);
        NSError* error = nil;
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion2_4; options.fastMathEnabled = NO;
        auto library = [s.device newLibraryWithSource:[NSString stringWithUTF8String:filter_shader_source] options:options error:&error];
        if (!library) metal_error("Compile HEVC field shader", error);
        auto function = [library newFunctionWithName:@"hevc_field_pair"];
        if (!function) metal_error("Load HEVC field kernel", nil);
        s.pipeline = [s.device newComputePipelineStateWithFunction:function error:&error];
        if (!s.pipeline) metal_error("Create HEVC field pipeline", error);
        CVMetalTextureCacheRef raw = nullptr;
        check_status(CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, s.device, nullptr, &raw), "Create HEVC field texture cache");
        s.cache.emplace(raw);
    }
}
GpuFieldPair::~GpuFieldPair() = default;
CFRef<CVPixelBufferRef> GpuFieldPair::process(CVPixelBufferRef first, CVPixelBufferRef second, unsigned parity) {
    @autoreleasepool {
        auto& s = *impl_;
        if (parity > 1) throw std::runtime_error("Invalid HEVC spatial field parity");
        s.validate(first); if (second) s.validate(second);
        const int limit = 10;
        CFRef number(CFNumberCreate(nullptr, kCFNumberIntType, &limit));
        const void* keys[] = {kCVPixelBufferPoolAllocationThresholdKey}; const void* values[] = {number.get()};
        CFRef options(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CVPixelBufferRef raw = nullptr;
        check_status(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, s.pool.get(), options.get(), &raw), "Allocate bounded HEVC field pair");
        CFRef output(raw);
        CVBufferRemoveAllAttachments(raw);
        if (s.metadata) CVBufferSetAttachments(raw, s.metadata->get(), kCVAttachmentMode_ShouldPropagate);
        // H.265 E.3.1 SAR already describes the complete frame. Aperture
        // coordinates, however, describe the decoded half-height picture.
        if (auto aperture = CVBufferCopyAttachment(first, kCVImageBufferCleanApertureKey, nullptr)) {
            CFRef original(aperture);
            if (CFGetTypeID(aperture) != CFDictionaryGetTypeID()) throw std::runtime_error("Invalid HEVC field clean aperture");
            CFRef adjusted(CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(aperture)));
            for (const auto key : {kCVImageBufferCleanApertureHeightKey, kCVImageBufferCleanApertureVerticalOffsetKey}) {
                auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(adjusted.get(), key));
                double n = 0;
                if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !CFNumberGetValue(value, kCFNumberDoubleType, &n))
                    throw std::runtime_error("Invalid HEVC field aperture coordinate");
                n *= 2;
                CFRef scaled(CFNumberCreate(nullptr, kCFNumberDoubleType, &n));
                CFDictionarySetValue(adjusted.get(), key, scaled.get());
            }
            CVBufferSetAttachment(raw, kCVImageBufferCleanApertureKey, adjusted.get(), kCVAttachmentMode_ShouldPropagate);
        }
        const int count = second ? 2 : 1;
        CFRef fields(CFNumberCreate(nullptr, kCFNumberIntType, &count));
        CVBufferSetAttachment(raw, kCVImageBufferFieldCountKey, fields.get(), kCVAttachmentMode_ShouldPropagate);
        CVBufferRemoveAttachment(raw, kCVImageBufferFieldDetailKey);
        if (second) CVBufferSetAttachment(raw, kCVImageBufferFieldDetailKey, parity ? kCVImageBufferFieldDetailSpatialFirstLineLate : kCVImageBufferFieldDetailSpatialFirstLineEarly, kCVAttachmentMode_ShouldPropagate);
        const std::array<unsigned, 3> params{parity, second ? 0U : 1U, s.depth};
        std::array textures{texture(s.cache->get(), first, 0, false), texture(s.cache->get(), second ? second : first, 0, false), texture(s.cache->get(), raw, 0, true),
            texture(s.cache->get(), first, 1, false), texture(s.cache->get(), second ? second : first, 1, false), texture(s.cache->get(), raw, 1, true)};
        auto command = [s.queue commandBuffer];
        if (!command) metal_error("Create HEVC field command", nil);
        for (unsigned plane = 0; plane < 2; ++plane) {
            auto encoder = [command computeCommandEncoder];
            if (!encoder) metal_error("Create HEVC field encoder", nil);
            [encoder setComputePipelineState:s.pipeline];
            [encoder setBytes:params.data() length:sizeof(params) atIndex:0];
            for (unsigned i = 0; i < 3; ++i) [encoder setTexture:CVMetalTextureGetTexture(textures[plane * 3 + i].get()) atIndex:i];
            const auto x = s.pipeline.threadExecutionWidth;
            const auto y = std::min<NSUInteger>(8, s.pipeline.maxTotalThreadsPerThreadgroup / x);
            [encoder dispatchThreads:MTLSizeMake(CVPixelBufferGetWidthOfPlane(raw, plane), CVPixelBufferGetHeightOfPlane(raw, plane), 1) threadsPerThreadgroup:MTLSizeMake(x, y, 1)];
            [encoder endEncoding];
        }
        [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) metal_error("Execute HEVC field pairing", command.error);
        return output;
    }
}
struct GpuSubtitles::Impl {
    SubtitleParameters parameters{};
    FilterPlan plan;
    CFRef<CVPixelBufferPoolRef> pool;
    std::optional<CFRef<CVMetalTextureCacheRef>> cache;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> clear, glyph, blend;
    id<MTLTexture> overlay;
    bool initialized=false;
    static FilterPlan describe(CVPixelBufferRef source) {
        CMVideoFormatDescriptionRef raw=nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,source,&raw),"Describe subtitle canvas");
        CFRef format(raw);
        EncodeConfig c;
        const auto type=CVPixelBufferGetPixelFormatType(source);
        const bool ten=type==kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange||type==kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        c.video.bit_depth=ten?10:8;
        c.filter.colorspace.enabled=true;
        return make_filter_plan(c,raw,source);
    }
    explicit Impl(CVPixelBufferRef source):plan(describe(source)),pool(make_filter_pool(plan)) {
        const auto& f=plan.parameters;
        auto& p=parameters;
        p.storage=f.input_storage; p.y_offset=f.input_y_offset; p.y_scale=f.input_y_scale;
        p.c_offset=f.input_c_offset; p.c_scale=f.input_c_scale; p.output_storage=f.output_storage; p.maximum=f.output_max;
        p.kr=f.input_kr; p.kb=f.input_kb; p.transfer=f.input_transfer;
        if (p.kr<=0 || p.kb<=0 || p.transfer==2) throw std::runtime_error("Subtitles require known matrix/transfer; set source interpretation with --vpp-colorspace");
        const auto v=CFDictionaryGetValue(plan.attachments.get(),kCVImageBufferColorPrimariesKey);
        if (!v) throw std::runtime_error("Subtitles require known primaries; use --vpp-colorspace colorprim=FROM:auto");
        const auto prim=static_cast<ColorPrimaries>(CVColorPrimariesGetIntegerCodePointForString(static_cast<CFStringRef>(v)));
        const auto gamut=gamut_matrix(ColorPrimaries::bt709,prim); const auto luma=primaries_luma(prim);
        std::copy(gamut.begin(),gamut.end(),p.gamut); std::copy(luma.begin(),luma.end(),p.luma);
        // Keep SAR, aperture, HDR metadata and source chroma; subtitle processing
        // does not reinterpret any attachment. Input must already be left-sited.
        if (f.chroma_x!=0.5F || f.chroma_y!=1.0F) throw std::runtime_error("Subtitle canvas requires left chroma; request --input-chroma-loc to resample first");

    }
    void dispatch(id<MTLComputeCommandEncoder> e,id<MTLComputePipelineState> p,int w,int h) {
        const auto x=p.threadExecutionWidth;
        [e dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(w),static_cast<NSUInteger>(h),1)
            threadsPerThreadgroup:MTLSizeMake(x,std::min<NSUInteger>(8,p.maxTotalThreadsPerThreadgroup/x),1)];
        [e endEncoding];
    }
};
GpuSubtitles::GpuSubtitles(CVPixelBufferRef source):impl_(std::make_unique<Impl>(source)) {
    @autoreleasepool {
        auto& s=*impl_;
        s.device=MTLCreateSystemDefaultDevice(); if (!s.device) metal_error("Create subtitle Metal device",nil);
        s.queue=[s.device newCommandQueue]; if (!s.queue) metal_error("Create subtitle command queue",nil);
        MTLCompileOptions* options=[[MTLCompileOptions alloc] init];
        options.languageVersion=MTLLanguageVersion2_4; options.fastMathEnabled=NO;
        NSError* error=nil;
        auto lib=[s.device newLibraryWithSource:[NSString stringWithUTF8String:filter_shader_source] options:options error:&error];
        if (!lib) metal_error("Compile subtitle Metal kernels",error);
        const auto pipeline=[&](NSString* name) {
            auto fn=[lib newFunctionWithName:name]; if (!fn) metal_error("Load subtitle kernel",nil);
            auto p=[s.device newComputePipelineStateWithFunction:fn error:&error];
            if (!p) metal_error("Create subtitle pipeline",error); return p;
        };
        s.clear=pipeline(@"subtitle_clear"); s.glyph=pipeline(@"subtitle_glyph"); s.blend=pipeline(@"subtitle_blend");
        if (static_cast<std::uint64_t>(s.plan.width)*static_cast<std::uint64_t>(s.plan.height)*16>512*1024*1024)
            throw std::runtime_error("Subtitle RGBA32Float canvas exceeds 512 MiB");
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:static_cast<NSUInteger>(s.plan.width) height:static_cast<NSUInteger>(s.plan.height) mipmapped:NO];
        desc.storageMode=MTLStorageModePrivate; desc.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
        s.overlay=[s.device newTextureWithDescriptor:desc]; if (!s.overlay) metal_error("Allocate subtitle overlay",nil);
        CVMetalTextureCacheRef raw=nullptr;
        check_status(CVMetalTextureCacheCreate(nullptr,nullptr,s.device,nullptr,&raw),"Create subtitle texture cache"); s.cache.emplace(raw);
    }
}
GpuSubtitles::~GpuSubtitles()=default;
CFRef<CVPixelBufferRef> GpuSubtitles::process(CVPixelBufferRef source,std::span<const SubtitleBitmap> images,bool changed) {
    @autoreleasepool {
        auto& s=*impl_;
        validate_filter_frame(s.plan,source);
        if (images.empty()) { s.initialized=false; CFRetain(source); return CFRef(source); }
        auto command=[s.queue commandBuffer]; if (!command) metal_error("Create subtitle command buffer",nil);
        command.label=@"VTEnc libass masks / Metal linear-light composition";
        const auto encoder=[&](id<MTLComputePipelineState> p) {
            auto e=[command computeCommandEncoder]; if (!e) metal_error("Create subtitle encoder",nil);
            [e setComputePipelineState:p]; return e;
        };
        if (changed||!s.initialized) {
            auto clear=encoder(s.clear); [clear setTexture:s.overlay atIndex:0]; s.dispatch(clear,s.clear,s.plan.width,s.plan.height);
            std::size_t total=0;
            for (const auto& image:images) {
                // Last libass row has only width bytes, not stride bytes.
                const auto bytes=static_cast<std::size_t>(image.height-1)*static_cast<std::size_t>(image.stride)+static_cast<std::size_t>(image.width);
                total+=bytes; if (total>256*1024*1024) throw std::runtime_error("Subtitle masks exceed 256 MiB per frame");
                auto mask=[s.device newBufferWithBytes:image.data length:bytes options:MTLResourceStorageModeShared];
                if (!mask) metal_error("Upload libass alpha mask",nil);
                struct Glyph { int x,y,w,h; unsigned stride,color; float opacity,white; int cx,cy,cw,ch; unsigned matrix; };
                Glyph g{image.x,image.y,image.width,image.height,static_cast<unsigned>(image.stride),image.color,image.opacity,image.white_nits,
                    image.clip[0],image.clip[1],image.clip[2],image.clip[3],image.color_matrix};
                auto e=encoder(s.glyph); [e setTexture:s.overlay atIndex:0]; [e setBuffer:mask offset:0 atIndex:0];
                [e setBytes:&g length:sizeof(g) atIndex:1]; [e setBytes:&s.parameters length:sizeof(s.parameters) atIndex:2];
                s.dispatch(e,s.glyph,image.width,image.height);
            }
            s.initialized=true;
        }
        auto output=make_filter_buffer(s.pool.get(),s.plan);
        std::array textures{texture(s.cache->get(),source,0,false),texture(s.cache->get(),source,1,false),
            texture(s.cache->get(),output.get(),0,true),texture(s.cache->get(),output.get(),1,true)};
        auto e=encoder(s.blend);
        for (std::size_t i=0;i<textures.size();++i) [e setTexture:CVMetalTextureGetTexture(textures[i].get()) atIndex:i];
        [e setTexture:s.overlay atIndex:4]; [e setBytes:&s.parameters length:sizeof(s.parameters) atIndex:0];
        s.dispatch(e,s.blend,s.plan.width,s.plan.height);
        [command commit]; [command waitUntilCompleted];
        if (command.status!=MTLCommandBufferStatusCompleted) metal_error("Execute subtitle Metal composition",command.error);
        return output;
    }
}
} // namespace vtenc
