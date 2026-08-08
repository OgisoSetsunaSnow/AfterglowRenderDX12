// Pluviometer 初始化：参考项目 PluviometerInitializer_CS.hlsl 为空实现（Vulkan SSBO 自动清零）。
// D3D12 纹理初始内容未定义，故显式写 0（API 差异适配）。格式 RG16Float（.r=降雨量）。
RWTexture2D<float2> PluviometerOut : register(u0);

[numthreads(32, 32, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
	PluviometerOut[threadID.xy] = float2(0.0, 0.0);
}
