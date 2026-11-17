// Adapted from NVEncFilterYadif.cu, Copyright (c) 2014-2016 rigaya, MIT.
// Full license and pinned upstream revision: THIRD_PARTY_NOTICES.md.
struct YadifParameters { uint field, second, copy, depth, spatial_only; };
int yadif_read(texture2d<float, access::read> t, int x, int y, uint component, uint depth) {
    const uint2 xy(uint(clamp(x, 0, int(t.get_width()) - 1)), uint(clamp(y, 0, int(t.get_height()) - 1)));
    return int(round(t.read(xy)[component] * (depth == 10 ? 65535.0f / 64.0f : 255.0f)));
}
int yadif_spatial(texture2d<float, access::read> cur, int x, int y, uint c, uint depth) {
    int up[7], down[7];
    for (int k = 0; k < 7; ++k) {
        up[k] = yadif_read(cur, x + k - 3, y - 1, c, depth);
        down[k] = yadif_read(cur, x + k - 3, y + 1, c, depth);
    }
    int score[5];
    const int shifts[5] = {0, -1, -2, 1, 2};
    for (int k = 0; k < 5; ++k) {
        const int d = shifts[k];
        score[k] = abs(up[2+d]-down[2-d]) + abs(up[3+d]-down[3-d]) + abs(up[4+d]-down[4-d]);
    }
    int best = 0;
    // The ±2 search is conditional on ±1 improving the current best, as upstream.
    if (score[1] < score[best]) { best = 1; if (score[2] < score[best]) best = 2; }
    if (score[3] < score[best]) { best = 3; if (score[4] < score[best]) best = 4; }
    const int shift = shifts[best];
    return (up[3+shift] + down[3-shift]) >> 1;
}
int yadif_temporal(texture2d<float, access::read> prev, texture2d<float, access::read> cur,
    texture2d<float, access::read> next, int spatial, int x, int y, uint c, constant YadifParameters& p) {
    const int up = yadif_read(cur,x,y-1,c,p.depth), down = yadif_read(cur,x,y+1,c,p.depth);
    int a[3], b[3];
    for (int k = 0; k < 3; ++k) {
        const int row = y + 2 * (k - 1);
        a[k] = p.second ? yadif_read(cur,x,row,c,p.depth) : yadif_read(prev,x,row,c,p.depth);
        b[k] = p.second ? yadif_read(next,x,row,c,p.depth) : yadif_read(cur,x,row,c,p.depth);
    }
    const int upper = (a[0]+b[0]) >> 1, center = (a[1]+b[1]) >> 1, lower = (a[2]+b[2]) >> 1;
    // NVEnc uses the full center difference (not FFmpeg's half difference).
    int diff = max(abs(a[1]-b[1]), max(
        (abs(yadif_read(prev,x,y-1,c,p.depth)-up)+abs(yadif_read(prev,x,y+1,c,p.depth)-down)) >> 1,
        (abs(yadif_read(next,x,y-1,c,p.depth)-up)+abs(yadif_read(next,x,y+1,c,p.depth)-down)) >> 1));
    diff = max(diff, max(-max(max(center-down,center-up),min(upper-up,lower-down)),
                         min(min(center-down,center-up),max(upper-up,lower-down))));
    return clamp(spatial, center-diff, center+diff);
}
kernel void yadif_frame(texture2d<float, access::read> prev [[texture(0)]],
    texture2d<float, access::read> cur [[texture(1)]], texture2d<float, access::read> next [[texture(2)]],
    texture2d<float, access::write> dst [[texture(3)]], constant YadifParameters& p [[buffer(0)]],
    uint2 xy [[thread_position_in_grid]]) {
    if (xy.x >= dst.get_width() || xy.y >= dst.get_height()) return;
    const int x = int(xy.x), y = int(xy.y);
    float4 output(0,0,0,1);
    for (uint c = 0; c < 2; ++c) {
        int value = yadif_read(cur,x,y,c,p.depth);
        if (!p.copy && (xy.y & 1) != p.field) {
            value = yadif_spatial(cur,x,y,c,p.depth);
            if (!p.spatial_only) value = yadif_temporal(prev,cur,next,value,x,y,c,p);
        }
        value = clamp(value,0,(1 << p.depth)-1);
        output[c] = float(value) * (p.depth == 10 ? 64.0f/65535.0f : 1.0f/255.0f);
    }
    dst.write(output,xy);
}
