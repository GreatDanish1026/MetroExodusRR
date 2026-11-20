// Replacement for the game's GI spatial blur (cs66e47af2) that keeps the GI colour unfiltered.
//
// The two GI chains (A, B) are stored divided by f = saturate(B.z * 12) * 0.9 + 0.1, and B.x / B.y
// carry a 4-bit frame count in their lowest half-float mantissa bits. The game's blur ends with:
//   outA = f * avg(A)
//   outB = (f * avg(B.x'), f * avg(B.y'), B.z~, count / 15)
// where B.x' / B.y' have the low 4 bits forced to 7, count = low 4 bits of B.y (centre pixel) and
// B.z~ (which f is computed from) is the blurred B.z.
//
// Here A, B.x and B.y are the centre pixel only (noisy GI for Ray Reconstruction). B.z, and with it
// f, is a 5x5 average (stride 2) of B.z: with the frame count reset the per-pixel B.z is a single
// noisy sample, and the clipped f of a noisy B.z is biased low (-16 % lit brightness measured).

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

    float bz = 0;
    float n = 0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            const int2 p = int2(id) + int2(x, y) * 2;
            if (p.x < 0 || p.y < 0 || p.x >= int(w) || p.y >= int(h)) continue;
            bz += ChainB.Load(int3(p, 0)).z;
            n += 1;
        }
    }
    bz /= max(n, 1.0);

    const float f = saturate(bz * 12.0) * 0.9 + 0.1;
    const float count = float(f32tof16(b.y) & 15u) * (1.0 / 15.0);
    OutA[id] = f * a;
    OutB[id] = float4(f * Bits7(b.x), f * Bits7(b.y), bz, count);
}
