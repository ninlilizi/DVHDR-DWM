// dvhdr_dwm.hlsl — plain-HLSL port of DVHDR.fx for compilation via
// D3DCompile inside the injected DLL. Algorithmically identical to the
// ReShade source at FFXIV-Dynamic-HDR/Shaders/DVHDR.fx; the only changes
// are mechanical:
//
//   ReShade                                  Plain HLSL (this file)
//   ───────────                              ────────────────────────────
//   BUFFER_WIDTH / BUFFER_HEIGHT             cbuffer field BufferSize
//   BUFFER_COLOR_SPACE                       cbuffer field ColorSpace
//   uniform float Foo < ui_* > = …;          cbuffer field
//   <source = "frametime">                   cbuffer field FrameTimeMs
//   texture / sampler / storage2D            Texture2D / SamplerState / RWTexture2D
//   tex2Dstore                               UAV index write
//   tex2Dfetch                               .Load
//   atomicAdd                                InterlockedAdd
//   technique pass {…}                       discrete entry points dispatched by C++
//
// Resource bindings (must match what the C++ side sets up):
//
//   t0  SceneTex          — back-buffer copy SRV (BGRA / RGBA16F)
//   t1  HistogramSRV      — read-only view of the histogram (debug overlay)
//   t2  AdaptSRV          — read-only view of the temporal adapt state
//   t3  LumaHSRV          — separable blur horizontal stage output
//   t4  LumaBlurSRV       — separable blur final (used by tonemap)
//
//   u0  HistogramUAV      — written by CS_Clear / CS_Analyze
//   u1  AdaptUAV          — written by CS_Adapt
//
//   s0  Samp              — linear, clamp
//
// Six entry points: CS_Clear, CS_Analyze, CS_Adapt, VS_Post, PS_BlurH, PS_BlurV, PS_Tonemap.

// ===========================================================================
//  Constants
// ===========================================================================

#define DVHDR_BINS           256
#define DVHDR_GROUP          16
#define DVHDR_LC_MAX_RADIUS  24

static const uint CSP_SCRGB = 1u;
static const uint CSP_HDR10 = 2u;

cbuffer DVHDRCb : register(b0)
{
    uint2  BufferSize;            // BUFFER_WIDTH, BUFFER_HEIGHT
    uint   ColorSpace;            // CSP_SCRGB | CSP_HDR10
    float  FrameTimeMs;           // ReShade's <source="frametime">

    float  DisplayPeak;
    float  DisplayMaxFALL;
    float  DisplayBlack;
    float  HeadroomPercent;

    float  MinGain;
    float  LiftStrength;
    float  MaxGain;
    float  HighlightProtect;

    float  PeakPercentile;
    float  AttackMs;
    float  ReleaseMs;
    float  DynamicContrast;

    float  LocalContrast;
    float  LocalContrastRadius;
    float  LocalContrastBias;
    uint   UseHighlightRolloff;

    float  Strength;
    uint   DebugOverlay;
    uint   AnalyzeStride;
    float  DitherThresholdNits;

    float  DitherStrengthNits;
    float  DitherGradBoost;
    uint   _pad1, _pad2;
};

// ===========================================================================
//  Resources
// ===========================================================================

Texture2D<float4>     SceneTex      : register(t0);
Texture2D<int>        HistogramSRV  : register(t1);
Texture2D<float4>     AdaptSRV      : register(t2);
Texture2D<float>      LumaHSRV      : register(t3);
Texture2D<float>      LumaBlurSRV   : register(t4);

RWTexture2D<int>      HistogramUAV  : register(u0);
RWTexture2D<float4>   AdaptUAV      : register(u1);

SamplerState          Samp          : register(s0);

// ===========================================================================
//  Colour-space helpers — verbatim from DVHDR.fx (algorithmically identical)
// ===========================================================================

static const float PQ_m1 = 0.1593017578125;
static const float PQ_m2 = 78.84375;
static const float PQ_c1 = 0.8359375;
static const float PQ_c2 = 18.8515625;
static const float PQ_c3 = 18.6875;

