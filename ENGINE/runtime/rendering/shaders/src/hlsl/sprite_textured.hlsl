float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float3 albedo = float3(uv.x, uv.y, 1.0 - uv.x);
    float3 flatNormal = float3(0.0, 0.0, 1.0);
    float3 pseudoNormal = normalize(float3(uv * 2.0 - 1.0, 1.0));
    bool hasNormal = false;
    float3 N = hasNormal ? pseudoNormal : flatNormal;
    float roughness = 1.0;
    float heightBias = 0.0;

    float3 L = normalize(float3(0.4, 0.5, 1.0 + heightBias));
    float3 V = float3(0.0, 0.0, 1.0);
    float3 H = normalize(L + V);

    float lambert = max(dot(N, L), 0.0);
    float specPower = lerp(8.0, 64.0, 1.0 - roughness);
    float spec = pow(max(dot(N, H), 0.0), specPower) * (1.0 - roughness);
    float3 lit = albedo * (0.15 + lambert) + spec.xxx;
    return float4(lit, 1.0);
}
