#include "gpu_decode.h"
#include "cyimg_plugin_i18n.h"  // messages traduits (état posé par cyimg_tr_begin dans le plugin)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {

// Triangle plein écran + lecture texel à texel (Load : aucun filtrage)
const char kShader[] = R"(
Texture2D<float4> Src : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target {
    return Src.Load(int3(pos.xy, 0));
}
)";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

struct Gpu {
    std::mutex lock;  // le contexte immédiat D3D11 n'est pas multithread
    bool tried = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::wstring error;

    bool Init() {
        if (tried) return device != nullptr;
        tried = true;
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        if (!compile) {
            error = CYIMG_TR("plugin.gpu.err.no_compiler", L"d3dcompiler_47.dll introuvable");
            return false;
        }
        // WARP : créé en quelques millisecondes, sans dépendre du pilote graphique
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                     nullptr, &ctx))) {
            error = CYIMG_TR("plugin.gpu.err.no_warp", L"Direct3D 11 (WARP) indisponible");
            return false;
        }
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(compile(kShader, sizeof(kShader) - 1, "decode", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(compile(kShader, sizeof(kShader) - 1, "decode", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err)) ||
            FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps))) {
            error = CYIMG_TR("plugin.gpu.err.shader", L"compilation du shader de décodage impossible");
            device.Reset();
            return false;
        }
        return true;
    }
};

Gpu& Instance() {
    static Gpu g;
    return g;
}

}  // namespace

// --- BC1 à BC5 en logiciel (rapide, sans Direct3D) ----------------------------------------------

namespace {

inline void Expand565(uint16_t c, uint8_t rgb[3]) {
    uint8_t r = uint8_t(c >> 11), g = uint8_t((c >> 5) & 63), b = uint8_t(c & 31);
    rgb[0] = uint8_t(r << 3 | r >> 2);
    rgb[1] = uint8_t(g << 2 | g >> 4);
    rgb[2] = uint8_t(b << 3 | b >> 2);
}

// Bloc couleur (BC1/2/3) -> 16 pixels RGBA. allowTransparent : mode 3 couleurs + transparent (BC1 seul).
void DecodeColorBlock(const uint8_t* b, uint8_t out[16][4], bool allowTransparent) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    Expand565(c0, pal[0]);
    Expand565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    if (c0 > c1 || !allowTransparent) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k] + 1) / 2);
            pal[3][k] = 0;
        }
        pal[3][3] = 0;
    }
    uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i) memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}

// Bloc d'un canal interpolé (alpha BC3, BC4, BC5) -> 16 valeurs
void DecodeChannelBlock(const uint8_t* b, uint8_t out[16]) {
    uint8_t v[8];
    v[0] = b[0];
    v[1] = b[1];
    if (v[0] > v[1]) {
        for (int i = 1; i < 7; ++i) v[i + 1] = uint8_t(((7 - i) * v[0] + i * v[1] + 3) / 7);
    } else {
        for (int i = 1; i < 5; ++i) v[i + 1] = uint8_t(((5 - i) * v[0] + i * v[1] + 2) / 5);
        v[6] = 0;
        v[7] = 255;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; ++i) idx |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = v[(idx >> (3 * i)) & 7];
}

