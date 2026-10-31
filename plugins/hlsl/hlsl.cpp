#include "cyimg_edit_plugin.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <array>
#include <algorithm>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t kMaxShaders = 64;
struct Shader {
    std::string id;
    std::wstring name;
    ComPtr<ID3DBlob> code;
    CyImgEffect effect{};
};
std::vector<Shader> shaders;
std::vector<const CyImgEffect*> effects;
const CyImgParam params[] = {CYIMG_SLIDER("plugin.hlsl.amount", L"Intensity", 0, 100, 100, 1, L"%")};
const char* prefix = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Settings : register(b0) { uint Width; uint Height; float Amount; float Scale; };
SamplerState LinearClamp : register(s0);
)";
const char* suffix = R"(
[numthreads(8,8,1)] void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= Width || tid.y >= Height) return;
    float4 original = Source.Load(int3(tid.xy,0));
    float4 result = CyEffect(original, (float2(tid.xy) + .5) / float2(Width,Height));
    Destination[tid.xy] = saturate(lerp(original, result, Amount));
})";

int Process(size_t index, const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* values) {
    if (!img || index >= shaders.size() || !img->width || !img->height || img->width > 16384 || img->height > 16384) return CYIMG_E_FAIL;
    auto cancelled = [&] { return host && host->is_cancelled && host->is_cancelled(ctx); };
    if (cancelled()) return CYIMG_E_CANCELLED;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> dc;
    D3D_FEATURE_LEVEL feature;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 1, D3D11_SDK_VERSION, &device, &feature, &dc);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, requested, 1, D3D11_SDK_VERSION, &device, &feature, &dc);
    if (FAILED(hr)) return CYIMG_E_FAIL;
    const size_t n = size_t(img->width) * img->height;
    if (n > 64000000) return CYIMG_E_OUTOFMEMORY;
    std::vector<uint8_t> rgba(n * 4);
    for (uint32_t y = 0; y < img->height; ++y) {
        if (cancelled()) return CYIMG_E_CANCELLED;
        for (uint32_t x = 0; x < img->width; ++x) {
            const uint8_t* p = img->source + size_t(y) * img->stride + x * 4;
            uint8_t* q = rgba.data() + (size_t(y) * img->width + x) * 4;
            q[3] = p[3];
            for (int c = 0; c < 3; ++c) q[c] = p[3] ? uint8_t(std::min(255u, unsigned(p[2-c]) * 255 / p[3])) : 0;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = img->width; desc.Height = img->height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA input{rgba.data(), img->width * 4, 0};
    ComPtr<ID3D11Texture2D> src, dst, readback;
    if (FAILED(device->CreateTexture2D(&desc, &input, &src))) return CYIMG_E_OUTOFMEMORY;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &dst))) return CYIMG_E_OUTOFMEMORY;
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return CYIMG_E_OUTOFMEMORY;
    ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ComputeShader> shader; ComPtr<ID3D11Buffer> constants; ComPtr<ID3D11SamplerState> sampler;
    auto* code = shaders[index].code.Get();
    if (FAILED(device->CreateShaderResourceView(src.Get(), nullptr, &srv)) ||
        FAILED(device->CreateUnorderedAccessView(dst.Get(), nullptr, &uav)) ||
        FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) return CYIMG_E_FAIL;
    struct Settings { uint32_t w, h; float amount, scale; } settings{img->width, img->height, float(values[0] / 100), float(img->scale)};
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(settings); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&settings, 0, 0};
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateBuffer(&bd, &init, &constants)) || FAILED(device->CreateSamplerState(&sd, &sampler))) return CYIMG_E_FAIL;
    dc->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = {srv.Get()}; ID3D11UnorderedAccessView* outputs[] = {uav.Get()};
    ID3D11Buffer* buffers[] = {constants.Get()}; ID3D11SamplerState* samplers[] = {sampler.Get()};
    dc->CSSetShaderResources(0, 1, views); dc->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    dc->CSSetConstantBuffers(0, 1, buffers); dc->CSSetSamplers(0, 1, samplers);
    if (cancelled()) return CYIMG_E_CANCELLED;
    dc->Dispatch((img->width + 7) / 8, (img->height + 7) / 8, 1);
    dc->CopyResource(readback.Get(), dst.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(dc->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map))) return CYIMG_E_FAIL;
    const int x0 = std::clamp(img->sel.x, 0, int(img->width)), y0 = std::clamp(img->sel.y, 0, int(img->height));
    const int x1 = int(std::min<int64_t>(img->width, int64_t(img->sel.x) + img->sel.w));
    const int y1 = int(std::min<int64_t>(img->height, int64_t(img->sel.y) + img->sel.h));
    for (int y = y0; y < y1 && !cancelled(); ++y) for (int x = x0; x < x1; ++x) {
        const auto* p = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch + x * 4;
        auto* q = img->data + size_t(y) * img->stride + x * 4;
        q[3] = p[3];
        for (int c = 0; c < 3; ++c) q[c] = uint8_t((unsigned(p[2-c]) * p[3] + 127) / 255);
    }
    dc->Unmap(readback.Get(), 0);
    return cancelled() ? CYIMG_E_CANCELLED : CYIMG_OK;
}
template<size_t I> int Invoke(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* values) {
    try { return Process(I, host, ctx, img, values); } catch (...) { return CYIMG_E_OUTOFMEMORY; }
}
template<size_t... I> constexpr auto Functions(std::index_sequence<I...>) { return std::array{&Invoke<I>...}; }
constexpr auto functions = Functions(std::make_index_sequence<kMaxShaders>{});
CyImgEditPlugin plugin{CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "hlsl", "plugin.hlsl.name", L"HLSL effects", L"1.0"};
void Load() {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    const auto directory = std::filesystem::path(executable).parent_path() / L"plugins" / L"effects";
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        if (entry.is_regular_file(error) && entry.path().extension() == L".hlsl") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    shaders.reserve(kMaxShaders);
    for (const auto& path : files) {
        if (shaders.size() == kMaxShaders) break;
        if (std::filesystem::file_size(path, error) > 1024 * 1024 || error) continue;
        std::ifstream file(path, std::ios::binary);
        std::string source((std::istreambuf_iterator<char>(file)), {});
        source = prefix + source + suffix;
        Shader shader; shader.name = path.stem().wstring(); shader.id = "shader_" + std::to_string(shaders.size());
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader.code, &errors))) {
            std::ofstream log(path.wstring() + L".errors.txt", std::ios::binary);
            if (errors) log.write(static_cast<const char*>(errors->GetBufferPointer()), std::streamsize(errors->GetBufferSize()));
            continue;
        }
        shaders.push_back(std::move(shader));
    }
    for (size_t i = 0; i < shaders.size(); ++i) {
        auto& s = shaders[i];
        s.effect = {sizeof(CyImgEffect), s.id.c_str(), nullptr, s.name.c_str(), "plugin.hlsl.category", L"HLSL",
            params, 1, functions[i]};
        effects.push_back(&s.effect);
    }
    plugin.effects = effects.data(); plugin.effect_count = uint32_t(effects.size());
}
}
extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) {
    static std::once_flag once;
    try { std::call_once(once, Load); } catch (...) { return nullptr; }
    return &plugin;
}
