// compute pipeline verification: write solid red to noise texture.
// If PostProcess key 5 shows solid red -> compute pipeline works.
RWTexture2D<float4> noiseTex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
	uint2 dims;
	noiseTex.GetDimensions(dims.x, dims.y);
	if (tid.x >= dims.x || tid.y >= dims.y) {
		return;
	}
	// debug: solid red.
	noiseTex[tid.xy] = float4(1.0, 0.0, 0.0, 1.0);
}
