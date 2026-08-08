#ifndef ACES_TONESCALE_HLSL
#define ACES_TONESCALE_HLSL

// Tonescale forward（照搬参考 Chromatics/ACESTonescale.hlsl，参数由 ACESOutParams 提供）。
float TonescaleForward(float x) {
	// forward MM tone scale
	float f = ACESOutParams[0].tonescale_m_2 * pow(max(0.0, x) / (x + ACESOutParams[0].tonescale_s_2), ACESOutParams[0].tonescale_g);
	float h = max(0.0, f * f / (f + ACESOutParams[0].tonescale_t_1)); // forward flare
	return h * ACESOutParams[0].tonescale_n_r; // output is luminance in cd/m^2
}

#endif
