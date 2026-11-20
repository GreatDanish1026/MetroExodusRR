// Replacement for the game's GI spatial blur (cs66e47af2) that keeps only the centre sample.
//
// The two GI chains (A, B) are stored divided by f = saturate(B.z * 12) * 0.9 + 0.1, and B.x / B.y
// carry a 4-bit frame count in their lowest half-float mantissa bits. The game's blur ends with:
//   outA = f * avg(A)
//   outB = (f * avg(B.x'), f * avg(B.y'), B.z, count / 15)
// where B.x' / B.y' have the low 4 bits forced to 7 and count = low 4 bits of B.y (centre pixel).
// With the centre sample only, the averages are the centre values, so this is that output step
// without the blur: noisy GI at the game's brightness, for Ray Reconstruction to denoise.

Texture2D<float4> ChainA : register(t0);  // mip 0 of the game's chain A (blur t3)
Texture2D<float4> ChainB : register(t1);  // mip 0 of the game's chain B (blur t4)
RWTexture2D<float4> OutA : register(u0);  // blur u0
RWTexture2D<float4> OutB : register(u1);  // blur u1

float Bits7(float v) {
    uint h = f32tof16(v);
    return f16tof32((h & 0xfff0u) | 7u);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h;
    OutA.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const float4 a = ChainA.Load(int3(id, 0));
    const float4 b = ChainB.Load(int3(id, 0));
    const float f = saturate(b.z * 12.0) * 0.9 + 0.1;
    const float count = float(f32tof16(b.y) & 15u) * (1.0 / 15.0);
    OutA[id] = f * a;
    OutB[id] = float4(f * Bits7(b.x), f * Bits7(b.y), b.z, count);
}
