// Grass 常量与结构（照搬参考项目 Grass/GrassCommon.hlsl + GrassInstancing.mat elementLayout）。
#ifndef GRASS_COMMON_HLSL
#define GRASS_COMMON_HLSL

#include "../WorldDefinitions.hlsl"

static const uint grassDataSideElements = 2048;
static const uint grassVisibleSideElement = 128;

static const float grassVisibleDistance = 160.0;

static const float grassInvDataSideElements = 1.0 / grassDataSideElements;

static const float grassDataInterval = worldSideLength / grassDataSideElements;
static const float grassInvDataInterval = 1.0 / grassDataInterval;

// 实例结构（GrassInstancing.mat elementLayout: model:9 invTransModel:9 wind:4）。
struct InstanceBufferStruct {
	float4x4 model;
	float4x4 invTransModel;
	float2 wind;
};

// IndirectBuffer（D3D12_DRAW_INDEXED_ARGUMENTS 布局：indexCount, instanceCount, firstIndex, vertexOffset, firstInstance）。
struct IndirectBufferStruct {
	uint indexCount;
	uint instanceCount;
	uint firstIndex;
	int  vertexOffset;
	uint firstInstance;
};

#endif
