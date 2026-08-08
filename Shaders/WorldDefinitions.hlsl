#ifndef WORLD_DEFINITIONS_HLSL
#define WORLD_DEFINITIONS_HLSL
static const float worldSideLength = 2048.0;
static const float worldGravity = 9.8; // m/(s^2)
static const float worldInvSideLength = 1.0 / worldSideLength;
static const float worldCenterOffset = worldSideLength * -0.5;

float2 WorldPositionFromTileID(uint2 id, float invSideElements) {
	return worldSideLength * (invSideElements * id) + worldCenterOffset;
}

uint2 TileIDFromWorldPosition(float2 worldPos, float invInterval) {
	return uint2(clamp((worldPos - worldCenterOffset) * invInterval, 0, worldSideLength * invInterval));
}
#endif
