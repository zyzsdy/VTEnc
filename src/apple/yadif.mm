#include "yadif.hpp"
#include "gpu_filter.hpp"
#include "output_metadata.hpp"
#include "encoder_session.hpp"
#include "filter_shader.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CVMetalTextureCache.h>
#include <array>

namespace vtenc {
namespace {
[[noreturn]] void fail(const char* operation, NSError* error = nil) {
    throw std::runtime_error(std::string(operation) + ": " + (error ? error.description.UTF8String : "Metal returned nil"));
}
CFRef<CVMetalTextureRef> map_plane(CVMetalTextureCacheRef cache, CVPixelBufferRef pixel, unsigned plane, bool write, unsigned depth) {
    const auto format = depth == 10 ? (plane ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm)
                                    : (plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm);
    NSDictionary* attributes = @{(__bridge NSString*)kCVMetalTextureUsage: @(write ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead)};
    CVMetalTextureRef raw = nullptr;
    check_status(CVMetalTextureCacheCreateTextureFromImage(nullptr, cache, pixel, (__bridge CFDictionaryRef)attributes,
        format, CVPixelBufferGetWidthOfPlane(pixel,plane), CVPixelBufferGetHeightOfPlane(pixel,plane), plane, &raw), "Map YADIF Metal plane");
    CFRef result(raw);
    if (!CVMetalTextureGetTexture(raw)) fail("Get YADIF Metal texture");
    return result;
}
}
struct GpuYadif::Impl {
    struct Parameters { unsigned field, second, copy, depth, spatial_only; };
    static_assert(sizeof(Parameters) == 20 && std::is_standard_layout_v<Parameters>);
    std::size_t width, height;
    OSType format;
    unsigned depth = 0;
    CFRef<CVPixelBufferPoolRef> pool;
    std::optional<CFRef<CFDictionaryRef>> metadata;
    std::optional<CFRef<CVMetalTextureCacheRef>> cache;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline;
    explicit Impl(CVPixelBufferRef source)
        : width(CVPixelBufferGetWidth(source)), height(CVPixelBufferGetHeight(source)),
          format(CVPixelBufferGetPixelFormatType(source)), pool(make_metal_pixel_pool(static_cast<int>(width), static_cast<int>(height), format)) {
        if (width == 0 || width % 2 || height < 8 || height % 4 || CVPixelBufferGetPlaneCount(source) != 2)
            throw std::runtime_error("YADIF requires even width and height >= 8 divisible by four (interlaced 4:2:0)");
        switch (format) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: depth = 8; break;
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: depth = 10; break;
        default: throw std::runtime_error("Metal YADIF requires NV12/P010; pixel format=" + std::to_string(format));
        }
        if (auto raw = CVBufferCopyAttachments(source,kCVAttachmentMode_ShouldPropagate)) metadata.emplace(raw);
        // YADIF operates on each plane's sample grid. Unlike Bob it does not
        // resample distinct top/bottom chroma grids into a common phase.
        const auto top = metadata ? CFDictionaryGetValue(metadata->get(),kCVImageBufferChromaLocationTopFieldKey) : nullptr;
        const auto bottom = metadata ? CFDictionaryGetValue(metadata->get(),kCVImageBufferChromaLocationBottomFieldKey) : nullptr;
        if (top && bottom && !CFEqual(top,bottom)) throw std::runtime_error("YADIF requires equal top/bottom chroma locations");
    }
    void validate(CVPixelBufferRef pixel) const {
        if (CVPixelBufferGetWidth(pixel) != width || CVPixelBufferGetHeight(pixel) != height ||
            CVPixelBufferGetPixelFormatType(pixel) != format || CVPixelBufferGetPlaneCount(pixel) != 2)
            throw std::runtime_error("Decoded pixel layout changed during Metal YADIF");
        for (const auto key : {kCVImageBufferColorPrimariesKey,kCVImageBufferTransferFunctionKey,
             kCVImageBufferYCbCrMatrixKey,kCVImageBufferChromaLocationTopFieldKey,kCVImageBufferChromaLocationBottomFieldKey,
             kCVImageBufferPixelAspectRatioKey,kCVImageBufferCleanApertureKey,
             kCMFormatDescriptionExtension_MasteringDisplayColorVolume,kCMFormatDescriptionExtension_ContentLightLevelInfo,
             source_master_key(),source_light_key()}) {
            const auto expected = metadata ? CFDictionaryGetValue(metadata->get(),key) : nullptr;
            auto raw = CVBufferCopyAttachment(pixel,key,nullptr);
            if (raw) {
                CFRef value(raw);
                if (!expected || !CFEqual(expected,value.get())) throw std::runtime_error("Decoded color/geometry metadata changed during YADIF");
            } else if (expected) throw std::runtime_error("Decoded color/geometry metadata disappeared during YADIF");
        }
    }
};
GpuYadif::GpuYadif(CVPixelBufferRef source) : impl_(std::make_unique<Impl>(source)) {
    @autoreleasepool {
        auto& s = *impl_;
        s.device = MTLCreateSystemDefaultDevice();
        if (!s.device) fail("Create YADIF Metal device");
        s.queue = [s.device newCommandQueue];
        if (!s.queue) fail("Create YADIF command queue");
        NSError* error = nil;
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion2_4; options.fastMathEnabled = NO;
        auto library = [s.device newLibraryWithSource:[NSString stringWithUTF8String:filter_shader_source] options:options error:&error];
        if (!library) fail("Compile Metal YADIF",error);
        auto function = [library newFunctionWithName:@"yadif_frame"];
        if (!function) fail("Load Metal YADIF kernel");
        s.pipeline = [s.device newComputePipelineStateWithFunction:function error:&error];
        if (!s.pipeline) fail("Create Metal YADIF pipeline",error);
        CVMetalTextureCacheRef raw = nullptr;
        check_status(CVMetalTextureCacheCreate(nullptr,nullptr,s.device,nullptr,&raw),"Create YADIF texture cache");
        s.cache.emplace(raw);
    }
}
GpuYadif::~GpuYadif() = default;
CFRef<CVPixelBufferRef> GpuYadif::process(CVPixelBufferRef previous, CVPixelBufferRef current,
    CVPixelBufferRef following, unsigned field, bool second, bool copy, bool spatial_only) {
    @autoreleasepool {
        auto& s = *impl_;
        if (field > 1) throw std::runtime_error("YADIF field parity must be 0 or 1");
        for (auto pixel : {previous,current,following}) s.validate(pixel);
        const int limit = 6; // Scheduler/CFR lookahead plus the output being written.
        CFRef threshold(CFNumberCreate(nullptr,kCFNumberIntType,&limit));
        const void* keys[] = {kCVPixelBufferPoolAllocationThresholdKey};
        const void* values[] = {threshold.get()};
        CFRef attributes(CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
        CVPixelBufferRef raw = nullptr;
        check_status(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr,s.pool.get(),attributes.get(),&raw),"Allocate bounded YADIF output");
        CFRef output(raw);
        CVBufferRemoveAllAttachments(raw);
        CVBufferPropagateAttachments(current,raw);
        CVBufferRemoveAttachment(raw,kCVImageBufferFieldDetailKey);
        const int one = 1;
        CFRef count(CFNumberCreate(nullptr,kCFNumberIntType,&one));
        CVBufferSetAttachment(raw,kCVImageBufferFieldCountKey,count.get(),kCVAttachmentMode_ShouldPropagate);
        auto command = [s.queue commandBuffer];
        if (!command) fail("Create YADIF command buffer");
        command.label = @"VTEnc motion-adaptive YADIF";
        // All IOSurface wrappers and buffers stay alive until completion.
        std::array textures{map_plane(s.cache->get(),previous,0,false,s.depth),map_plane(s.cache->get(),current,0,false,s.depth),
            map_plane(s.cache->get(),following,0,false,s.depth),map_plane(s.cache->get(),raw,0,true,s.depth),
            map_plane(s.cache->get(),previous,1,false,s.depth),map_plane(s.cache->get(),current,1,false,s.depth),
            map_plane(s.cache->get(),following,1,false,s.depth),map_plane(s.cache->get(),raw,1,true,s.depth)};
        const Impl::Parameters p{field,second,copy,s.depth,spatial_only};
        for (unsigned plane = 0; plane < 2; ++plane) {
            auto encoder = [command computeCommandEncoder];
            if (!encoder) fail("Create YADIF compute encoder");
            [encoder setComputePipelineState:s.pipeline];
            for (unsigned i = 0; i < 4; ++i) [encoder setTexture:CVMetalTextureGetTexture(textures[plane*4+i].get()) atIndex:i];
            [encoder setBytes:&p length:sizeof(p) atIndex:0];
            const auto x = s.pipeline.threadExecutionWidth;
            const auto y = std::min<NSUInteger>(8,s.pipeline.maxTotalThreadsPerThreadgroup/x);
            [encoder dispatchThreads:MTLSizeMake(CVPixelBufferGetWidthOfPlane(raw,plane),CVPixelBufferGetHeightOfPlane(raw,plane),1)
                threadsPerThreadgroup:MTLSizeMake(x,y,1)];
            [encoder endEncoding];
        }
        [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) fail("Execute Metal YADIF",command.error);
        return output;
    }
}
}