float3 PQ_to_linear(float3 E)
{
    float3 Ep  = pow(max(E, 0.0), 1.0 / PQ_m2);
    float3 num = max(Ep - PQ_c1, 0.0);
    float3 den = PQ_c2 - PQ_c3 * Ep;
    return pow(num / max(den, 1e-6), 1.0 / PQ_m1);
}

float3 linear_to_PQ(float3 L)
{
    float3 Lm  = pow(max(L, 0.0), PQ_m1);
    float3 num = PQ_c1 + PQ_c2 * Lm;
    float3 den = 1.0 + PQ_c3 * Lm;
    return pow(num / den, PQ_m2);
}

float nits_to_pq(float nits)
{
    return linear_to_PQ(saturate(nits / 10000.0).xxx).x;
}

float pq_to_nits(float e)
{
    return PQ_to_linear(saturate(e).xxx).x * 10000.0;
}

float3 decode_to_nits(float3 c, uint csp)
{
    if (csp == CSP_HDR10) return PQ_to_linear(saturate(c)) * 10000.0;
    return c * 80.0;
}

float3 encode_from_nits(float3 n, uint csp)
{
    if (csp == CSP_HDR10) return linear_to_PQ(saturate(n / 10000.0));
    return n / 80.0;
}

float luminance(float3 n, uint csp)
{
    if (csp == CSP_HDR10) return dot(max(n, 0.0), float3(0.2627, 0.6780, 0.0593));
    return dot(max(n, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float bt2390_eetf(float L, float srcPeak, float dstPeak, float dstBlack)
{
    srcPeak = max(srcPeak, dstPeak);
    float e    = nits_to_pq(L);
    float sMax = nits_to_pq(srcPeak);
    float sMin = 0.0;
    float dMax = nits_to_pq(dstPeak);
    float dMin = nits_to_pq(dstBlack);

    float denom  = max(sMax - sMin, 1e-5);
    float E1     = saturate((e - sMin) / denom);
    float maxLum = (dMax - sMin) / denom;
    float minLum = (dMin - sMin) / denom;
    float KS     = 1.5 * maxLum - 0.5;

    float E2 = E1;
    if (E1 > KS && KS < 1.0)
    {
        float T  = (E1 - KS) / (1.0 - KS);
        float T2 = T * T;
        float T3 = T2 * T;
        E2 = (2.0 * T3 - 3.0 * T2 + 1.0) * KS
           + (T3 - 2.0 * T2 + T) * (1.0 - KS)
           + (-2.0 * T3 + 3.0 * T2) * maxLum;
    }

    float E3   = E2 + minLum * pow(max(1.0 - E2, 0.0), 4.0);
    float Eout = E3 * denom + sMin;
    return pq_to_nits(Eout);
}

float bin_to_nits(int i)
{
    return pq_to_nits((i + 0.5) / float(DVHDR_BINS));
}

// Interleaved Gradient Noise — Jorge Jimenez. Cheap blue-noise-like pattern
// from screen-space pixel coordinates, no texture required. Output is in
// [0, 1] with good spectral properties for dither.
float ign(float2 pos)
{
    return frac(52.9829189 * frac(dot(pos, float2(0.06711056, 0.00583715))));
}

// ===========================================================================
//  Pass 1 — clear histogram (one workgroup of 256)
// ===========================================================================

[numthreads(DVHDR_BINS, 1, 1)]
void CS_Clear(uint3 id : SV_DispatchThreadID)
{
    if (id.x < (uint)DVHDR_BINS)
        HistogramUAV[int2(int(id.x), 0)] = 0;
}

// ===========================================================================
//  Pass 2 — accumulate luminance histogram
// ===========================================================================

[numthreads(DVHDR_GROUP, DVHDR_GROUP, 1)]
void CS_Analyze(uint3 id : SV_DispatchThreadID)
{
    int2 px = int2(id.xy) * (int)AnalyzeStride;
    if ((uint)px.x >= BufferSize.x || (uint)px.y >= BufferSize.y) return;

    float3 c = SceneTex.Load(int3(px, 0)).rgb;
    float  Y = luminance(decode_to_nits(c, ColorSpace), ColorSpace);

    int bin = clamp(int(nits_to_pq(Y) * (DVHDR_BINS - 1) + 0.5), 0, DVHDR_BINS - 1);
    InterlockedAdd(HistogramUAV[int2(bin, 0)], 1);
}

// ===========================================================================
//  Pass 3 — reduce histogram → stats → temporal adapt
// ===========================================================================

[numthreads(1, 1, 1)]
void CS_Adapt(uint3 id : SV_DispatchThreadID)
{
    float total   = 0.0;
    float sumNits = 0.0;
    float maxBin  = 1.0;

    [loop]
    for (int i = 0; i < DVHDR_BINS; i++)
    {
        float cnt = float(HistogramUAV[int2(i, 0)]);
        total   += cnt;
        sumNits += cnt * bin_to_nits(i);
        maxBin   = max(maxBin, cnt);
    }

    float fall = sumNits / max(total, 1.0);

    float allow = total * (1.0 - PeakPercentile * 0.01);
    float acc   = 0.0;
    int   pidx  = DVHDR_BINS - 1;
    [loop]
    for (int j = DVHDR_BINS - 1; j >= 0; j--)
    {
        acc += float(HistogramUAV[int2(j, 0)]);
        if (acc >= allow) { pidx = j; break; }
    }
    float peak = bin_to_nits(pidx);

    float4 prev  = AdaptUAV[int2(0, 0)];
    float  pPeak = prev.x;
    float  pFall = prev.y;
    if (pFall <= 0.0001) pFall = fall;
    if (pPeak <= 0.0001) pPeak = peak;

    float aUp = saturate(1.0 - exp(-FrameTimeMs / max(AttackMs,  1.0)));
    float aDn = saturate(1.0 - exp(-FrameTimeMs / max(ReleaseMs, 1.0)));

    float sFall = lerp(pFall, fall, (fall > pFall) ? aUp : aDn);
    float sPeak = lerp(pPeak, peak, (peak > pPeak) ? aUp : aDn);

    AdaptUAV[int2(0, 0)] = float4(sPeak, sFall, fall, maxBin);
}

// ===========================================================================
//  Full-screen triangle for the raster passes
// ===========================================================================

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VS_OUT VS_Post(uint id : SV_VertexID)
{
    VS_OUT o;
    o.uv  = float2((id == 2) ? 2.0 : 0.0, (id == 1) ? 2.0 : 0.0);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// ===========================================================================
//  Pass 3b — separable luminance blur (PQ-encoded neighbourhood mean)
// ===========================================================================

float pq_luma_at(int2 px)
{
    float3 c = SceneTex.Load(int3(px, 0)).rgb;
    return nits_to_pq(luminance(decode_to_nits(c, ColorSpace), ColorSpace));
}

float PS_BlurH(VS_OUT input) : SV_TARGET
{
    int2 px = int2(input.pos.xy);
    if (LocalContrast <= 0.0)
        return pq_luma_at(px);

    int   R     = clamp(int(LocalContrastRadius + 0.5), 1, DVHDR_LC_MAX_RADIUS);
    float sigma = max(R * 0.5, 1.0);
    float sum = 0.0, wsum = 0.0;
    [loop]
    for (int d = -R; d <= R; d++)
    {
        int2  sp  = int2(clamp(px.x + d, 0, int(BufferSize.x) - 1), px.y);
        float wgt = exp(-0.5 * float(d * d) / (sigma * sigma));
        sum  += pq_luma_at(sp) * wgt;
        wsum += wgt;
    }
    return sum / max(wsum, 1e-5);
}

float PS_BlurV(VS_OUT input) : SV_TARGET
{
    int2 px = int2(input.pos.xy);
    if (LocalContrast <= 0.0)
        return LumaHSRV.Load(int3(px, 0));

    int   R     = clamp(int(LocalContrastRadius + 0.5), 1, DVHDR_LC_MAX_RADIUS);
    float sigma = max(R * 0.5, 1.0);
    float sum = 0.0, wsum = 0.0;
    [loop]
    for (int d = -R; d <= R; d++)
    {
        int2  sp  = int2(px.x, clamp(px.y + d, 0, int(BufferSize.y) - 1));
        float wgt = exp(-0.5 * float(d * d) / (sigma * sigma));
        sum  += LumaHSRV.Load(int3(sp, 0)) * wgt;
        wsum += wgt;
    }
    return sum / max(wsum, 1e-5);
}

// ===========================================================================
//  Pass 4 — dynamic tonemap
// ===========================================================================

float4 PS_Tonemap(VS_OUT input) : SV_TARGET
{
    float2 uv  = input.uv;
    int2   px  = int2(input.pos.xy);

    float3 src   = SceneTex.SampleLevel(Samp, uv, 0).rgb;
    float4 ad    = AdaptSRV.Load(int3(0, 0, 0));
    float  adPeak = ad.x;
    float  adFall = ad.y;

    float targetFall = DisplayMaxFALL * (HeadroomPercent * 0.01);
    float ratio      = targetFall / max(adFall, 0.01);
    float g = (ratio < 1.0) ? max(ratio, MinGain)
                            : min(pow(ratio, LiftStrength), MaxGain);

    float3 lin  = decode_to_nits(src, ColorSpace);
    float  Ysrc = luminance(lin, ColorSpace);

    // Highlight protection — full multiplicative lift below 50% of
    // HighlightProtect, smoothstep blend on the gain across [50%, 100%], then
    // identity above. Keeps a flat "fully-lifted dark zone" the user can size
    // with HighlightProtect, while the upper half of that band smoothly
    // releases the lift so the seam at the protection boundary is invisible.
    //
    // Monotonic for MaxGain <= 2.0 (the default MaxGain of 1.5 has comfortable
    // margin). Above g = 2 the blend lets Y*gEff overshoot HighlightProtect
    // and the boundary re-introduces a non-monotonic seam — keep MaxGain
    // below 2 to stay on the safe side.
    float Ylift, w;
    if (g > 1.0)
    {
        if (Ysrc >= HighlightProtect)
        {
            Ylift = Ysrc;
            w     = 0.0;
        }
        else
        {
            float t    = smoothstep(0.5, 1.0, Ysrc / max(HighlightProtect, 1e-4));
            float gEff = lerp(g, 1.0, t);
            Ylift      = Ysrc * gEff;
            w          = 1.0 - t;
        }
    }
    else
    {
        // Global compression — applied uniformly, protect band irrelevant.
        Ylift = Ysrc * g;
        w     = 1.0;
    }

    float Yc = Ylift;
    if (DynamicContrast > 0.0 && g > 1.0)
    {
        float strength = DynamicContrast * saturate((g - 1.0) / max(MaxGain - 1.0, 0.01)) * w;
        float pivot    = nits_to_pq(max(adFall * g, 0.1));
        float p        = nits_to_pq(Ylift);
        Yc = pq_to_nits(saturate(pivot + (p - pivot) * (1.0 + strength)));
    }

    float Ylc = Yc;
    if (LocalContrast > 0.0 && g > 1.0)
    {
        float pqBlur = LumaBlurSRV.SampleLevel(Samp, uv, 0);
        float detail = nits_to_pq(Ysrc) - pqBlur;
        float biased = (detail > 0.0) ? detail : detail * (1.0 - LocalContrastBias);
        float amount = LocalContrast * saturate((g - 1.0) / max(MaxGain - 1.0, 0.01)) * w;
        Ylc = pq_to_nits(saturate(nits_to_pq(Yc) + biased * amount));
    }

    float Yt = (UseHighlightRolloff != 0)
                 ? bt2390_eetf(Ylc, adPeak * min(g, 1.0), DisplayPeak, DisplayBlack)
                 : Ylc;

    // Hot-highlight dither — break up the panel's posterisation in the
    // brightest regions. Amplitude scales with how far Yt sits above
    // DitherThresholdNits (linear ramp to full at DisplayPeak), and is
    // additionally boosted in pixels with large 4-neighbour luma deltas where
    // the banding is most visible. Pattern is IGN (cheap, near-blue spectrum).
    if (DitherStrengthNits > 0.0 && Yt > DitherThresholdNits)
    {
        float bright = saturate((Yt - DitherThresholdNits)
                              / max(DisplayPeak - DitherThresholdNits, 1.0));

        float2 ts = float2(1.0 / float(BufferSize.x), 1.0 / float(BufferSize.y));
        float YL = luminance(decode_to_nits(SceneTex.SampleLevel(Samp, uv + float2(-ts.x, 0.0), 0).rgb, ColorSpace), ColorSpace);
        float YR = luminance(decode_to_nits(SceneTex.SampleLevel(Samp, uv + float2( ts.x, 0.0), 0).rgb, ColorSpace), ColorSpace);
        float YU = luminance(decode_to_nits(SceneTex.SampleLevel(Samp, uv + float2(0.0, -ts.y), 0).rgb, ColorSpace), ColorSpace);
        float YD = luminance(decode_to_nits(SceneTex.SampleLevel(Samp, uv + float2(0.0,  ts.y), 0).rgb, ColorSpace), ColorSpace);
        float gradient = max(max(abs(Ysrc - YL), abs(Ysrc - YR)),
                             max(abs(Ysrc - YU), abs(Ysrc - YD)));
        float gradFactor = saturate(gradient * 0.005); // full response at ~200 nit delta

        float noise = ign(input.pos.xy) - 0.5;        // [-0.5, +0.5]
        float amp   = DitherStrengthNits * bright * (1.0 + DitherGradBoost * gradFactor);
        Yt = max(Yt + noise * amp, 0.0);
    }

    float3 outNits = lin * (Yt / max(Ysrc, 1e-4));
    float3 outc    = lerp(src, encode_from_nits(outNits, ColorSpace), Strength);

    // Overlay mode 2 — fill every presented pixel with a bright HDR red so the
    // user can confirm the write path is reaching the panel independent of any
    // tonemapping maths. Unconditional, fires before the histogram overlay.
    if (DebugOverlay == 2u)
    {
        return float4(encode_from_nits(float3(800.0, 0.0, 0.0), ColorSpace), 1.0);
    }

    if (DebugOverlay == 1u)
    {
        float2 o0 = float2(24.0, float(BufferSize.y) - 24.0 - 180.0);
        float2 o1 = float2(24.0 + float(DVHDR_BINS) * 2.0, float(BufferSize.y) - 24.0);
        if (input.pos.x >= o0.x && input.pos.x < o1.x && input.pos.y >= o0.y && input.pos.y < o1.y)
        {
            int   bin   = clamp(int((input.pos.x - o0.x) / 2.0), 0, DVHDR_BINS - 1);
            float count = float(HistogramSRV.Load(int3(bin, 0, 0)));
            float h     = count / max(ad.w, 1.0);
            float yNorm = 1.0 - (input.pos.y - o0.y) / (o1.y - o0.y);

            int fallBin   = clamp(int(nits_to_pq(ad.z) * (DVHDR_BINS - 1) + 0.5), 0, DVHDR_BINS - 1);
            int targetBin = clamp(int(nits_to_pq(DisplayMaxFALL * (HeadroomPercent * 0.01)) * (DVHDR_BINS - 1) + 0.5), 0, DVHDR_BINS - 1);
            int peakBin   = clamp(int(nits_to_pq(adPeak) * (DVHDR_BINS - 1) + 0.5), 0, DVHDR_BINS - 1);

            float3 panel = (yNorm <= h) ? float3(0.85, 0.85, 0.85) : float3(0.05, 0.05, 0.05);
            if (bin == fallBin)   panel = float3(0.1, 1.0, 0.1);
            if (bin == targetBin) panel = float3(0.1, 0.8, 1.0);
            if (bin == peakBin)   panel = float3(1.0, 0.2, 0.2);

            outc = encode_from_nits(panel * 100.0, ColorSpace);
        }
    }

    return float4(outc, 1.0);
}
