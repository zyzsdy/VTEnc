// Original Metal compositor. libass supplies masks only; all pixel blending is on GPU.
struct SubtitleParameters {
    float storage, y_offset, y_scale, c_offset, c_scale, output_storage, maximum;
    float kr, kb;
    uint transfer;
    float luma[3], gamut[9];
};
struct GlyphParameters {
    int x,y,width,height;
    uint stride,color;
    float opacity,white;
    int clip_x,clip_y,clip_w,clip_h;
    uint matrix;
};
kernel void subtitle_clear(texture2d<float,access::write> overlay [[texture(0)]],uint2 p [[thread_position_in_grid]]) {
    if (p.x<overlay.get_width()&&p.y<overlay.get_height()) overlay.write(float4(0),p);
}
kernel void subtitle_glyph(texture2d<float,access::read_write> overlay [[texture(0)]],
    device const uchar* mask [[buffer(0)]],constant GlyphParameters& g [[buffer(1)]],
    constant SubtitleParameters& p [[buffer(2)]],uint2 index [[thread_position_in_grid]]) {
    if (index.x>=uint(g.width)||index.y>=uint(g.height)) return;
    int2 pos=int2(index)+int2(g.x,g.y);
    if (any(pos<0)||pos.x>=int(overlay.get_width())||pos.y>=int(overlay.get_height())||
        pos.x<g.clip_x||pos.y<g.clip_y||pos.x>=g.clip_x+g.clip_w||pos.y>=g.clip_y+g.clip_h) return;
    float alpha=float(mask[index.y*g.stride+index.x])/255.0f*(1.0f-float(g.color&255)/255.0f)*g.opacity;
    float3 rgb=float3((g.color>>24)&255,(g.color>>16)&255,(g.color>>8)&255)/255.0f;
    const bool hdr=p.transfer==16||p.transfer==18;
    if (!hdr && g.matrix) {
        // libass documents the VSFilter compatibility transform. Header RGB
        // -> header YCbCr code values -> actual video RGB; None bypasses it.
        const uint kind=(g.matrix-1)/2;
        float kr=kind==0?0.299f:kind==1?0.2126f:kind==2?0.212f:0.30f;
        float kb=kind==0?0.114f:kind==1?0.0722f:kind==2?0.087f:0.11f;
        const bool full=(g.matrix%2)==0;
        float yy=dot(rgb,float3(kr,1-kr-kb,kb));
        float2 cc=float2((rgb.b-yy)/(2*(1-kb)),(rgb.r-yy)/(2*(1-kr)));
        const bool video_full=p.y_offset==0;
        yy=(yy*(full?255.0f:219.0f)+(full?0.0f:16.0f)-(video_full?0.0f:16.0f))/(video_full?255.0f:219.0f);
        cc*=float(full?255:224)/float(video_full?255:224);
        float r=yy+2*(1-p.kr)*cc.y,b=yy+2*(1-p.kb)*cc.x;
        rgb=clamp(float3(r,(yy-p.kr*r-p.kb*b)/(1-p.kr-p.kb),b),0.0f,1.0f);
    }
    rgb=to_linear(rgb,hdr?13:p.transfer,false,1.0f,float3(p.luma[0],p.luma[1],p.luma[2]));
    if (hdr) {
        rgb=float3(dot(rgb,float3(p.gamut[0],p.gamut[1],p.gamut[2])),
                   dot(rgb,float3(p.gamut[3],p.gamut[4],p.gamut[5])),
                   dot(rgb,float3(p.gamut[6],p.gamut[7],p.gamut[8])))*g.white;
    }
    float4 old=overlay.read(uint2(pos));
    overlay.write(float4(rgb*alpha+old.rgb*(1-alpha),alpha+old.a*(1-alpha)),uint2(pos));
}
float3 subtitle_pixel(texture2d<float,access::read> y,texture2d<float,access::read> uv,
    texture2d<float,access::read> overlay,uint2 index,constant SubtitleParameters& p) {
    float yy=(y.read(index).r*p.storage-p.y_offset)/p.y_scale;
    float2 position=(float2(index)+float2(0.0f,-0.5f))*0.5f+0.5f;
    float2 cc=(bilinear(uv,position,float2(0),float2(uv.get_width()-1,uv.get_height()-1)).rg*p.storage-p.c_offset)/p.c_scale;
    float4 sub=overlay.read(index);
    float r=yy+2*(1-p.kr)*cc.y, b=yy+2*(1-p.kb)*cc.x;
    float3 rgb(r,(yy-p.kr*r-p.kb*b)/(1-p.kr-p.kb),b);
    const float3 luma(p.luma[0],p.luma[1],p.luma[2]);
    rgb=to_linear(rgb,p.transfer,false,1.0f,luma);
    rgb=from_linear(rgb*(1-sub.a)+sub.rgb,p.transfer,false,1.0f,luma);
    float l=dot(rgb,float3(p.kr,1-p.kr-p.kb,p.kb));
    return float3(l,(rgb.b-l)/(2*(1-p.kb)),(rgb.r-l)/(2*(1-p.kr)));
}
kernel void subtitle_blend(texture2d<float,access::read> y [[texture(0)]],
    texture2d<float,access::read> uv [[texture(1)]],texture2d<float,access::write> out_y [[texture(2)]],
    texture2d<float,access::write> out_uv [[texture(3)]],texture2d<float,access::read> overlay [[texture(4)]],
    constant SubtitleParameters& p [[buffer(0)]],uint2 index [[thread_position_in_grid]]) {
    if (index.x>=y.get_width()||index.y>=y.get_height()) return;
    float value=y.read(index).r;
    if (overlay.read(index).a>0) value=round(clamp(subtitle_pixel(y,uv,overlay,index,p).x*p.y_scale+p.y_offset,0.0f,p.maximum))*p.output_storage;
    out_y.write(float4(value),index);
    if (index.x<uv.get_width()&&index.y<uv.get_height()) {
        float2 cc=uv.read(index).rg;
        // Left-sited chroma: vertical pair at the left luma position. Include
        // right coverage via [1 2 1] horizontal taps to avoid losing thin glyphs.
        float2 sum=0; float coverage=0;
        for (uint dy=0;dy<2;++dy) for (int dx=-1;dx<=1;++dx) {
            uint2 pos(uint(clamp(int(index.x*2)+dx,0,int(y.get_width())-1)),index.y*2+dy);
            float weight=dx==0?0.25f:0.125f;
            coverage+=overlay.read(pos).a;
            sum+=subtitle_pixel(y,uv,overlay,pos,p).yz*weight;
        }
        if (coverage>0) cc=round(clamp(sum*p.c_scale+p.c_offset,0.0f,p.maximum))*p.output_storage;
        out_uv.write(float4(cc,0,1),index);
    }
}
