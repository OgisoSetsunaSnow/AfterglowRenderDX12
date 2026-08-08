// Random/noise library ported from reference project (AfterglowRender-main/vsbuild/Shaders/Random.hlsl).
// Only the functions needed by terrain height generation are kept.
#ifndef RANDOM_HLSL
#define RANDOM_HLSL

static const float randomSeed0 = 1312.21551f;
static const float randomSeed1 = 5441.35424f;
static const float randomSeed2 = 3829.19572f;
static const float randomSeed3 = 2948.75398f;

// ---- helpers (from reference Common.hlsl / Constants.hlsl) ----
float SnormF(float v) { return v * 2.0 - 1.0; }
float2 Snorm2(float2 v) { return v * 2.0 - 1.0; }
float3 Snorm3(float3 v) { return v * 2.0 - 1.0; }
float UnormF(float v) { return v * 0.5 + 0.5; }
static const float kSqrt2 = 1.41421356237; // 不与 Constants.hlsl 的 sqrt2 冲突
float Pow4F(float x) { float xx = x * x; return xx * xx; }

// ---- hashes ----
float Hash(float x) {
	return frac(sin(x * 15368.6234 + 475.053) + 15413.21);
}

float Hash(float x, float seed) {
	return frac(sin(x * 14523.46187) * seed);
}

float Hash(float2 xy, float seed) {
	return frac(sin(dot(xy, float2(13.6196, 54.2197))) * 43758.5453123 + seed);
}

float2 Hash2D(float2 xy, float seed) {
	return frac(sin(float2(
		dot(xy, float2(46.531, 91.4653)),
		dot(xy, float2(64.4634, 49.4349))
	)) * 13127.643 + seed);
}

// ---- 参考项目 Random.hlsl：Boid/Grass 用（BitHash/UniformBitHash/Hash3D）----
int BitHash(int x) {
	x = (x ^ 61) ^ (x >> 16);
	x = x + (x << 3);
	x = x ^ (x >> 4);
	x = x * 0x27d4eb2d;
	x = x ^ (x >> 15);
	return x;
}

float UniformBitHash(int x) {
	return float(BitHash(x)) / float(0x7FFFFFFF);
}

float3 Hash3D(float3 xyz, float seed) {
	return frac(sin(float3(
		dot(xyz, float3(83.7247, 71.7823, 24.274)),
		dot(xyz, float3(64.4634, 49.4349, 82.263)),
		dot(xyz, float3(94.262, 20.9245, 34.8256))
	)) * 52567.0925 + seed);
}

float Hash(float x, float y, float seed) {
	return Hash(float2(x, y), seed);
}

// ---- Perlin noise 2D (reference Random.hlsl) ----
float PerlinNoise(float2 xy, float2 scale, float seed) {
	float2 scaledXY = xy * scale;
	float2 scaledFloorXY = floor(scaledXY);
	float2 scaledCeilXY = ceil(scaledXY);

	float2 floorXY = scaledFloorXY / scale;
	float2 ceilXY = scaledCeilXY / scale;

	float2 gradientFF = Snorm2(Hash2D(floorXY, seed));
	float2 gradientFC = Snorm2(Hash2D(float2(floorXY.x, ceilXY.y), seed));
	float2 gradientCF = Snorm2(Hash2D(float2(ceilXY.x, floorXY.y), seed));
	float2 gradientCC = Snorm2(Hash2D(ceilXY, seed));

	float2 distFF = scaledXY - scaledFloorXY;
	float productFF = dot(gradientFF, distFF);
	float productFC = dot(gradientFC, scaledXY - float2(scaledFloorXY.x, scaledCeilXY.y));
	float productCF = dot(gradientCF, scaledXY - float2(scaledCeilXY.x, scaledFloorXY.y));
	float productCC = dot(gradientCC, scaledXY - scaledCeilXY);

	float2 weight = smoothstep(0.0, 1.0, distFF);
	return UnormF(lerp(lerp(productFF, productFC, weight.y), lerp(productCF, productCC, weight.y), weight.x) * kSqrt2);
}

// ---- Simplex noise 2D (reference Random.hlsl) ----
float SimplexNoise(float2 xy, float2 scale, float seed) {
	float2 scaledXY = xy * scale;

	// 2d skew constants.
	static const float F = 0.366025404; // (sqrt(3) - 1) / 2
	static const float G = 0.211324865; // (3 - sqrt(3)) / 6

	float2 skewedID = floor(scaledXY + (scaledXY.x + scaledXY.y) * F);

	float2 skewedToStdOriginDir = scaledXY - (skewedID - (skewedID.x + skewedID.y) * G);

	float2 halfSign = skewedToStdOriginDir.x > skewedToStdOriginDir.y ? float2(1.0, 0.0) : float2(0.0, 1.0);

	float2 skewedToStdAdjacentDir = skewedToStdOriginDir - halfSign + G;

	float2 skewedToStdOppositeDir = skewedToStdOriginDir - 1.0 + 2.0 * G;

	float3 barycenter = {
		dot(skewedToStdOriginDir, skewedToStdOriginDir),
		dot(skewedToStdAdjacentDir, skewedToStdAdjacentDir),
		dot(skewedToStdOppositeDir, skewedToStdOppositeDir)
	};

	float3 invHalfBarycenter = max(0.5 - barycenter, 0.0);

	float hashOri = dot(skewedToStdOriginDir, Snorm2(Hash2D(skewedID, seed)));
	float hashAdj = dot(skewedToStdAdjacentDir, Snorm2(Hash2D(skewedID + halfSign, seed)));
	float hashOpp = dot(skewedToStdOppositeDir, Snorm2(Hash2D(skewedID + 1.0, seed)));

	float3 doubleInvHalfBarycenter = invHalfBarycenter * invHalfBarycenter;

	float3 hashGradiant =
		doubleInvHalfBarycenter * doubleInvHalfBarycenter * float3(hashOri, hashAdj, hashOpp);

	return UnormF(dot(float3(70.0, 70.0, 70.0), hashGradiant));
}

// ---- Worley noise 2D (reference Random.hlsl) ----
float WorleyNoise(float2 xy, float2 scale, float seed) {
	float2 scaledXY = xy * scale;
	float2 scaledFloorXY = floor(scaledXY);

	float minDistance = 1.0;
	[unroll] for (int offsetY = -1; offsetY <= 1; ++offsetY) {
		[unroll] for (int offsetX = -1; offsetX <= 1; ++offsetX) {
			float2 tilePos = scaledFloorXY + float2(offsetX, offsetY);
			float distanceToPoint = distance(scaledXY, float2(offsetX, offsetY) + Hash2D(tilePos, seed) + scaledFloorXY);
			minDistance = min(distanceToPoint, minDistance);
		}
	}
	return minDistance;
}

#endif
