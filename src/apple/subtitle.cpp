// libass API workflow inspired by NVEncFilterSubburn.cpp (MIT); see notices.
#include "subtitle.hpp"
#include <ass/ass.h>
#if LIBASS_VERSION < 0x01700000
#error "VTEnc requires libass 0.17 or newer"
#endif
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstdarg>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace vtenc {
namespace {
std::vector<char> read_file(const std::filesystem::path& path, std::size_t limit) {
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Subtitle/font is not a regular file: " + path.string());
    const auto size=std::filesystem::file_size(path);
    if (!size || size>limit) throw std::runtime_error("Empty or oversized subtitle/font: " + path.string());
    std::vector<char> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path,std::ios::binary);
    if (!file.read(bytes.data(),static_cast<std::streamsize>(size))) throw std::runtime_error("Read subtitle/font: " + path.string());
    return bytes;
}
struct LibraryDeleter { void operator()(ASS_Library* p) const { ass_library_done(p); } };
struct RendererDeleter { void operator()(ASS_Renderer* p) const { ass_renderer_done(p); } };
struct TrackDeleter { void operator()(ASS_Track* p) const { ass_free_track(p); } };
// CoreText may write NSLog directly to fd 2 (e.g. LastResort lookup), bypassing
// libass's callback. Capture only the synchronous font call, then route those
// diagnostics through std::cerr / the existing JSON DiagnosticBuffer. Decode,
// GPU and encode work is drained before this scheduler-only scope.
class NativeFontLog {
public:
    NativeFontLog() { file_=std::tmpfile(); if (!file_) fail("Create native font diagnostic buffer"); }
    ~NativeFontLog() {
        if (saved_>=0) { if (dup2(saved_,STDERR_FILENO)<0) std::terminate(); close(saved_); }
        if (file_) std::fclose(file_);
    }
    void begin() {
        if (saved_>=0) throw std::runtime_error("Nested native font diagnostic capture");
        if (std::fflush(stderr)!=0 || std::fflush(file_)!=0 || ftruncate(fileno(file_),0)!=0 || std::fseek(file_,0,SEEK_SET)!=0)
            fail("Reset native font diagnostics");
        saved_=dup(STDERR_FILENO); if (saved_<0) fail("Duplicate font diagnostic descriptor");
        if (dup2(fileno(file_),STDERR_FILENO)<0) fail("Capture native font diagnostics");
    }
    void finish() {
        const int flush=std::fflush(stderr);
        if (dup2(saved_,STDERR_FILENO)<0) fail("Restore native font diagnostics");
        close(saved_); saved_=-1;
        if (flush!=0 || std::fseek(file_,0,SEEK_SET)!=0) fail("Read native font diagnostics");
        std::string message; char buffer[4096];
        while (const auto size=std::fread(buffer,1,sizeof(buffer),file_)) {
            message.append(buffer,size);
            if (message.size()>1024*1024) throw std::runtime_error("Native font diagnostics exceed 1 MiB per render");
        }
        if (std::ferror(file_)) fail("Read native font diagnostic buffer");
        if (!message.empty()) std::cerr << "vtenc: native font diagnostic: " << message << (message.back()=='\n'?"":"\n");
    }
private:
    [[noreturn]] static void fail(const char* operation) { throw std::runtime_error(std::string(operation)+": "+std::strerror(errno)); }
    FILE* file_=nullptr;
    int saved_=-1;
};
struct Track {
    SubtitleConfig config;
    std::exception_ptr error;
    std::size_t font_bytes=0;
    std::vector<std::string> messages;
    NativeFontLog native;
    std::unique_ptr<ASS_Library,LibraryDeleter> library;
    std::unique_ptr<ASS_Renderer,RendererDeleter> renderer;
    std::unique_ptr<ASS_Track,TrackDeleter> track;
    static void log(int level, const char* format, va_list args, void* ctx) noexcept {
        auto& self=*static_cast<Track*>(ctx);
        try {
            char text[4096]; const auto n=vsnprintf(text,sizeof(text),format,args);
            if (n<0) throw std::runtime_error("libass diagnostic formatting failed");
            const std::string_view message(text);
            if (level<=2 || message.find("fontselect")!=std::string_view::npos || message.find("Glyph")!=std::string_view::npos)
            {
                if (self.messages.size()>=4096) throw std::runtime_error("Excessive libass diagnostics");
                self.messages.push_back("vtenc: libass ["+self.config.filename+"]: "+std::string(message));
            }
            if (level<=1 || message.find("failed to find any fallback")!=std::string_view::npos ||
                message.find("Error opening font")!=std::string_view::npos)
                throw std::runtime_error("libass ["+self.config.filename+"]: "+std::string(message));
        } catch (...) { if (!self.error) self.error=std::current_exception(); }
    }
    void check() {
        for (const auto& message:messages) std::cerr << message << '\n';
        messages.clear();
        if (error) std::rethrow_exception(error);
    }
    void font(const std::filesystem::path& path) {
        auto bytes=read_file(path,64*1024*1024);
        font_bytes+=bytes.size();
        if (font_bytes>256*1024*1024) throw std::runtime_error("Explicit fonts exceed 256 MiB per subtitle track");
        FT_Library ft=nullptr;
        if (const auto code=FT_Init_FreeType(&ft)) throw std::runtime_error("Initialize FreeType: "+std::to_string(code));
        FT_Face face=nullptr;
        const auto code=FT_New_Memory_Face(ft,reinterpret_cast<const FT_Byte*>(bytes.data()),static_cast<FT_Long>(bytes.size()),0,&face);
        if (face) FT_Done_Face(face);
        FT_Done_FreeType(ft);
        if (code) throw std::runtime_error("Invalid font ["+path.string()+"], FreeType="+std::to_string(code));
        ass_add_font(library.get(),path.filename().c_str(),bytes.data(),static_cast<int>(bytes.size()));
        check();
        std::cerr << "vtenc: libass loaded font: " << path.string() << '\n';
    }
    Track(SubtitleConfig c,int width,int height,double sar):config(std::move(c)),library(ass_library_init()) {
        if (!library) throw std::runtime_error("Initialize libass library");
        ass_set_message_cb(library.get(),log,this);
        ASS_DefaultFontProvider* providers=nullptr; size_t count=0;
        ass_get_available_font_providers(library.get(),&providers,&count);
        bool coretext=false; for (size_t i=0;i<count;++i) coretext|=providers[i]==ASS_FONTPROVIDER_CORETEXT;
        free(providers);
        if (!coretext) throw std::runtime_error("libass was built without the required CoreText font provider");
        // Only explicitly supplied fonts are added; embedded font extraction is out of scope.
        ass_set_extract_fonts(library.get(),0);
        if (!config.fontsdir.empty()) {
            if (!std::filesystem::is_directory(config.fontsdir)) throw std::runtime_error("Invalid fontsdir: "+config.fontsdir);
            std::vector<std::filesystem::path> paths;
            for (const auto& f:std::filesystem::directory_iterator(config.fontsdir)) {
                auto ext=f.path().extension().string();
                std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char x){return static_cast<char>(std::tolower(x));});
                if (ext==".ttf"||ext==".otf"||ext==".ttc"||ext==".otc") paths.push_back(f.path());
            }
            if (paths.empty()||paths.size()>256) throw std::runtime_error("fontsdir must contain 1..256 TTF/OTF/TTC/OTC files");
            std::sort(paths.begin(),paths.end()); for (const auto& path:paths) font(path);
        }
        if (!config.fontfile.empty()) font(config.fontfile);
        renderer.reset(ass_renderer_init(library.get()));
        if (!renderer) throw std::runtime_error("Initialize libass renderer");
        ass_set_frame_size(renderer.get(),width,height);
        ass_set_pixel_aspect(renderer.get(),sar);
        native.begin();
        ass_set_fonts(renderer.get(),nullptr,config.font.c_str(),ASS_FONTPROVIDER_CORETEXT,nullptr,1);
        native.finish();
        ass_set_cache_limits(renderer.get(),10000,64);
        auto bytes=read_file(config.filename,64*1024*1024);
        track.reset(ass_read_memory(library.get(),bytes.data(),bytes.size(),"UTF-8"));
        check();
        if (!track||track->track_type==ASS_Track::TRACK_TYPE_UNKNOWN||track->n_events==0)
            throw std::runtime_error("Subtitle must contain ASS/SSA events: "+config.filename);
        if (track->YCbCrMatrix==YCBCR_UNKNOWN) throw std::runtime_error("Unrecognized ASS YCbCr Matrix: "+config.filename);
        std::cerr << "vtenc: subtitle fallback family=" << config.font << "; CoreText selections are logged [" << config.filename << "]\n";
    }
};
}
struct SubtitleRenderer::Impl {
    std::vector<std::unique_ptr<Track>> tracks;
    std::vector<SubtitleBitmap> images;
    int width,height;
    bool changed=true,first=true;
    Impl(const std::vector<SubtitleConfig>& configs,int w,int h,double sar):width(w),height(h) {
        if (w<=0||h<=0||w>32768||h>32768||!(sar>0)) throw std::runtime_error("Invalid subtitle dimensions/SAR");
        for (const auto& c:configs) tracks.push_back(std::make_unique<Track>(c,w,h,sar));
    }
};
SubtitleRenderer::SubtitleRenderer(const std::vector<SubtitleConfig>& c,int w,int h,double sar):impl_(std::make_unique<Impl>(c,w,h,sar)) {}
SubtitleRenderer::~SubtitleRenderer()=default;
bool SubtitleRenderer::changed() const { return impl_->changed; }
std::span<const SubtitleBitmap> SubtitleRenderer::render(CMTime pts) {
    auto& s=*impl_;
    if (!CMTIME_IS_NUMERIC(pts)||pts.epoch||CMTIME_HAS_BEEN_ROUNDED(pts)) throw std::runtime_error("Invalid subtitle PTS");
    s.images.clear(); s.changed=s.first; s.first=false;
    for (auto& t:s.tracks) {
        // Subtract exact rational offset before flooring; boundaries never round into a future event.
        const auto time=CMTimeConvertScale(CMTimeSubtract(pts,CMTimeMake(t->config.offset_ms,1000)),1000,kCMTimeRoundingMethod_RoundTowardNegativeInfinity);
        if (!CMTIME_IS_NUMERIC(time)) throw std::runtime_error("Subtitle timestamp overflow");
        int changed=0;
        t->native.begin();
        auto* image=ass_render_frame(t->renderer.get(),t->track.get(),time.value,&changed);
        t->native.finish();
        t->check(); s.changed|=changed!=0;
        for (;image;image=image->next) {
            if (image->w<=0||image->h<=0) continue;
            if (!image->bitmap||image->stride<image->w||image->w>s.width||image->h>s.height||s.images.size()>=65536)
                throw std::runtime_error("Invalid or excessive libass bitmap layout");
            s.images.push_back({image->bitmap,image->w,image->h,image->stride,image->dst_x,image->dst_y,image->color,
                t->config.opacity,t->config.white_nits,t->config.clip.value_or(std::array<int,4>{0,0,s.width,s.height}),
                t->track->YCbCrMatrix==YCBCR_NONE ? 0U : t->track->YCbCrMatrix==YCBCR_DEFAULT ? 1U :
                static_cast<unsigned>(t->track->YCbCrMatrix)-2U});
        }
    }
    return s.images;
}
}
