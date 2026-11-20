// Ray Reconstruction guides from Metro Exodus EE's G-buffer (decoded from the RT composite pass,
// cs56190203; FINDINGS.md), at render size:
//   RT0 (rgba8 sRGB)  rgb = diffuse albedo
//   RT1 (rgb10a2)     rgb = view-space normal * 0.5 + 0.5, a = material id / 3
//   RT2 (rgba8 sRGB)  r = gloss (roughness = 1 - gloss), gba = specular colour F0
//   material id 2 uses F0 = 0.05 (dielectric), as the composite does
//   depth (D32S8, inverted, far = 0): 0 = sky
// Outputs: diffuse albedo, specular albedo (env-BRDF approximation of F0, roughness and N.V) and the
// world-space normal with linear roughness in w (RR's packed roughness mode).

cbuffer Params : register(b0) {
    float4 ViewToWorld0;  // rows of the view -> world rotation (composite registers 10-12, xyz)
    float4 ViewToWorld1;
    float4 ViewToWorld2;
    float2 InvProjScale;  // 1 / projection [0][0], 1 / [1][1] (view direction from NDC)
    float2 InvSize;       // 1 / render size
};

Texture2D<float4> GBuffer0 : register(t0);
Texture2D<float4> GBuffer1 : register(t1);
Texture2D<float4> GBuffer2 : register(t2);
Texture2D<float> Depth : register(t3);
RWTexture2D<float4> DiffuseOut : register(u0);
RWTexture2D<float4> SpecularOut : register(u1);
RWTexture2D<float4> NormalOut : register(u2);

float3 EnvBRDFApprox(float3 f0, float roughness, float NoV) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    AB.y *= saturate(50.0 * f0.g);
    return f0 * AB.x + AB.y;
}

float3 ToWorld(float3 v) {
    return float3(dot(ViewToWorld0.xyz, v), dot(ViewToWorld1.xyz, v), dot(ViewToWorld2.xyz, v));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    uint w, h;
    DiffuseOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    // Camera-to-pixel direction in view space (x right, y up, z forward).
    const float2 ndc = float2((id.x + 0.5) * InvSize.x * 2 - 1, 1 - (id.y + 0.5) * InvSize.y * 2);
    const float3 dirView = normalize(float3(ndc * InvProjScale, 1));

    if (Depth[id] <= 0) {  // sky: nothing to demodulate, normal facing the camera
        DiffuseOut[id] = float4(1, 1, 1, 1);
        SpecularOut[id] = float4(0, 0, 0, 1);
        NormalOut[id] = float4(ToWorld(-dirView), 1);
        return;
    }

    const float4 g0 = GBuffer0[id];
    const float4 g1 = GBuffer1[id];
    const float4 g2 = GBuffer2[id];

    float3 n = g1.rgb * 2 - 1;
    const float len = length(n);
    n = len > 1e-4 ? n / len : -dirView;
    const uint material = uint(g1.a * 3.0039);
    const float roughness = saturate(1 - g2.r);
    const float3 f0 = material == 2 ? float3(0.05, 0.05, 0.05) : g2.gba;
    const float NoV = saturate(dot(n, -dirView));

    DiffuseOut[id] = float4(g0.rgb, 1);
    SpecularOut[id] = float4(saturate(EnvBRDFApprox(f0, roughness, max(NoV, 1e-3))), 1);
    NormalOut[id] = float4(normalize(ToWorld(n)), roughness);
}
