// Explicit Apple Silicon test: actual Metal results against scalar CUDA-formula
// references and known progressive film frames. Never part of default CTest.
#include "ivtc.hpp"
#include "gpu_filter.hpp"
#include "encoder_session.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <vector>
using namespace vtenc;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
constexpr int width = 66, height = 24; // Partial GPU groups and padded IOSurface rows.
using Image = std::array<std::vector<int>, 2>;
Image pattern(int seed, int depth) {
    Image image;
    for (int plane = 0; plane < 2; ++plane) {
        auto& data = image[static_cast<std::size_t>(plane)];
        for (int y = 0; y < height / (plane + 1); ++y) for (int x = 0; x < width; ++x)
            data.push_back((16 + (x * 19 + y * 41 + seed * 31) % 220) << (depth - 8));
    }
    return image;
}
CFRef<CVPixelBufferRef> upload(const Image& image, int depth, bool full = false) {
    const auto type = depth == 10 ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                                  : (full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    auto pool = make_metal_pixel_pool(width, height, type);
    CVPixelBufferRef raw = nullptr;
    check_status(CVPixelBufferPoolCreatePixelBuffer(nullptr, pool.get(), &raw), "Create IVTC fixture");
    CFRef output(raw);
    check_status(CVPixelBufferLockBaseAddress(raw, 0), "Lock IVTC fixture");
    for (std::size_t p = 0; p < 2; ++p) {
        auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(raw, p));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(raw, p);
        for (std::size_t i = 0; i < image[p].size(); ++i) {
            auto* row = base + (i / width) * stride;
            if (depth == 10) reinterpret_cast<std::uint16_t*>(row)[i % width] = static_cast<std::uint16_t>(image[p][i] << 6);
            else row[i % width] = static_cast<unsigned char>(image[p][i]);
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(raw, 0), "Unlock IVTC fixture");
    return output;
}
Image download(CVPixelBufferRef raw, int depth) {
    Image image;
    check_status(CVPixelBufferLockBaseAddress(raw, kCVPixelBufferLock_ReadOnly), "Read IVTC result");
    for (std::size_t p = 0; p < 2; ++p) {
        const auto* base = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(raw, p));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(raw, p);
        for (std::size_t y = 0; y < CVPixelBufferGetHeightOfPlane(raw, p); ++y) for (int x = 0; x < width; ++x) {
            const auto* row = base + y * stride;
            int value = row[x];
            if (depth == 10) {
                value = reinterpret_cast<const std::uint16_t*>(row)[x];
                require((value & 63) == 0, "IVTC P010 low bits are nonzero"); value >>= 6;
            }
            image[p].push_back(value);
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(raw, kCVPixelBufferLock_ReadOnly), "Unlock IVTC result");
    return image;
}
int read(const Image& image, int plane, int x, int y) {
    y = std::clamp(y, 0, height / (plane + 1) - 1);
    return image[static_cast<std::size_t>(plane)][static_cast<std::size_t>(y * width + x)];
}
Image woven(const std::array<Image, 3>& images, unsigned first, unsigned match) {
    auto output = images[1];
    for (int p = 0; p < 2; ++p) for (int y = 0; y < height / (p + 1); ++y) {
        const unsigned source = match == 1 && (static_cast<unsigned>(y) & 1U) != first ? 0U :
                                match == 2 && (static_cast<unsigned>(y) & 1U) == first ? 2U : 1U;
        std::copy_n(images[source][static_cast<std::size_t>(p)].begin() + y * width, width,
                    output[static_cast<std::size_t>(p)].begin() + y * width);
    }
    return output;
}
Image repair_reference(const std::array<Image, 3>& images, unsigned first, unsigned match, int depth, int dthresh) {
    auto out = woven(images, first, match);
    const auto original = out;
    const int gate = dthresh << (depth - 8), maximum = (1 << depth) - 1;
    for (int p = 0; p < 2; ++p) for (int y = 0; y < height / (p + 1); ++y) for (int x = 0; x < width; ++x) {
        if ((static_cast<unsigned>(y) & 1U) == first) continue;
        int up = read(original,p,x,y-1), dn = read(original,p,x,y+1), val = read(original,p,x,y);
        int linear = (up + dn + 1) / 2;
        if (gate > 0 && std::abs(val - linear) <= gate) continue;
        int prev = read(images[0],p,x,y), next = read(images[2],p,x,y);
        int motion = std::max(0, std::max(std::abs(prev-val), std::abs(next-val)) - std::max(1,gate/8));
        int spatial = linear;
        if (y >= 3 && y < height / (p+1) - 3)
            spatial = (5077*(up+dn)-981*(read(original,p,x,y-3)+read(original,p,x,y+3))+4096) >> 13;
        int blend = spatial;
        if (gate > 0) {
            int w = std::min(motion,gate*2), w2 = (w*w+gate)/(gate*2);
            int temporal = (prev+next+2*val+2)/4;
            blend = (temporal*(gate*2-w2)+spatial*w2+gate)/(gate*2);
        }
        int epsilon = std::min(std::max(1,gate/2),8 << (depth-8));
        out[static_cast<std::size_t>(p)][static_cast<std::size_t>(y*width+x)] = std::clamp(blend,std::max(0,std::min(up,dn)-epsilon),std::min(maximum,std::max(up,dn)+epsilon));
    }
    return out;
}
IvtcScores score_reference(const std::array<Image, 3>& images, unsigned first, int depth) {
    IvtcScores result;
    const int nt = ((1 << depth)-1)*10/255, threshold = ((1 << depth)-1)*4/255;
    for (unsigned m = 0; m < 3; ++m) {
        auto raster = woven(images, first, m);
        std::uint64_t sm = 0, sc = 0, nm = 0, nc = 0;
        for (int gy = 0; gy < height; gy += 8) for (int gx = 0; gx < width; gx += 32) {
            std::uint64_t metric = 0, comb = 0;
            for (int y = gy; y < std::min(gy+8,height-4); ++y) for (int x = gx; x < std::min(gx+32,width); ++x) {
                if ((static_cast<unsigned>(y)&1U) != first) continue;
                int a=read(raster,0,x,y), b=read(raster,0,x,y+1), c=read(raster,0,x,y+2), d=read(raster,0,x,y+3), e=read(raster,0,x,y+4);
                int diff=std::abs(b-(a+c+1)/2)+std::abs(d-(c+e+1)/2);
                if (diff>nt) metric += static_cast<unsigned>(diff);
                if (std::abs(b-a)>threshold && std::abs(b-c)>threshold && ((b-a<0)==(b-c<0))) ++comb;
            }
            if (metric>static_cast<unsigned>(nt*4)) { sm+=metric; ++nm; }
            if (comb>static_cast<unsigned>(nt*4)) { sc+=comb; ++nc; }
            result[m].maximum=std::max(result[m].maximum,comb);
            result[m].blocks += comb>=8;
        }
        result[m].match=nm>=2 ? sm/nm : 0; result[m].comb=nc>=2 ? sc/nc : 0;
    }
    return result;
}
Image film(int index, int depth) {
    Image result;
    for (int p=0;p<2;++p) for (int y=0;y<height/(p+1);++y) for (int x=0;x<width;++x)
        result[static_cast<std::size_t>(p)].push_back((30 + ((x + index*9) % width < 20 ? 110 : 0) + p*30) << (depth-8));
    return result;
}
class Telecine final : public DecodedFrames {
public:
    Telecine(int depth, unsigned first, int count, int phase, bool vfr=false)
        : depth_(depth), first_(first), count_(count), phase_(phase), vfr_(vfr) {}
    std::unique_ptr<DecodedFrame> next() override {
        if (index_==count_) return nullptr;
        const int n=index_+phase_, base=(n/5)*4;
        constexpr int first_ids[]={0,1,1,2,3}, second_ids[]={0,1,2,3,3};
        auto a=film(base+first_ids[n%5],depth_), b=film(base+second_ids[n%5],depth_);
        for (int p=0;p<2;++p) for (int y=0;y<height/(p+1);++y)
            if ((static_cast<unsigned>(y)&1U)!=first_) std::copy_n(b[static_cast<std::size_t>(p)].begin()+y*width,width,a[static_cast<std::size_t>(p)].begin()+y*width);
        auto pixel=upload(a,depth_);
        // Mix progressive/interlaced tags as in actual hard telecine.
        const int fields=index_%3==0 ? 1 : 2;
        CFRef count(CFNumberCreate(nullptr,kCFNumberIntType,&fields));
        CVBufferSetAttachment(pixel.get(),kCVImageBufferFieldCountKey,count.get(),kCVAttachmentMode_ShouldPropagate);
        const auto duration=CMTimeMake(vfr_ ? 900+index_%4*101 : 1001,30000);
        auto result=std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel),pts_,duration});
        pts_=CMTimeAdd(pts_,duration); ++index_; return result;
    }
    CMTime pts_=CMTimeMake(1,5);
