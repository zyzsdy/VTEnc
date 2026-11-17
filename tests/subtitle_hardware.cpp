// Explicit Apple Silicon check, never registered as an automatic CTest.
#include "subtitle.hpp"
#include "encoder_session.hpp"
#include "colorspace_reference.hpp"
#include <cstring>
#include <iostream>
using namespace vtenc;
using color_reference::V;
void require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
void check_gpu(int depth,bool full,ColorTransfer transfer,unsigned matrix=0) {
    const bool hdr=transfer==ColorTransfer::pq||transfer==ColorTransfer::hlg;
    const auto prim=hdr?ColorPrimaries::bt2020:ColorPrimaries::bt709;
    const double kr=hdr?.2627:.2126,kb=hdr?.0593:.0722;
    const int factor=depth==10?4:1, maximum=depth==10?1023:255;
    const int yoffset=full?0:16*factor, yscale=full?maximum:219*factor;
    const int coffset=128*factor, cscale=full?maximum:224*factor;
    auto pool=make_metal_pixel_pool(16,12,depth==10?(full?kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange):
        (full?kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange));
    CVPixelBufferRef raw=nullptr; check_status(CVPixelBufferPoolCreatePixelBuffer(nullptr,pool.get(),&raw),"Test canvas"); CFRef pixel(raw);
    check_status(CVPixelBufferLockBaseAddress(raw,0),"Lock fixture");
    for (unsigned plane=0;plane<2;++plane) {
        auto base=static_cast<unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(raw,plane)); auto stride=CVPixelBufferGetBytesPerRowOfPlane(raw,plane);
        for (unsigned y=0;y<(plane?6U:12U);++y) for (unsigned x=0;x<16;++x) {
            const int value=plane?coffset:yoffset;
            if (depth==10) reinterpret_cast<std::uint16_t*>(base+y*stride)[x]=static_cast<std::uint16_t>(value<<6);
            else base[y*stride+x]=static_cast<unsigned char>(value);
        }
    }
    check_status(CVPixelBufferUnlockBaseAddress(raw,0),"Unlock fixture");
    CVBufferSetAttachment(raw,kCVImageBufferColorPrimariesKey,CVColorPrimariesGetStringForIntegerCodePoint(static_cast<int>(prim)),kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(raw,kCVImageBufferTransferFunctionKey,CVTransferFunctionGetStringForIntegerCodePoint(static_cast<int>(transfer)),kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(raw,kCVImageBufferYCbCrMatrixKey,CVYCbCrMatrixGetStringForIntegerCodePoint(hdr?9:1),kCVAttachmentMode_ShouldPropagate);
    for (auto key:{kCVImageBufferChromaLocationTopFieldKey,kCVImageBufferChromaLocationBottomFieldKey})
        CVBufferSetAttachment(raw,key,kCVImageBufferChromaLocation_Left,kCVAttachmentMode_ShouldPropagate);
    GpuSubtitles gpu(raw);
    // Only the final row width is allocated, catching illegal stride*height reads.
    std::vector<unsigned char> mask(8*4+5,255),second(7*3+6,128),white(16,255);
    std::array<SubtitleBitmap,3> images{{{mask.data(),5,5,8,-1,2,0xff402000,0.8F,203,{0,0,16,12}},
        {second.data(),6,4,7,2,4,0x20c0ff40,0.65F,100,{3,5,5,4}},
        {white.data(),4,4,4,10,0,0xffffff00,1,203,{0,0,16,12}}}};
    images[0].color_matrix=matrix;
    std::array<V,192> linear{}; std::array<double,192> alpha{};
    for (const auto& image:images) for (int y=0;y<12;++y) for (int x=0;x<16;++x) {
        const auto& c=image.clip;
        if (x<image.x||y<image.y||x>=image.x+image.width||y>=image.y+image.height||x<c[0]||y<c[1]||x>=c[0]+c[2]||y>=c[1]+c[3]) continue;
        const auto index=static_cast<std::size_t>(y*16+x);
        double a=image.data[(y-image.y)*image.stride+x-image.x]/255.0*(1-(image.color&255)/255.0)*image.opacity;
        V rgb{double(image.color>>24)/255,double((image.color>>16)&255)/255,double((image.color>>8)&255)/255};
        if (!hdr && image.color_matrix) {
            const unsigned kind=(image.color_matrix-1)/2;
            const double sk=kind==0?.299:kind==1?.2126:kind==2?.212:.30;
            const double sb=kind==0?.114:kind==1?.0722:kind==2?.087:.11;
            const bool sf=image.color_matrix%2==0;
            double yy=sk*rgb[0]+(1-sk-sb)*rgb[1]+sb*rgb[2];
            double u=(rgb[2]-yy)/(2*(1-sb)),v=(rgb[0]-yy)/(2*(1-sk));
            yy=(yy*(sf?255:219)+(sf?0:16)-(full?0:16))/(full?255:219);
            u*=(sf?255.0:224)/(full?255:224); v*=(sf?255.0:224)/(full?255:224);
            rgb[0]=yy+2*(1-kr)*v; rgb[2]=yy+2*(1-kb)*u; rgb[1]=(yy-kr*rgb[0]-kb*rgb[2])/(1-kr-kb);
            for (auto& channel:rgb) channel=std::clamp(channel,0.0,1.0);
        }
        for (auto& v:rgb) v=color_reference::decode(v,hdr?ColorTransfer::srgb:transfer,false);
        if (hdr) rgb=color_reference::mul(color_reference::rgb(prim),color_reference::mul(color_reference::xyz(ColorPrimaries::bt709),rgb));
        for (unsigned i=0;i<3;++i) linear[index][i]=rgb[i]*(hdr?image.white_nits:1)*a+linear[index][i]*(1-a);
        alpha[index]=a+alpha[index]*(1-a);
    }
    const auto yuv=[&](int x,int y) {
        V v=linear[static_cast<std::size_t>(y*16+x)]; double scale=1;
        if (transfer==ColorTransfer::pq) scale=1.0/10000;
        if (transfer==ColorTransfer::hlg) scale=0.001*std::pow(std::max(color_reference::mul(color_reference::xyz(prim),v)[1]*.001,1.175494351e-38),-1.0/6);
        for (auto& c:v) c=color_reference::encode(c*scale,transfer,false);
        const double yy=kr*v[0]+(1-kr-kb)*v[1]+kb*v[2];
        return V{yy,(v[2]-yy)/(2*(1-kb)),(v[0]-yy)/(2*(1-kr))};
    };
    for (bool changed:{true,false}) {
        auto out=gpu.process(raw,images,changed); check_status(CVPixelBufferLockBaseAddress(out.get(),kCVPixelBufferLock_ReadOnly),"Read GPU output");
        for (unsigned plane=0;plane<2;++plane) {
            auto base=static_cast<const unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(out.get(),plane)); auto stride=CVPixelBufferGetBytesPerRowOfPlane(out.get(),plane);
            for (int y=0;y<(plane?6:12);++y) for (int x=0;x<16;++x) {
                double expected;
                if (!plane) expected=yuv(x,y)[0]*yscale+yoffset;
                else {
                    V sum{};
                    for (int dy=0;dy<2;++dy) for (int dx=-1;dx<=1;++dx) {
                        auto value=yuv(std::clamp((x/2)*2+dx,0,15),y*2+dy);
                        for (unsigned c=0;c<3;++c) sum[c]+=value[c]*(dx==0?.25:.125);
                    }
                    expected=sum[static_cast<unsigned>(x%2+1)]*cscale+coffset;
                }
                const int want=static_cast<int>(std::round(std::clamp(expected,0.0,double(maximum))));
                const int got=depth==10?reinterpret_cast<const std::uint16_t*>(base+static_cast<std::size_t>(y)*stride)[x]>>6:base[static_cast<std::size_t>(y)*stride+static_cast<std::size_t>(x)];
                if (std::abs(want-got)>1) throw std::runtime_error("Subtitle scalar mismatch want="+std::to_string(want)+" got="+std::to_string(got));
            }
        }
        check_status(CVPixelBufferUnlockBaseAddress(out.get(),kCVPixelBufferLock_ReadOnly),"Unlock GPU output");
    }
    auto blank=gpu.process(raw,{},true); require(blank.get()==raw,"Empty subtitle must preserve pixels");
    auto reappear=gpu.process(raw,images,false); require(reappear.get()!=raw,"Subtitle reappearance");
    std::cout << "PASS Metal " << depth << " full=" << full << " transfer=" << static_cast<int>(transfer) << " matrix=" << matrix << '\n';
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Supply ASS fixture path");
        SubtitleConfig c; c.filename=argv[1]; c.offset_ms=125;
        SubtitleRenderer renderer({c},320,180,1);
        require(renderer.render(CMTimeMake(3249,10000)).empty(),"Subtitle started before shifted boundary");
        require(!renderer.render(CMTimeMake(325,1000)).empty(),"Chinese ASS missing at exact start");
        require(renderer.changed(),"New overlay not dirty");
        require(!renderer.render(CMTimeMake(326,1000)).empty()&&!renderer.changed(),"Static overlay not cached");
        require(renderer.render(CMTimeMake(825,1000)).empty(),"Subtitle visible at exclusive end");
        c.filename=std::string(argv[1]).substr(0,std::string(argv[1]).size()-3)+"ssa";
        SubtitleRenderer ssa({c},320,180,1);
        require(!ssa.render(CMTimeMake(325,1000)).empty(),"SSA Chinese rendering");
        c.filename=argv[1];
        c.offset_ms=-125; SubtitleRenderer early({c},320,180,1.5);
        require(!early.render(CMTimeMake(75,1000)).empty(),"Negative offset/SAR");
        for (int depth:{8,10}) for (bool full:{false,true}) for (auto t:{ColorTransfer::bt709,ColorTransfer::pq,ColorTransfer::hlg}) check_gpu(depth,full,t);
        for (unsigned m=1;m<=8;++m) { check_gpu(8,false,ColorTransfer::bt709,m); check_gpu(10,true,ColorTransfer::bt709,m); }
        std::cout << "PASS libass Chinese, rational boundaries, offsets, cache, Metal scalar reference\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
