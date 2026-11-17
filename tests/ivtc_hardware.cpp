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
    auto pool = make_metal_pixel_pool(width, static_cast<int>(image[0].size() / width), type);
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
struct Picture {
    int top, bottom, repeat = 0;
    unsigned first = 0;
    bool interlaced = false;
};
class Pictures final : public DecodedFrames {
public:
    Pictures(std::vector<Picture> list, int depth, bool cfr=false) : list_(std::move(list)), depth_(depth), cfr_(cfr) {}
    std::unique_ptr<DecodedFrame> next() override {
        if (index_ == list_.size()) return nullptr;
        const auto& picture = list_[index_++];
        auto image = film(picture.top,depth_); const auto bottom = film(picture.bottom,depth_);
        for (int p=0;p<2;++p) for (int y=1;y<height/(p+1);y+=2)
            std::copy_n(bottom[static_cast<std::size_t>(p)].begin()+y*width,width,image[static_cast<std::size_t>(p)].begin()+y*width);
        auto pixel=upload(image,depth_);
        const auto duration=CMTimeMake(cfr_ ? 2500 : 1001*(2+picture.repeat),60000);
        const FrameCadence cadence{known,picture.interlaced,picture.repeat,picture.first ? FieldOrder::bff : FieldOrder::tff};
        auto frame=std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel),pts_,
            omit_final_duration && index_==list_.size() ? kCMTimeInvalid : duration,cadence});
        pts_=CMTimeAdd(pts_,duration); return frame;
    }
    CMTime pts_=CMTimeMake(1,5);
    bool known=true, omit_final_duration=false;
