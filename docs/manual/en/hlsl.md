---
id: hlsl
title: Write an HLSL effect
description: Write an HLSL effect — CyImgView.
section: Guides
order: 200
revision: 1
---

## Shader contract {#contract}

Place `.hlsl` files in `plugins/effects/` beside the executable, then restart. The gallery loads up to 64 files in alphabetical order. Each becomes an effect with an intensity slider. Invalid shaders are skipped; diagnostics go into `name.hlsl.errors.txt`.

```hlsl
float4 CyEffect(float4 color, float2 uv) {
    return float4(1.0 - color.rgb, color.a);
}
```

`color` and the result are straight RGBA in 0–1. `uv` is the normalized pixel center. The host handles intensity, premultiplied BGRA conversion, selection, scaled preview, full rendering and undo. Neighbor sampling is available:

```hlsl
float4 CyEffect(float4 color, float2 uv) {
    float2 pixel = float2(1.0 / Width, 1.0 / Height);
    return (Source.SampleLevel(LinearClamp, uv - pixel, 0) + color
          + Source.SampleLevel(LinearClamp, uv + pixel, 0)) / 3.0;
}
```

Available resources: `Texture2D<float4> Source`, `SamplerState LinearClamp`, `uint Width`, `uint Height`, `float Amount` (0–1), and `float Scale` (preview scale). Multiply document-pixel distances by Scale. Do not declare `main` or the host resources. Includes are unsupported. Compilation targets `cs_5_0`; execution uses D3D11 with WARP fallback. Cancellation is checked before and after GPU execution; a running dispatch is not interrupted.

Native tools, effects, modes and future AI extensions use `include/cyimg_edit_plugin.h` and `CyImg_GetEditPlugin`. Place DLLs in `plugins/`. No model or AI service is bundled.
