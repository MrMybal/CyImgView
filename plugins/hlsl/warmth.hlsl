// Input/output: straight RGBA, [0,1]. Source is available for neighboring pixels.
float4 CyEffect(float4 color, float2 uv) {
    return float4(saturate(color.rgb * float3(1.08, 1.0, 0.90)), color.a);
}