private:
    std::vector<Picture> list_; int depth_; bool cfr_; std::size_t index_=0;
};
std::vector<Picture> soft_pictures(int count,unsigned first) {
    std::vector<Picture> result;
    for(int i=0;i<count;++i) {
        result.push_back({i,i,i%2==0 ? 1:0,first,false});
        if(i%2==0) first^=1;
    }
    return result;
}
int rff_tests() {
    int cases=0;
    for(int depth : {8,10}) for(unsigned first : {0U,1U}) {
        FilterConfig config; config.ivtc.enabled=true;
        volatile std::sig_atomic_t cancelled=0;
        RffFrames expanded(std::make_unique<Pictures>(soft_pictures(4,first),depth),config,cancelled);
        // Independent field-list oracle for three/two/three/two display fields.
        const int source_id[]={0,0,0,1,1,2,2,2,3,3};
        int n=0;
        while(auto frame=expanded.next()) {
            const int a=source_id[n*2], b=source_id[n*2+1];
            auto expected=film(first ? b:a,depth), other=film(first ? a:b,depth);
            for(int p=0;p<2;++p) for(int y=1;y<height/(p+1);y+=2)
                std::copy_n(other[static_cast<std::size_t>(p)].begin()+y*width,width,expected[static_cast<std::size_t>(p)].begin()+y*width);
            require(download(frame->pixel.get(),depth)==expected,"RFF expanded field pair differs from display field list");
            require(CMTimeCompare(frame->pts,CMTimeAdd(CMTimeMake(1,5),CMTimeMake(n*1001,30000)))==0,"RFF expansion PTS incorrect");
            require(CMTimeCompare(frame->duration,CMTimeMake(1001,30000))==0,"RFF expanded duration incorrect");
            ++n;
        }
        require(n==5,"Four coded RFF pictures must expand to five display rasters"); ++cases;
        for(bool mixed : {false,true}) for(bool cfr : {false,true}) {
            auto fixture=upload(film(0,depth),depth); CMVideoFormatDescriptionRef raw=nullptr;
            check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,fixture.get(),&raw),"Describe soft telecine"); CFRef format(raw);
            config.ivtc.mixed=mixed;
            // Explicit hard-raster interpretation must not overwrite alternating
            // bitstream first-field flags used by the RFF expansion stage.
            config.field_order=first ? FieldOrder::bff : FieldOrder::tff;
            IvtcFrames frames(std::make_unique<Pictures>(soft_pictures(16,first),depth,cfr),format.get(),config,cancelled);
            int count=0;
            while(auto frame=frames.next()) {
                require(download(frame->pixel.get(),depth)==film(count,depth),"Soft IVTC lost/repeated a coded movie picture");
                const auto period=CMTimeMake(cfr ? 2500:25025,cfr ? 60000:600000);
                require(CMTimeCompare(frame->duration,period)==0,"Soft IVTC output is not expected CFR");
                require(CMTimeCompare(frame->pts,CMTimeAdd(CMTimeMake(1,5),CMTimeMultiply(period,count)))==0,"Soft IVTC PTS drift");
                ++count;
            }
            require(count==16,"Soft IVTC decimated already coded film twice"); ++cases;
        }
    }
    // Unmatched final field, full-frame repetition, mixed transition and VFR.
    for(int count=1;count<=7;++count) {
        auto source=std::make_unique<Pictures>(soft_pictures(count,0),8); auto* input=source.get();
        FilterConfig config; config.ivtc.enabled=true; volatile std::sig_atomic_t cancelled=0;
        RffFrames frames(std::move(source),config,cancelled);
        auto end=CMTimeMake(1,5); int fields=0;
        while(auto frame=frames.next()) {
            require(CMTimeCompare(end,frame->pts)==0,"RFF tail gap/overlap");
            end=CMTimeAdd(frame->pts,frame->duration); ++fields;
        }
        require(CMTimeCompare(end,input->pts_)==0,"RFF tail truncated display duration");
        require(fields==((count*2+(count+1)/2)+1)/2,"RFF wrong partial display count"); ++cases;
    }
    for(int repeat : {2,4}) {
        FilterConfig config; config.ivtc.enabled=true; volatile std::sig_atomic_t cancelled=0;
        RffFrames frames(std::make_unique<Pictures>(std::vector<Picture>{{0,0,repeat,0,false}},10),config,cancelled);
        int count=0; while(auto frame=frames.next()) { require(download(frame->pixel.get(),10)==film(0,10),"Full-frame repeat corrupted pixels"); ++count; }
        require(count==1+repeat/2,"Wrong full-frame repetition expansion"); ++cases;
    }
    // Shift the progressive/soft/hard transitions through every cycle offset.
    for(int depth : {8,10}) for(unsigned first : {0U,1U}) for(int prefix=1;prefix<=5;++prefix) {
        std::vector<Picture> list;
        for(int i=0;i<prefix;++i) list.push_back({i,i,0,first,false});
        auto soft=soft_pictures(8,first);
        for(auto p:soft) { p.top+=prefix; p.bottom+=prefix; list.push_back(p); }
        constexpr int a[]={0,1,1,2,3},b[]={0,1,2,3,3};
        for(int i=0;i<10;++i) {
            const int top=prefix+8+i/5*4+a[i%5],bottom=prefix+8+i/5*4+b[i%5];
            list.push_back({first ? bottom:top,first ? top:bottom,0,first,true});
        }
        for(int i=prefix+16;i<prefix+21;++i) list.push_back({i,i,0,first,false});
        auto source=std::make_unique<Pictures>(list,depth); auto* input=source.get();
        auto fixture=upload(film(0,depth),depth); CMVideoFormatDescriptionRef raw=nullptr;
        check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,fixture.get(),&raw),"Describe mixed telecine"); CFRef format(raw);
        FilterConfig config; config.ivtc.enabled=true; config.ivtc.mixed=true; config.field_order=first ? FieldOrder::bff:FieldOrder::tff;
        volatile std::sig_atomic_t cancelled=0; IvtcFrames frames(std::move(source),format.get(),config,cancelled);
        auto end=CMTimeMake(1,5); int count=0;
        while(auto frame=frames.next()) {
            require(download(frame->pixel.get(),depth)==film(count,depth),"Mixed soft/hard/progressive route lost movie order");
            require(CMTimeCompare(end,frame->pts)==0,"Mixed route gap/overlap");
            end=CMTimeAdd(frame->pts,frame->duration); ++count;
        }
        require(count==prefix+21,"Mixed mode decimated progressive video or retained telecine duplicates");
        require(CMTimeCompare(end,input->pts_)==0,"Mixed mode changed movie end"); ++cases;
    }
    auto fixture=upload(film(0,8),8); CMVideoFormatDescriptionRef raw=nullptr;
    check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,fixture.get(),&raw),"Describe RFF errors"); CFRef format(raw);
    for(int scenario=0;scenario<4;++scenario) {
        auto source=std::make_unique<Pictures>(std::vector<Picture>{{0,0,scenario==0 ? 3:1,0,false}},8);
        source->known=scenario!=1;
        source->omit_final_duration=scenario==2;
        FilterConfig config; config.ivtc.enabled=true; config.ivtc.mixed=scenario!=3;
        if(scenario==3) config.ivtc.expand=RffExpansion::off;
        volatile std::sig_atomic_t cancelled=0;
        bool rejected=false;
        try { IvtcFrames frames(std::move(source),format.get(),config,cancelled); while(frames.next()) {} }
        catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"IVTC accepted invalid repeat/unknown metadata/missing duration/disabled RFF"); ++cases;
    }
    // True interlaced video contains two different exposures in every raster.
    // Residual comb evidence must inhibit five-to-four decimation.
    std::vector<Picture> video;
    for(int i=0;i<10;++i) video.push_back({i*2,i*2+1,0,0,true});
    FilterConfig config; config.ivtc.enabled=true; config.ivtc.mixed=true;
    volatile std::sig_atomic_t cancelled=0;
    IvtcFrames frames(std::make_unique<Pictures>(video,8),format.get(),config,cancelled);
    int count=0; while(frames.next()) ++count;
    require(count==10,"Mixed IVTC decimated true interlaced video"); ++cases;
    // A discontinuous first-field order flushes the pending field's half-frame
    // interval, marks both sides, and never borrows across the discontinuity.
    RffFrames boundary(std::make_unique<Pictures>(std::vector<Picture>{{0,0,1,0,false},{1,2,0,0,true}},8),config,cancelled);
    auto end=CMTimeMake(1,5); count=0;
    while(auto frame=boundary.next()) {
        require(CMTimeCompare(frame->pts,end)==0,"RFF field discontinuity changed PTS");
        require(count==0 || frame->cadence.boundary,"RFF field discontinuity lacks boundary marker");
        end=CMTimeAdd(end,frame->duration); ++count;
    }
    require(count==3 && CMTimeCompare(end,CMTimeAdd(CMTimeMake(1,5),CMTimeMake(5005,60000)))==0,
        "RFF field discontinuity lost display time"); ++cases;
    std::vector<Picture> parity_change;
    constexpr int top_ids[]={0,1,1,2,3}, bottom_ids[]={0,1,2,3,3};
    for(int i=0;i<20;++i) {
        const int a=i/5*4+top_ids[i%5], b=i/5*4+bottom_ids[i%5];
        parity_change.push_back({i<10 ? a:b,i<10 ? b:a,0,i<10 ? 0U:1U,true});
    }
    IvtcFrames switched(std::make_unique<Pictures>(parity_change,8),format.get(),config,cancelled);
    count=0;
    while(auto frame=switched.next()) {
        require(download(frame->pixel.get(),8)==film(count,8),"Mixed field-order switch lost a film exposure");
        require(CMTimeCompare(frame->pts,CMTimeAdd(CMTimeMake(1,5),CMTimeMake(count*1001,24000)))==0,
            "Mixed field-order switch changed timeline"); ++count;
    }
    require(count==16,"Mixed field-order switch did not drain/reset correctly"); ++cases;
    return cases;
}