private:
    int depth_; unsigned first_; int count_,phase_,index_=0; bool vfr_;
};
}
int main() try {
    int cases=0;
    for (int depth : {8,10}) for (bool full : {false,true}) {
        std::array<Image,3> images{pattern(0,depth),pattern(1,depth),pattern(2,depth)};
        std::array pixels{upload(images[0],depth,full),upload(images[1],depth,full),upload(images[2],depth,full)};
        IvtcConfig config;
        GpuIvtc gpu(pixels[1].get(),config);
        std::uint64_t sad=0;
        for (std::size_t i=0;i<images[0][0].size();++i) sad+=static_cast<unsigned>(std::abs(images[0][0][i]-images[1][0][i]));
        require(gpu.difference(pixels[0].get(),pixels[1].get())==sad,"Metal SAD differs from scalar reference");
        for (unsigned first : {0U,1U}) {
            auto actual=gpu.score(pixels[0].get(),pixels[1].get(),pixels[2].get(),first), expected=score_reference(images,first,depth);
            for (unsigned m=0;m<3;++m) {
                require(actual[m].match==expected[m].match && actual[m].comb==expected[m].comb && actual[m].maximum==expected[m].maximum && actual[m].blocks==expected[m].blocks,"Metal C/P/N score differs from scalar reference");
                for (bool repair : {false,true}) {
                    auto out=gpu.synthesize(pixels[0].get(),pixels[1].get(),pixels[2].get(),first,m,repair);
                    require(download(out.get(),depth)==(repair ? repair_reference(images,first,m,depth,7) : woven(images,first,m)),"Metal reconstruction differs from scalar reference");
                    ++cases;
                }
            }
        }
    }
    for (int depth : {8,10}) for (unsigned first : {0U,1U}) for (int phase=0;phase<5;++phase) {
        auto source=std::make_unique<Telecine>(depth,first,20,phase);
        auto fixture=upload(film(0,depth),depth);
        CMVideoFormatDescriptionRef raw=nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,fixture.get(),&raw),"Describe IVTC fixture"); CFRef format(raw);
        FilterConfig config; config.ivtc.enabled=true; config.field_order=first==0 ? FieldOrder::tff : FieldOrder::bff;
        volatile std::sig_atomic_t cancelled=0;
        IvtcFrames frames(std::move(source),format.get(),config,cancelled);
        int count=0, last=-1;
        while (auto f=frames.next()) {
            auto image=download(f->pixel.get(),depth);
            int id=-1;
            for (int i=0;i<20;++i) if (image==film(i,depth)) { id=i; break; }
            // The first/last incomplete film exposure at a cropped cadence edge
            // may need residual repair. Interior outputs must be exact originals.
            if (count>0 && count<15) {
                require(id>=0,"IVTC did not recover a progressive film frame");
                if (!(last<0 || id==last+1)) std::cerr << "depth="<<depth<<" first="<<first<<" phase="<<phase<<" output="<<count<<" last="<<last<<" id="<<id<<"\n";
                require(last<0 || id==last+1,"IVTC duplicated, reordered or lost an interior film frame");
            }
            last=id;
            require(CMTimeCompare(f->pts,CMTimeAdd(CMTimeMake(1,5),CMTimeMake(count*1001,24000)))==0,"IVTC CFR PTS drift");
            require(CMTimeCompare(f->duration,CMTimeMake(1001,24000))==0,"IVTC wrong film duration");
            ++count;
        }
        require(count==16,"IVTC wrong 5:4 count"); ++cases;
    }
    // Every EOF remainder, one-frame inputs, VFR and match-only drain.
    for (int count=1;count<=11;++count) for (int cycle : {0,5}) {
        auto source=std::make_unique<Telecine>(8,0,count,0,true); auto* input=source.get();
        auto fixture=upload(film(0,8),8); CMVideoFormatDescriptionRef raw=nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,fixture.get(),&raw),"Describe drain fixture"); CFRef format(raw);
        FilterConfig config; config.ivtc.enabled=true; config.ivtc.cycle=cycle; config.field_order=FieldOrder::tff;
        volatile std::sig_atomic_t cancelled=0; IvtcFrames frames(std::move(source),format.get(),config,cancelled);
        CMTime end=CMTimeMake(1,5); int emitted=0;
        while(auto f=frames.next()) { require(CMTimeCompare(end,f->pts)==0,"IVTC VFR gap/overlap"); end=CMTimeAdd(f->pts,f->duration); ++emitted; }
        require(emitted==count-(cycle==5 ? count/5 : 0),"IVTC incomplete cycle dropped a frame");
        require(CMTimeCompare(end,input->pts_)==0,"IVTC changed VFR end time"); ++cases;
    }
    std::cout << "Metal IVTC: " << cases << " reconstruction, cadence and timeline cases passed\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
