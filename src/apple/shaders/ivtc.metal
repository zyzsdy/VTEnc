// Adapted from NVEncFilterIvtc.cu, Copyright (c) 2014-2016 rigaya, MIT.
// Full license and pinned upstream revision: THIRD_PARTY_NOTICES.md.
#include <metal_stdlib>
using namespace metal;
struct IvtcParameters { uint first, match, repair, nt, threshold, combpel, dthresh, depth; };
int2 ivtc_read(texture2d<float, access::read> t, int x, int y, uint depth) {
    const uint2 xy(uint(clamp(x, 0, int(t.get_width()) - 1)), uint(clamp(y, 0, int(t.get_height()) - 1)));
    return int2(round(t.read(xy).rg * (depth == 10 ? 65535.0f / 64.0f : 255.0f)));
}
int2 ivtc_pixel(texture2d<float, access::read> prev, texture2d<float, access::read> cur,
    texture2d<float, access::read> next, int x, int y, constant IvtcParameters& p, uint match) {
    y = clamp(y, 0, int(cur.get_height()) - 1);
    bool first = (uint(y) & 1) == p.first;
    if (match == 1 && !first) return ivtc_read(prev, x, y, p.depth);
    if (match == 2 && first) return ivtc_read(next, x, y, p.depth);
    return ivtc_read(cur, x, y, p.depth);
}
kernel void ivtc_score(texture2d<float, access::read> prev [[texture(0)]],
    texture2d<float, access::read> cur [[texture(1)]], texture2d<float, access::read> next [[texture(2)]],
    constant IvtcParameters& p [[buffer(0)]], device uint* scores [[buffer(1)]],
    uint2 xy [[thread_position_in_grid]], uint tid [[thread_index_in_threadgroup]],
    uint2 group [[threadgroup_position_in_grid]], uint2 groups [[threadgroups_per_grid]]) {
    threadgroup uint reduction[6 * 256];
    for (uint m = 0; m < 3; ++m) {
        uint metric = 0, comb = 0;
        if (xy.x < cur.get_width() && xy.y + 4 < cur.get_height() && (xy.y & 1) == p.first) {
            int v[5];
            for (int k = 0; k < 5; ++k) v[k] = ivtc_pixel(prev, cur, next, int(xy.x), int(xy.y) + k, p, m).x;
            int d = abs(v[1] - ((v[0] + v[2] + 1) >> 1)) + abs(v[3] - ((v[2] + v[4] + 1) >> 1));
            metric = d > int(p.nt) ? uint(d) : 0;
            int a = v[1] - v[0], b = v[1] - v[2];
            comb = abs(a) > int(p.threshold) && abs(b) > int(p.threshold) && ((a < 0) == (b < 0));
        }
        reduction[m * 256 + tid] = metric;
        reduction[(m + 3) * 256 + tid] = comb;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = 128; step > 0; step >>= 1) {
        if (tid < step) for (uint k = 0; k < 6; ++k) reduction[k * 256 + tid] += reduction[k * 256 + tid + step];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) {
        uint index = (group.y * groups.x + group.x) * 9;
        for (uint k = 0; k < 6; ++k) scores[index + k] = reduction[k * 256];
        for (uint k = 0; k < 3; ++k) scores[index + 6 + k] = reduction[(k + 3) * 256] >= p.combpel;
    }
}
kernel void ivtc_diff(texture2d<float, access::read> a [[texture(0)]], texture2d<float, access::read> b [[texture(1)]],
    constant IvtcParameters& p [[buffer(0)]], device uint* scores [[buffer(1)]],
    uint2 xy [[thread_position_in_grid]], uint tid [[thread_index_in_threadgroup]],
    uint2 group [[threadgroup_position_in_grid]], uint2 groups [[threadgroups_per_grid]]) {
    threadgroup uint reduction[256];
    reduction[tid] = xy.x < a.get_width() && xy.y < a.get_height()
        ? uint(abs(ivtc_read(a, int(xy.x), int(xy.y), p.depth).x - ivtc_read(b, int(xy.x), int(xy.y), p.depth).x)) : 0;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = 128; step > 0; step >>= 1) {
        if (tid < step) reduction[tid] += reduction[tid + step];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) scores[group.y * groups.x + group.x] = reduction[0];
}
kernel void ivtc_synthesize(texture2d<float, access::read> prev [[texture(0)]],
    texture2d<float, access::read> cur [[texture(1)]], texture2d<float, access::read> next [[texture(2)]],
    texture2d<float, access::write> dst [[texture(3)]], constant IvtcParameters& p [[buffer(0)]],
    uint2 xy [[thread_position_in_grid]]) {
    if (xy.x >= dst.get_width() || xy.y >= dst.get_height()) return;
    int x = int(xy.x), y = int(xy.y);
    int2 value = ivtc_pixel(prev, cur, next, x, y, p, p.match);
    if (p.repair && (xy.y & 1) != p.first) {
        int2 up = ivtc_pixel(prev, cur, next, x, y - 1, p, p.match);
        int2 dn = ivtc_pixel(prev, cur, next, x, y + 1, p, p.match);
        int2 interp = (up + dn + 1) >> 1;
        int2 pv = ivtc_read(prev, x, y, p.depth), nv = ivtc_read(next, x, y, p.depth);
        int2 motion = max(int2(0), max(abs(pv - value), abs(nv - value)) - max(1, int(p.dthresh) >> 3));
        int2 spatial = interp;
        if (y >= 3 && y < int(dst.get_height()) - 3) {
            int2 up3 = ivtc_pixel(prev, cur, next, x, y - 3, p, p.match);
            int2 dn3 = ivtc_pixel(prev, cur, next, x, y + 3, p, p.match);
            spatial = (5077 * (up + dn) - 981 * (up3 + dn3) + 4096) >> 13;
        }
        int2 temporal = (pv + nv + 2 * value + 2) >> 2;
        int threshold = int(p.dthresh) * 2;
        int2 blend = spatial;
        if (threshold > 0) {
            int2 w = clamp(motion, int2(0), int2(threshold));
            int2 w2 = (w * w + (threshold >> 1)) / threshold;
            blend = (temporal * (threshold - w2) + spatial * w2 + (threshold >> 1)) / threshold;
        }
        int epsilon = min(max(1, int(p.dthresh) >> 1), 8 << (p.depth - 8));
        blend = clamp(blend, max(int2(0), min(up, dn) - epsilon), min(int2((1 << p.depth) - 1), max(up, dn) + epsilon));
        value = select(blend, value, (abs(value - interp) <= int(p.dthresh)) & bool2(p.dthresh > 0));
    }
    float storage = p.depth == 10 ? 64.0f / 65535.0f : 1.0f / 255.0f;
    dst.write(float4(float2(value) * storage, 0, 1), xy);
}
