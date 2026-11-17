#include "gpu_filter.hpp"
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
} // namespace vtenc
