// Ported 1:1 from reference project ColorConversion.hlsl.
#ifndef COLOR_CONVERSION_HLSL
#define COLOR_CONVERSION_HLSL

float Grayscale(float3 color) {
	return color.r * 0.299 + color.g * 0.587 + color.b * 0.114;
}

float3 Desaturation(float3 color, float value) {
	return lerp(color, Grayscale(color), value);
}

float3 HueToLinearRGB(float hue) {
	float r = abs(hue * 6 - 3) - 1;
	float g = 2 - abs(hue * 6 - 2);
	float b = 2 - abs(hue * 6 - 4);
	return saturate(float3(r, g, b));
}

float3 HSVToLinearRGB(float3 hsv) {
	float3 rgb = HueToLinearRGB(hsv.x);
	return ((rgb - 1) * hsv.y + 1) * hsv.z;
}

float3 RGBToHCV(float3 rgb) {
	float4 p = (rgb.g < rgb.b) ? float4(rgb.bg, -1.0, 2.0 / 3.0) : float4(rgb.gb, 0.0, -1.0 / 3.0);
	float4 q = (rgb.r < p.x) ? float4(p.xyw, rgb.r) : float4(rgb.r, p.yzx);
	float chroma = q.x - min(q.w, q.y);
	float hue = abs((q.w - q.y) / (6.0 * chroma + 1e-10) + q.z);
	return float3(hue, chroma, q.x);
}

float3 LinearRGBToHSV(float3 rgb) {
	float3 hcv = RGBToHCV(rgb);
	float s = hcv.y / (hcv.z + 1e-10);
	return float3(hcv.x, s, hcv.z);
}

float3 HueShift(float3 rgb, float offset) {
	float3 hsv = LinearRGBToHSV(rgb);
	hsv.x = frac(hsv.x + offset);
	return HSVToLinearRGB(hsv);
}

#endif