bool SoftDecodeBlocks(DXGI_FORMAT f, uint32_t width, uint32_t height, const uint8_t* data, std::vector<uint8_t>& out) {
    const uint32_t bb = BlockBytes(f), bw = (width + 3) / 4, bh = (height + 3) / 4;
    out.assign(size_t(width) * height * 4, 0);
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data + (size_t(by) * bw + bx) * bb;
            uint8_t px[16][4];
            switch (f) {
            case DXGI_FORMAT_BC1_UNORM:
                DecodeColorBlock(b, px, true);
                break;
            case DXGI_FORMAT_BC2_UNORM:
                DecodeColorBlock(b + 8, px, false);
                for (int i = 0; i < 16; ++i) {
                    uint8_t a = uint8_t((b[i / 2] >> ((i & 1) * 4)) & 15);
                    px[i][3] = uint8_t(a << 4 | a);
                }
                break;
            case DXGI_FORMAT_BC3_UNORM: {
                uint8_t a[16];
                DecodeChannelBlock(b, a);
                DecodeColorBlock(b + 8, px, false);
                for (int i = 0; i < 16; ++i) px[i][3] = a[i];
                break;
            }
            case DXGI_FORMAT_BC4_UNORM: {
                uint8_t r[16];
                DecodeChannelBlock(b, r);
                for (int i = 0; i < 16; ++i) {
                    px[i][0] = r[i];
                    px[i][1] = px[i][2] = 0;
                    px[i][3] = 255;
                }
                break;
            }
            case DXGI_FORMAT_BC5_UNORM: {
                uint8_t r[16], g[16];
                DecodeChannelBlock(b, r);
                DecodeChannelBlock(b + 8, g);
                for (int i = 0; i < 16; ++i) {
                    px[i][0] = r[i];
                    px[i][1] = g[i];
                    px[i][2] = 0;
                    px[i][3] = 255;
                }
                break;
            }
            default:
                return false;
            }
            for (int i = 0; i < 16; ++i) {
                uint32_t x = bx * 4 + uint32_t(i & 3), y = by * 4 + uint32_t(i >> 2);
                if (x < width && y < height) memcpy(&out[(size_t(y) * width + x) * 4], px[i], 4);
            }
        }
    }
    return true;
}

}  // namespace

uint32_t BlockBytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return 8;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;
    default:
        return 0;
    }
}

bool GpuDecodeBlocks(DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t* data, size_t size,
                     bool floatOut, std::vector<uint8_t>& out, std::wstring& error) {
    const uint32_t bb = BlockBytes(format);
    if (!bb || !width || !height || width > 16384 || height > 16384) {
        error = CYIMG_TR("plugin.gpu.err.invalid", L"dimensions ou format GPU invalides");
        return false;
    }
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (size < size_t(bw) * bh * bb) {
        error = CYIMG_TR("plugin.gpu.err.short_mip", L"données de mip trop courtes");
        return false;
    }
    // BC1 à BC5 : décodeur logiciel (instantané, pas d'attente du verrou global de Direct3D)
    if (!floatOut && SoftDecodeBlocks(format, width, height, data, out)) return true;
    Gpu& g = Instance();
    std::lock_guard<std::mutex> lock(g.lock);
    if (!g.Init()) {
        error = g.error;
        return false;
    }
    // Texture compressée : dimensions arrondies au bloc (exigé par D3D11 pour le niveau 0)
    D3D11_TEXTURE2D_DESC td{};
    td.Width = bw * 4;
    td.Height = bh * 4;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, bw * bb, 0};
    ComPtr<ID3D11Texture2D> src, rt, staging;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    const DXGI_FORMAT outFmt = floatOut ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = width;
    rd.Height = height;
    rd.MipLevels = 1;
    rd.ArraySize = 1;
    rd.Format = outFmt;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC sd = rd;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.device->CreateTexture2D(&td, &init, &src)) || FAILED(g.device->CreateShaderResourceView(src.Get(), nullptr, &srv)) ||
        FAILED(g.device->CreateTexture2D(&rd, nullptr, &rt)) || FAILED(g.device->CreateRenderTargetView(rt.Get(), nullptr, &rtv)) ||
        FAILED(g.device->CreateTexture2D(&sd, nullptr, &staging))) {
        error = CYIMG_TR("plugin.gpu.err.textures", L"création des textures Direct3D impossible (mémoire ?)");
        return false;
    }
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
    ID3D11ShaderResourceView* srvs[] = {srv.Get()};
    g.ctx->OMSetRenderTargets(1, rtvs, nullptr);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, srvs);
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[] = {nullptr};
    g.ctx->PSSetShaderResources(0, 1, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx->CopyResource(staging.Get(), rt.Get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        error = CYIMG_TR("plugin.gpu.err.readback", L"lecture du résultat impossible");
        return false;
    }
    const size_t px = floatOut ? 16 : 4, row = size_t(width) * px;
    out.resize(row * height);
    for (uint32_t y = 0; y < height; ++y) memcpy(out.data() + y * row, static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, row);
    g.ctx->Unmap(staging.Get(), 0);
    return true;
}
