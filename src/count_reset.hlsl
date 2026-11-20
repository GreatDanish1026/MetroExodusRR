// Resets the GI temporal pass's per-pixel frame count before it runs.
//
// The temporal pass (csb83925ed) gathers last frame's r32f "depth + count" texture (its t3): the
// float's lowest 5 mantissa bits hold how many frames were accumulated (capped at 31), the rest is
// depth for the disocclusion test. With the count at 0 the blend weight is 1, so the pass keeps the
// current frame only (no temporal accumulation) while the depth test still works.

RWTexture2D<float> DepthCount : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h;
    DepthCount.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    DepthCount[id] = asfloat(asuint(DepthCount[id]) & ~31u);
}
