// GPU mip chain generation, equivalent to reference project vkCmdBlitImage mipmap gen.
// SRV is bound with MipSlice = srcMipLevel, so SampleLevel(uv, 0) samples resource mip (srcMipLevel).
Texture2D<float4> srcTex : register(t0);
RWTexture2D<float4> dstTex : register(u0);
SamplerState mipSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
	uint2 dstDim;
	dstTex.GetDimensions(dstDim.x, dstDim.y);
	if (tid.x >= dstDim.x || tid.y >= dstDim.y) {
		return;
	}
	float2 uv = (float2(tid.xy) + 0.5f) / float2(dstDim);
	dstTex[tid.xy] = srcTex.SampleLevel(mipSampler, uv, 0.0f);
}
