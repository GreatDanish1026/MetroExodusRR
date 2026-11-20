// Caps the GI temporal pass's per-pixel frame count before it runs.
//
// The temporal pass (csb83925ed) gathers last frame's r32f "depth + count" texture (its t3): the
// float's lowest 5 mantissa bits hold how many frames were accumulated (capped at 31 by the game),
// the rest is depth for the disocclusion test. Count 0 = blend weight 1 (current frame only);
// a cap of N keeps at most about N + 1 frames of history. The depth test is untouched.

cbuffer Params : register(b0) {
    uint MaxCount;  // 0..31
};

RWTexture2D<float> DepthCount : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h;
    DepthCount.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const uint v = asuint(DepthCount[id]);
    DepthCount[id] = asfloat((v & ~31u) | min(v & 31u, MaxCount));
}