Image packed(const Image& input, unsigned parity) {
    Image result;
    for (std::size_t p=0;p<2;++p)
        for (std::size_t row=parity;row<input[p].size()/width;row+=2)
            result[p].insert(result[p].end(),input[p].begin()+static_cast<std::ptrdiff_t>(row*width),
                            input[p].begin()+static_cast<std::ptrdiff_t>((row+1)*width));
    return result;
}
class FieldPictures final : public DecodedFrames {
public:
    int count=40, index=0, depth=8;
    unsigned first=0;
    bool hints=true, trailing_hints=false, vfr=false, broken=false, cut_in=false, omit_duration=false;
    CMTime pts=CMTimeMake(1,5);
    std::unique_ptr<DecodedFrame> next() override {
        if(index==count) return nullptr;
        const auto identity=[](int i) { constexpr int ids[]={0,0,0,1,1,2,2,2,3,3}; return i/10*4+ids[i%10]; };
        const auto n=index+(cut_in ? 2:0);
        const auto parity=first ^ static_cast<unsigned>(broken ? index/3%2 : index%2);
        FrameCadence cadence{true,true,0,parity ? FieldOrder::bff:FieldOrder::tff};
        cadence.single_field=true;
        if(hints) cadence.pairing=n>0 && identity(n)==identity(n-1) ? FieldPairing::previous:
            identity(n)==identity(n+1) ? FieldPairing::next:FieldPairing::unspecified;
        if(trailing_hints && index%2==0) cadence.pairing=FieldPairing::unspecified;
        cadence.duplicate=n>=2 && identity(n)==identity(n-2);
        auto pixel=upload(packed(film(identity(n),depth),parity),depth);
        const auto duration=CMTimeMake(vfr ? 900+index%4*101 : 1001,60000);
        auto output=std::make_unique<DecodedFrame>(DecodedFrame{std::move(pixel),pts,
            omit_duration && index==count-1 ? kCMTimeInvalid:duration,cadence});
        pts=CMTimeAdd(pts,duration); ++index; return output;
    }
};
int field_tests() {
    int cases=0;
    for(int depth:{8,10}) for(bool full:{false,true}) for(unsigned first:{0U,1U}) {
        const auto a=pattern(0,depth), b=pattern(1,depth);
        auto pa=upload(packed(a,first),depth,full), pb=upload(packed(b,first^1),depth,full);
        GpuFieldPair gpu(pa.get());
        auto output=gpu.process(pa.get(),pb.get(),first);
        auto expected=a;
        for(int p=0;p<2;++p) for(int y=0;y<height/(p+1);++y) if((static_cast<unsigned>(y)&1U)!=first)
            std::copy_n(b[static_cast<std::size_t>(p)].begin()+y*width,width,expected[static_cast<std::size_t>(p)].begin()+y*width);
        require(download(output.get(),depth)==expected,"Packed field Y/UV weave differs from original rows"); ++cases;
        auto orphan=gpu.process(pa.get(),nullptr,first);
        auto field=packed(a,first); expected=a;
        for(int p=0;p<2;++p) for(int y=0;y<height/(p+1);++y) if((static_cast<unsigned>(y)&1U)!=first) {
            const int above=static_cast<int>(first) ? y/2-1:y/2;
            const int rows=height/(p+1)/2;
            for(int x=0;x<width;++x) expected[static_cast<std::size_t>(p)][static_cast<std::size_t>(y*width+x)]=
                (field[static_cast<std::size_t>(p)][static_cast<std::size_t>(std::clamp(above,0,rows-1)*width+x)]+
                 field[static_cast<std::size_t>(p)][static_cast<std::size_t>(std::clamp(above+1,0,rows-1)*width+x)]+1)/2;
        }
        require(download(orphan.get(),depth)==expected,"Orphan field interpolation differs from scalar result"); ++cases;
    }
    auto fixture=upload(film(0,8),8); CMVideoFormatDescriptionRef raw=nullptr;
    check_status(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,fixture.get(),&raw),"Describe field fixture"); CFRef format(raw);
    volatile std::sig_atomic_t cancelled=0;
    for(int depth:{8,10}) for(unsigned first:{0U,1U}) for(bool hints:{false,true}) for(bool expand:{false,true}) {
        auto source=std::make_unique<FieldPictures>(); source->depth=depth; source->first=first; source->hints=hints;
        FilterConfig config; config.ivtc.enabled=true; config.ivtc.mixed=true;
        config.ivtc.expand=expand ? RffExpansion::on:RffExpansion::off;
        // User's temporal-order override must not reinterpret HEVC spatial parity.
        config.field_order=first ? FieldOrder::tff:FieldOrder::bff;
        IvtcFrames frames(std::move(source),format.get(),config,cancelled);
        int count=0;
        while(auto frame=frames.next()) {
            require(download(frame->pixel.get(),depth)==film(count,depth),"Single-field IVTC lost a film exposure");
            require(CMTimeCompare(frame->pts,CMTimeAdd(CMTimeMake(1,5),CMTimeMake(count*1001,24000)))==0,"Single-field IVTC PTS drift");
            require(CMTimeCompare(frame->duration,CMTimeMake(1001,24000))==0,"Single-field IVTC duration drift"); ++count;
        }
        require(count==16,"Duplicate single fields were expanded twice or not decimated"); ++cases;
    }
    for(int count=1;count<=13;++count) for(bool broken:{false,true}) for(bool cut:{false,true}) {
        auto source=std::make_unique<FieldPictures>(); auto* input=source.get();
        source->count=count; source->vfr=true; source->broken=broken; source->cut_in=cut; source->hints=!broken;
        FilterConfig config; config.ivtc.enabled=true; config.ivtc.mixed=!broken; config.ivtc.cycle=0;
        IvtcFrames frames(std::move(source),format.get(),config,cancelled);
        CMTime end=CMTimeMake(1,5); int emitted=0;
        while(auto frame=frames.next()) {
            require(CMTimeCompare(frame->pts,end)==0,"Single-field VFR gap/overlap");
            require(CVPixelBufferGetHeight(frame->pixel.get())==height,"Orphan field did not restore full height");
            end=CMTimeAdd(end,frame->duration); ++emitted;
        }
        require(CMTimeCompare(end,input->pts)==0,"Single-field tail lost time");
        require(emitted>0,"Single-field stream yielded no images"); ++cases;
    }
    for(unsigned first:{0U,1U}) {
        auto source=std::make_unique<FieldPictures>(); source->count=10; source->first=first; source->trailing_hints=true;
        FilterConfig config; config.ivtc.enabled=true;
        FieldFrames paired(std::move(source),config,cancelled);
        constexpr FieldPairing hints[]={FieldPairing::next,FieldPairing::unspecified,FieldPairing::previous,FieldPairing::previous,FieldPairing::next};
        int count=0;
        while(auto frame=paired.next()) {
            require(count<5 && frame->cadence.pairing==hints[count],"Complementary field pairing hint was discarded"); ++count;
        }
        require(count==5,"Complementary hint changed field intervals"); ++cases;
    }
    auto source=std::make_unique<FieldPictures>(); source->count=1; source->omit_duration=true;
    FilterConfig config; config.ivtc.enabled=true;
    FieldFrames frames(std::move(source),config,cancelled);
    bool rejected=false;
    try { (void)frames.next(); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"Missing single-field tail duration was guessed"); ++cases;
    return cases;
}

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
    cases += rff_tests();
    cases += field_tests();
    std::cout << "Metal IVTC: " << cases << " reconstruction, cadence and timeline cases passed\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
