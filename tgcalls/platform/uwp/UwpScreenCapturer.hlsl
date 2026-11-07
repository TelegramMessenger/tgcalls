// The capture, scaled down and converted to NV12 on the GPU, so that only NV12 at the
// delivered size is read back. It reproduces what libyuv did on the CPU before: ARGBScale
// with kFilterBox, which at a power-of-two ratio averages square blocks exactly, followed
// by ARGBToNV12, BT.601 studio range with libyuv's own coefficients.
//
// Compiled into UwpScreenCapturerShader.h, which is checked in:
//   fxc UwpScreenCapturer.hlsl /nologo /T cs_5_0 /E main /O3 /Vn kScreenToNV12Shader /Fh UwpScreenCapturerShader.h

Texture2D<float4> Source : register(t0);
RWTexture2D<float> Luma : register(u0);
RWTexture2D<float2> Chroma : register(u1);

cbuffer Parameters : register(b0)
{
    // Source pixels per output pixel, in each direction: 1, 2, 4...
    uint Block;
    uint ChromaWidth;
    uint ChromaHeight;
};

float3 Average(uint2 origin)
{
    float3 sum = 0;
    for (uint y = 0; y < Block; ++y)
    {
        for (uint x = 0; x < Block; ++x)
        {
            sum += Source.Load(int3(origin + uint2(x, y), 0)).rgb;
        }
    }
    return sum / (Block * Block);
}

// One thread per chroma sample, which covers two by two luma samples.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= ChromaWidth || id.y >= ChromaHeight)
    {
        return;
    }

    float3 sum = 0;
    for (uint y = 0; y < 2; ++y)
    {
        for (uint x = 0; x < 2; ++x)
        {
            const uint2 position = id.xy * 2 + uint2(x, y);
            const float3 rgb = Average(position * Block);

            Luma[position] = dot(rgb, float3(66, 129, 25) / 256) + 16.0 / 255;
            sum += rgb;
        }
    }

    const float3 rgb = sum / 4;
    Chroma[id.xy] = float2(
        dot(rgb, float3(-38, -74, 112) / 256) + 128.0 / 255,
        dot(rgb, float3(112, -94, -18) / 256) + 128.0 / 255);
}
