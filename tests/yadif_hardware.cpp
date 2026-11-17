// Explicit Apple Silicon validation; never included in default CTest.
// Scalar YADIF formulas adapted from NVEncFilterYadif.cu (rigaya, MIT).
// See THIRD_PARTY_NOTICES.md. CPU reference exists only in this test.
#include "yadif.hpp"
#include "gpu_filter.hpp"
#include "encoder_session.hpp"
#include "bob.hpp"
#include <algorithm>
#include <array>
#include <iostream>
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
            data.push_back((x * 191 + y * 419 + seed * 313) % (1 << depth));
    }
    return image;
}
CFRef<CVPixelBufferRef> upload(const Image& image, int depth, bool full = false) {
    const auto type = depth == 10 ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                                  : (full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    auto pool = make_metal_pixel_pool(width, static_cast<int>(image[0].size() / width), type);
    CVPixelBufferRef raw = nullptr;
    check_status(CVPixelBufferPoolCreatePixelBuffer(nullptr, pool.get(), &raw), "Create YADIF fixture");
    CFRef output(raw);
    check_status(CVPixelBufferLockBaseAddress(raw, 0), "Lock YADIF fixture");
    for (std::size_t p = 0; p < 2; ++p) {
        auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(raw, p));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(raw, p);
        for (std::size_t i = 0; i < image[p].size(); ++i) {
            auto* row = base + (i / width) * stride;
            if (depth == 10) reinterpret_cast<std::uint16_t*>(row)[i % width] = static_cast<std::uint16_t>(image[p][i] << 6);
            else row[i % width] = static_cast<unsigned char>(image[p][i]);
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(raw, 0), "Unlock YADIF fixture");
    return output;
}
Image download(CVPixelBufferRef raw, int depth) {
    Image image;
    check_status(CVPixelBufferLockBaseAddress(raw, kCVPixelBufferLock_ReadOnly), "Read YADIF result");
    for (std::size_t p = 0; p < 2; ++p) {
        const auto* base = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(raw, p));
        const auto stride = CVPixelBufferGetBytesPerRowOfPlane(raw, p);
        for (std::size_t y = 0; y < CVPixelBufferGetHeightOfPlane(raw, p); ++y) for (int x = 0; x < width; ++x) {
            const auto* row = base + y * stride;
            int value = row[x];
            if (depth == 10) {
                value = reinterpret_cast<const std::uint16_t*>(row)[x];
                require((value & 63) == 0, "YADIF P010 low bits are nonzero"); value >>= 6;
            }
            image[p].push_back(value);
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(raw, kCVPixelBufferLock_ReadOnly), "Unlock YADIF result");
    return image;
}
int sample(const Image& a, int p, int x, int y) {
    const int step=p ? 2:1, component=p ? ((x%2)+2)%2:0;
    x=std::clamp((x-component)/step,0,width/step-1)*step+component;
    y=std::clamp(y,0,static_cast<int>(a[static_cast<std::size_t>(p)].size()/width)-1);
    return a[static_cast<std::size_t>(p)][static_cast<std::size_t>(y*width+x)];
}
Image reference(const std::array<Image,3>& frames, unsigned field, bool second, bool spatial_only=false) {
    Image out=frames[1];
    for(int p=0;p<2;++p) for(int y=0;y<height/(p+1);++y) for(int x=0;x<width;++x) {
        if((static_cast<unsigned>(y)&1U)==field) continue;
        const int step=p ? 2:1;
        const auto at=[&](int f,int dx,int dy) { return sample(frames[static_cast<std::size_t>(f)],p,x+dx*step,y+dy); };
        int best=0;
        const auto score=[&](int shift) {
            int sum=0;
            for(int dx=-1;dx<=1;++dx) sum+=std::abs(at(1,dx+shift,-1)-at(1,dx-shift,1));
            return sum;
        };
        // Independent directional loop, including upstream's conditional ±2 search.
        int best_score=score(0);
        for(int sign:{-1,1}) if(score(sign)<best_score) {
            best=sign; best_score=score(sign);
            if(score(sign*2)<best_score) { best=sign*2; best_score=score(sign*2); }
        }
        int value=(at(1,best,-1)+at(1,-best,1))/2;
        if(!spatial_only) {
            const int a=second ? 1:0,b=second ? 2:1;
            const int center=(at(a,0,0)+at(b,0,0))/2;
            const int up=at(1,0,-1),down=at(1,0,1);
            const int upper=(at(a,0,-2)+at(b,0,-2))/2, lower=(at(a,0,2)+at(b,0,2))/2;
            int difference=std::max({std::abs(at(a,0,0)-at(b,0,0)),
                (std::abs(at(0,0,-1)-up)+std::abs(at(0,0,1)-down))/2,
                (std::abs(at(2,0,-1)-up)+std::abs(at(2,0,1)-down))/2,
                -std::max({center-down,center-up,std::min(upper-up,lower-down)}),
                std::min({center-down,center-up,std::max(upper-up,lower-down)})});
            value=std::clamp(value,center-difference,center+difference);
        }
        out[static_cast<std::size_t>(p)][static_cast<std::size_t>(y*width+x)]=value;
    }
    return out;
}
Image bars(int index,int depth) {
    auto a=pattern(0,depth);
    for(int p=0;p<2;++p) for(int y=0;y<height/(p+1);++y) for(int x=0;x<width;++x)
        a[static_cast<std::size_t>(p)][static_cast<std::size_t>(y*width+x)]=
            (p ? (x%2 ? 160:80) : (x+index*3)%width<width/2 ? 200:30) << (depth-8);
    return a;
}
Image weave(const Image& a,const Image& b,unsigned first) {
    Image out=a;
    for(int p=0;p<2;++p) for(int y=0;y<height/(p+1);++y)
        if((static_cast<unsigned>(y)&1U)!=first)
            std::copy_n(b[static_cast<std::size_t>(p)].begin()+y*width,width,out[static_cast<std::size_t>(p)].begin()+y*width);
    return out;
}
struct Picture {
    Image image;
    FrameCadence cadence{true,true,0,FieldOrder::tff};
    int ticks=1001;
};
class Source final : public DecodedFrames {
public:
    Source(std::vector<Picture> pictures,int bits) : pictures_(std::move(pictures)),depth_(bits) {}
    CMTime end=CMTimeMake(1,5);
    bool omit_duration=false;
    std::unique_ptr<DecodedFrame> next() override {
        if(index_==pictures_.size()) return nullptr;
        const auto& p=pictures_[index_++];
        const auto duration=CMTimeMake(p.ticks,60000);
        auto frame=std::make_unique<DecodedFrame>(DecodedFrame{upload(p.image,depth_),end,
            omit_duration && index_==pictures_.size() ? kCMTimeInvalid:duration,p.cadence});
        end=CMTimeAdd(end,duration); return frame;
    }
private:
    std::vector<Picture> pictures_; int depth_; std::size_t index_=0;
};
CFRef<CMVideoFormatDescriptionRef> describe(CVPixelBufferRef pixel) {
    CMVideoFormatDescriptionRef raw=nullptr;
    check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,pixel,&raw),"Describe YADIF fixture");
    return CFRef(raw);
}
void compare(const Image& actual,const Image& expected,const char* context) {
    for(std::size_t p=0;p<2;++p) for(std::size_t i=0;i<actual[p].size();++i)
        if(actual[p][i]!=expected[p][i]) throw std::runtime_error(std::string(context)+" plane="+std::to_string(p)+
            " x="+std::to_string(i%width)+" y="+std::to_string(i/width)+" actual="+std::to_string(actual[p][i])+" expected="+std::to_string(expected[p][i]));
}
int pixel_tests() {
    int cases=0;
    for(int depth:{8,10}) for(bool full:{false,true}) {
        const auto a=upload(pattern(0,depth),depth,full);
        GpuYadif gpu(a.get());
        for(int scenario=0;scenario<8;++scenario) {
            std::array<Image,3> images{pattern(0,depth),pattern(1,depth),pattern(2,depth)};
            if(scenario==1) images[0]=images[1]; // First frame clamp.
            if(scenario==2) images[2]=images[1]; // Last frame clamp.
            if(scenario==3) images[0]=images[2]=images[1]; // Static fine detail.
            if(scenario==4) for(int f=0;f<3;++f) images[static_cast<std::size_t>(f)]=weave(bars(f*2,depth),bars(f*2+1,depth),0);
            if(scenario==5) images[0]=bars(0,depth); // Abrupt cut.
            if(scenario==6) for(auto& im:images) for(auto& plane:im) std::fill(plane.begin(),plane.end(),0);
            if(scenario==7) for(auto& im:images) for(auto& plane:im) std::fill(plane.begin(),plane.end(),(1<<depth)-1);
            std::array pixels{upload(images[0],depth,full),upload(images[1],depth,full),upload(images[2],depth,full)};
            for(unsigned field:{0U,1U}) for(bool second:{false,true}) {
                auto out=gpu.process(pixels[0].get(),pixels[1].get(),pixels[2].get(),field,second);
                compare(download(out.get(),depth),reference(images,field,second),"YADIF scalar mismatch");
                auto spatial=gpu.process(pixels[0].get(),pixels[1].get(),pixels[2].get(),field,second,false,true);
                compare(download(spatial.get(),depth),reference(images,field,second,true),"Orphan borrowed missing field pixels");
                auto copy=gpu.process(pixels[0].get(),pixels[1].get(),pixels[2].get(),field,second,true);
                compare(download(copy.get(),depth),images[1],"Progressive pixels changed");
                require(!CVBufferHasAttachment(out.get(),kCVImageBufferFieldDetailKey),"Progressive field detail leaked");
                ++cases;
            }
        }
        // A static horizontal one-pixel line must survive, unlike spatial Bob.
        auto thin=bars(0,depth);
        for(int x=0;x<width;++x) thin[0][static_cast<std::size_t>(11*width+x)]=235<<(depth-8);
        auto pixels=upload(thin,depth,full);
        auto out=gpu.process(pixels.get(),pixels.get(),pixels.get(),0,false);
        compare(download(out.get(),depth),thin,"Static fine line lost"); ++cases;
    }
    // Metadata changes must fail even when the frame would otherwise bypass
    // interpolation; field flags alone are allowed to change.
    auto a=upload(pattern(0,8),8), b=upload(pattern(1,8),8);
    GpuYadif gpu(a.get());
    CVBufferSetAttachment(b.get(),kCVImageBufferYCbCrMatrixKey,kCVImageBufferYCbCrMatrix_ITU_R_709_2,kCVAttachmentMode_ShouldPropagate);
    bool rejected=false;
    try { (void)gpu.process(a.get(),b.get(),a.get(),0,false,true); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"YADIF ignored a color metadata change"); ++cases;
    CVBufferSetAttachment(a.get(),kCVImageBufferChromaLocationTopFieldKey,kCVImageBufferChromaLocation_Left,kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(a.get(),kCVImageBufferChromaLocationBottomFieldKey,kCVImageBufferChromaLocation_Center,kCVAttachmentMode_ShouldPropagate);
    rejected=false;
    try { GpuYadif invalid(a.get()); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"YADIF accepted mismatched field chroma grids"); ++cases;
    return cases;
}
int quality_tests() {
    int cases=0;
    std::uint64_t yadif_error=0,bob_error=0,woven_error=0;
    for(int depth:{8,10}) for(unsigned first:{0U,1U}) {
        const auto scene=[&](int time) {
            auto image=bars(0,depth);
            for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                // Static one-row detail beside a moving diagonal edge.
                const int value=x<width/2 ? (y%4==1 ? 220:35) : (x-y-time*2<width/2 ? 45:200);
                image[0][static_cast<std::size_t>(y*width+x)]=value<<(depth-8);
            }
            return image;
        };
        std::array<Image,3> images{weave(scene(0),scene(1),first),weave(scene(2),scene(3),first),weave(scene(4),scene(5),first)};
        std::array pixels{upload(images[0],depth),upload(images[1],depth),upload(images[2],depth)};
        GpuYadif gpu(pixels[1].get());
        for(unsigned second:{0U,1U}) {
            const auto field=first^second; const auto expected=scene(2+static_cast<int>(second));
            auto output=gpu.process(pixels[0].get(),pixels[1].get(),pixels[2].get(),field,second!=0);
            const auto actual=download(output.get(),depth);
            for(int y=3;y<height-3;++y) for(int x=3;x<width-3;++x) {
                const auto i=static_cast<std::size_t>(y*width+x);
                const int bob=(static_cast<unsigned>(y)&1U)==field ? images[1][0][i] :
                    (sample(images[1],0,x,y-1)+sample(images[1],0,x,y+1))/2;
                const auto squared=[&](int v) { const auto delta=static_cast<std::int64_t>(v-expected[0][i]); return static_cast<std::uint64_t>(delta*delta); };
                yadif_error+=squared(actual[0][i]); bob_error+=squared(bob); woven_error+=squared(images[1][0][i]);
            }
            ++cases;
        }
    }
    std::cout << "Synthetic static-line/moving-diagonal luma SSE: YADIF=" << yadif_error
              << " Bob=" << bob_error << " woven=" << woven_error << '\n';
    require(yadif_error < bob_error && yadif_error < woven_error,"YADIF did not preserve static detail/reduce motion combing");
    return cases;
}
int schedule_tests() {
    int cases=0;
    auto fixture=upload(pattern(0,8),8); auto format=describe(fixture.get());
    volatile std::sig_atomic_t cancelled=0;
    for(int depth:{8,10}) for(unsigned first:{0U,1U}) for(bool bob:{false,true}) for(int n:{1,2,3,11}) {
        std::vector<Picture> list;
        for(int i=0;i<n;++i) list.push_back({pattern(i,depth),{true,true,0,first ? FieldOrder::bff:FieldOrder::tff},1001+i*31});
        auto source=std::make_unique<Source>(list,depth); auto* input=source.get();
        FilterConfig config; config.yadif={true,bob};
        YadifFrames frames(std::move(source),format.get(),config,cancelled);
        int count=0; CMTime end=CMTimeMake(1,5);
        while(auto f=frames.next()) {
            const int index=count/(bob ? 2:1); const unsigned second=bob ? static_cast<unsigned>(count%2):0;
            std::array images{list[static_cast<std::size_t>(std::max(index-1,0))].image,list[static_cast<std::size_t>(index)].image,
                             list[static_cast<std::size_t>(std::min(index+1,n-1))].image};
            compare(download(f->pixel.get(),depth),reference(images,first^second,second!=0),"YADIF scheduler context");
            require(CMTimeCompare(end,f->pts)==0,"YADIF VFR gap/overlap");
            const auto interval=CMTimeMake(list[static_cast<std::size_t>(index)].ticks,bob ? 120000:60000);
            require(CMTimeCompare(interval,f->duration)==0,"YADIF inexact VFR duration");
            require(f->cadence.known && !f->cadence.interlaced && !f->cadence.repeat_fields,"Output cadence is not progressive");
            end=CMTimeAdd(end,f->duration); ++count;
        }
        require(count==n*(bob ? 2:1),"YADIF dropped/added frames");
        require(CMTimeCompare(end,input->end)==0,"YADIF changed final endpoint"); ++cases;
    }
    // Mixed progressive/interlaced, field-order changes and declared cuts.
    for(bool bob:{false,true}) {
        std::vector<Picture> list;
        for(int i=0;i<12;++i) list.push_back({pattern(i,8),{true,i%3!=0,0,i%2 ? FieldOrder::bff:FieldOrder::tff,false,i==7},1001});
        auto source=std::make_unique<Source>(list,8); FilterConfig config; config.yadif={true,bob};
        YadifFrames frames(std::move(source),format.get(),config,cancelled);
        int count=0;
        while(auto f=frames.next()) {
            const auto& pic=list[static_cast<std::size_t>(count/(bob ? 2:1))];
            auto expected=pic.image;
            if(pic.cadence.interlaced) {
                const unsigned second=bob ? static_cast<unsigned>(count%2):0;
                expected=reference({pic.image,pic.image,pic.image},(pic.cadence.first==FieldOrder::bff ? 1U:0U)^second,second!=0);
            }
            compare(download(f->pixel.get(),8),expected,"Mixed routing borrowed across boundary"); ++count;
        }
        require(count==12*(bob ? 2:1),"Mixed mode decimated true video"); ++cases;
    }
    // RFF 2+1 fields, arbitrary EOF parity; no five-to-four decimation.
    for(unsigned first:{0U,1U}) for(bool bob:{false,true}) for(int n=1;n<=7;++n) {
        std::vector<Picture> list;
        for(int i=0;i<n;++i) list.push_back({bars(i,8),{true,true,1,(first^(static_cast<unsigned>(i)&1U)) ? FieldOrder::bff:FieldOrder::tff},3003});
        auto source=std::make_unique<Source>(list,8); auto* input=source.get(); FilterConfig config; config.yadif={true,bob};
        YadifFrames frames(std::move(source),format.get(),config,cancelled);
        int count=0; auto end=CMTimeMake(1,5);
        while(auto f=frames.next()) { require(CMTimeCompare(f->pts,end)==0,"RFF gap/overlap"); end=CMTimeAdd(end,f->duration); ++count; }
        require(count==(bob ? 3*n:(3*n+1)/2),"RFF orphan doubled or video decimated");
        require(CMTimeCompare(end,input->end)==0,"RFF endpoint drift"); ++cases;
    }
    // RFF orphan must use only its retained field, not the other exposure.
    for(unsigned first:{0U,1U}) {
        const auto raster=weave(bars(0,8),bars(9,8),first);
        std::vector<Picture> list{{raster,{true,true,1,first ? FieldOrder::bff:FieldOrder::tff},3003}};
        FilterConfig config; config.yadif={true,true};
        YadifFrames frames(std::make_unique<Source>(list,8),format.get(),config,cancelled);
        int count=0;
        while(auto f=frames.next()) {
            if(count==2) compare(download(f->pixel.get(),8),reference({raster,raster,raster},first,false,true),"RFF orphan used opposite exposure");
            ++count;
        }
        require(count==3,"Single RFF raster did not retain three field exposures"); ++cases;
    }
    for(int repeat:{2,4}) for(bool bob:{false,true}) {
        std::vector<Picture> list{{pattern(0,10),{true,false,repeat,FieldOrder::tff},1001*(2+repeat)}};
        FilterConfig config; config.yadif={true,bob};
        YadifFrames frames(std::make_unique<Source>(list,10),format.get(),config,cancelled);
        int count=0; auto end=CMTimeMake(1,5);
        while(auto f=frames.next()) {
            require(CMTimeCompare(f->pts,end)==0,"Full-frame repetition gap"); end=CMTimeAdd(end,f->duration); ++count;
        }
        require(count==(2+repeat)*(bob ? 2:1)/2,"Full-frame repetition was dropped");
        require(CMTimeCompare(end,CMTimeAdd(CMTimeMake(1,5),CMTimeMake(1001*(2+repeat),60000)))==0,"Repeated frame endpoint drift"); ++cases;
    }
    // Packed HEVC fields, including orphan tails. Authoritative spatial parity
    // must survive a conflicting global temporal-order override.
    for(unsigned first:{0U,1U}) for(bool bob:{false,true}) for(int n=1;n<=7;++n) {
        std::vector<Picture> list;
        for(int i=0;i<n;++i) {
            const auto parity=first^(static_cast<unsigned>(i)&1U); auto image=bars(i,10); Image packed;
            for(std::size_t p=0;p<2;++p) for(std::size_t y=parity;y<image[p].size()/width;y+=2)
                packed[p].insert(packed[p].end(),image[p].begin()+static_cast<std::ptrdiff_t>(y*width),image[p].begin()+static_cast<std::ptrdiff_t>((y+1)*width));
            FrameCadence c{true,true,0,parity ? FieldOrder::bff:FieldOrder::tff}; c.single_field=true;
            list.push_back({packed,c,1001});
        }
        auto source=std::make_unique<Source>(list,10); auto* input=source.get(); FilterConfig config; config.yadif={true,bob};
        config.field_order=first ? FieldOrder::tff:FieldOrder::bff;
        YadifFrames frames(std::move(source),format.get(),config,cancelled);
        int count=0; auto end=CMTimeMake(1,5);
        while(auto f=frames.next()) {
            require(CVPixelBufferGetHeight(f->pixel.get())==height,"HEVC full-height assembly failed");
            require(CMTimeCompare(f->pts,end)==0,"HEVC field gap/overlap"); end=CMTimeAdd(end,f->duration); ++count;
        }
        require(count==(bob ? n:(n+1)/2),"HEVC orphan doubled or video decimated");
        require(CMTimeCompare(end,input->end)==0,"HEVC endpoint drift"); ++cases;
    }
    for(int scenario=0;scenario<3;++scenario) {
        Picture pic{pattern(0,8)};
        if(scenario==0) { pic.cadence.known=false; pic.cadence.first=FieldOrder::automatic; }
        if(scenario==2) pic.cadence.repeat_fields=3;
        auto source=std::make_unique<Source>(std::vector<Picture>{pic},8); source->omit_duration=scenario==1;
        FilterConfig config; config.yadif={true,true};
        YadifFrames frames(std::move(source),format.get(),config,cancelled);
        bool rejected=false;
        try { (void)frames.next(); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"YADIF guessed missing order/duration or accepted invalid RFF"); ++cases;
    }
    cancelled=1;
    FilterConfig config; config.yadif={true,true};
    YadifFrames stopped(std::make_unique<Source>(std::vector<Picture>{{pattern(0,8)}},8),format.get(),config,cancelled);
    require(!stopped.next(),"YADIF ignored cancellation"); ++cases;
    return cases;
}
}
int main() try {
    const int pixels=pixel_tests(),quality=quality_tests(),schedule=schedule_tests();
    std::cout << "Metal YADIF: " << pixels << " pixel/reference cases, " << quality << " quality cases, " << schedule << " cadence/timeline/error cases passed\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
