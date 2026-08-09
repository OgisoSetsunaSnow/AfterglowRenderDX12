// ParticleExample 粒子 GS（公告板）：把每个 POINT 扩展成以点为中心的屏幕空间四边形（2 三角形），
// 输出点中心像素坐标 + 点直径给 FS 画圆（等效参考 gl_PointSize 点精灵，D3D12 更可靠）。
// 注意：pointSize 是"直径"（像素），圆半径 = pointSize/2。
cbuffer ParticleRenderParams : register(b0) {
    float4x4 viewProjection;
    float2 screenResolution;
    float2 invScreenResolution;
    float cameraFov;
    float _pad0;
    float2 _pad1;
};

struct VSOutput {
    float4 clipPos : SV_POSITION;
    float3 velocity : VELOCITY;
    float4 color : COLOR;
    float pointSize : POINTSIZE;
};

struct GSOutput {
    float4 position : SV_POSITION;
    float3 velocity : VELOCITY;
    float4 color : COLOR;
    nointerpolation float2 centerPosition : CENTERPOSITION;
    nointerpolation float transferPointSize : TRANSFERPOINTSIZE;
};

[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<GSOutput> stream) {
    float4 P = input[0].clipPos;
    float2 ndcCenter = P.xy / P.w;
    // 点中心像素坐标（FS 用）
    float2 center = (ndcCenter * 0.5 + 0.5) * screenResolution;
    // 点直径（像素）→ 半径。clamp 上限 10px（直径 20px）小粒子避免贴相机巨大 quad。
    float radiusPx = min(input[0].pointSize * 0.5, 10.0);
    // 显式标量除法：NDC 偏移 = 2 * 像素半径 / 屏幕分辨率（X/Y 分别计算）。
    float2 ndcHalf;
    ndcHalf.x = 2.0 * radiusPx / screenResolution.x;
    ndcHalf.y = 2.0 * radiusPx / screenResolution.y;
    // 防御：粒子在相机后方（P.w < 0）时跳过。
    if (P.w <= 0.0) {
        return;
    }

    // 4 角展开成 quad（2 三角形）。用红色方块测试验证过的原始顺序（CULL_BACK + 此顺序 quad 完整可见）。
    const float2 corners[4] = {
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0,  1.0), float2(1.0,  1.0)
    };
    GSOutput o;
    o.velocity = input[0].velocity;
    o.color = input[0].color;
    o.centerPosition = center;
    // 关键：transferPointSize 必须是 quad 实际直径（2*radiusPx，含 clamp）。
    // 之前传未 clamp 的 pointSize（≤30 → 圆边界 15px > quad 半径 12.5px）→ quad 全填充 → 矩形。
    o.transferPointSize = 2.0 * ndcHalf.x * screenResolution.x; // 直径（像素），FS 圆边界 = 直径/2 = quad 半径
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float2 ndc = ndcCenter + corners[i] * ndcHalf;
        o.position = float4(ndc * P.w, P.z, P.w);
        stream.Append(o);
    }
    // 单 quad 不用 RestartStrip（4 顶点 TriangleStrip 自动形成 2 三角形）。
}