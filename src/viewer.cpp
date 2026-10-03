#include "viewer.h"
#include "i18n.h"
#include "version.h"
#include "viewer_util.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <future>
#include <mutex>

namespace {

constexpr UINT WM_APP_LOADED = WM_APP + 1;
constexpr UINT WM_APP_PREUPLOAD = WM_APP + 2;
constexpr UINT WM_APP_GPU_READY = WM_APP + 3;
constexpr UINT_PTR TIMER_HUD = 1;
constexpr wchar_t kClassName[] = L"CyImgViewWindow";

using namespace ui;

void OrientationToFlipRot(uint32_t o, bool& flip, int& rot) {
    static const bool f[9] = {false, false, true, false, true, true, false, true, false};
    static const int r[9] = {0, 0, 0, 2, 2, 3, 1, 1, 3};
    if (o > 8) o = 1;
    flip = f[o];
    rot = r[o];
}

// Remplace le point décimal (printf) par le séparateur de la langue (virgule en français)
std::wstring LocalDecimal(std::wstring s) {
    const wchar_t* sep = T("viewer.decimal");
    if (sep[0] && !sep[1]) std::replace(s.begin(), s.end(), L'.', sep[0]);
    return s;
}

std::wstring FormatBytes(uint64_t b) {
    if (b < 1024) return Tf("viewer.unit.bytes", b);
    if (b < 1024 * 1024) return LocalDecimal(Tf("viewer.unit.kb", b / 1024.0));
    if (b < 1024ull * 1024 * 1024) return LocalDecimal(Tf("viewer.unit.mb", b / (1024.0 * 1024.0)));
    return LocalDecimal(Tf("viewer.unit.gb", b / (1024.0 * 1024.0 * 1024.0)));
}

// Date et heure au format régional de Windows
std::wstring FormatDate(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    wchar_t date[64] = {}, time[32] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, ARRAYSIZE(date), nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time, ARRAYSIZE(time));
    return std::wstring(date) + L" " + time;
}

std::wstring Percent(double z) {
    double p = z * 100.0;
    return LocalDecimal(p < 10 ? Tf("viewer.zoom.pct1", p) : Tf("viewer.zoom.pct0", p));
}


ComPtr<ID3D11Device> CreateD3DDevice(D3D_DRIVER_TYPE type) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    ComPtr<ID3D11Device> dev;
    if (FAILED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
        dev.Reset();
    return dev;
}

// Préchauffage du GPU : le device matériel est créé sur un thread dès le lancement. S'il n'est pas
// prêt pour la première image, on dessine avec WARP (rendu logiciel, créé en quelques ms) et la
// fenêtre reçoit WM_APP_GPU_READY pour basculer sur le GPU.
// WARP est créé avant de lancer le thread : en parallèle, il attendrait le verrou pris par le pilote.
std::future<ComPtr<ID3D11Device>> g_warmDevice;
ComPtr<ID3D11Device> g_warpDevice;
std::mutex g_warmMutex;
HWND g_warmNotify = nullptr;
bool g_warmDone = false;

ComPtr<ID3D11Device> TakeWarmDevice(HWND hwnd, bool& usingWarp) {
    usingWarp = false;
    ComPtr<ID3D11Device> warp = std::move(g_warpDevice);
    if (g_warmDevice.valid()) {
        if (warp && g_warmDevice.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            std::lock_guard<std::mutex> lock(g_warmMutex);
            g_warmNotify = hwnd;
            if (g_warmDone) PostMessageW(hwnd, WM_APP_GPU_READY, 0, 0);
            usingWarp = true;
            return warp;
        }
        if (ComPtr<ID3D11Device> dev = g_warmDevice.get()) return dev;
    }
    // Après une perte de device, ou si le préchauffage a échoué
    if (ComPtr<ID3D11Device> dev = CreateD3DDevice(D3D_DRIVER_TYPE_HARDWARE)) return dev;
    return CreateD3DDevice(D3D_DRIVER_TYPE_WARP);
}

}  // namespace

void StartGpuWarmup() {
    g_warpDevice = CreateD3DDevice(D3D_DRIVER_TYPE_WARP);
    g_warmDevice = std::async(std::launch::async, [] {
        ComPtr<ID3D11Device> dev = CreateD3DDevice(D3D_DRIVER_TYPE_HARDWARE);
        std::lock_guard<std::mutex> lock(g_warmMutex);
        g_warmDone = true;
        if (g_warmNotify) PostMessageW(g_warmNotify, WM_APP_GPU_READY, 0, 0);
        return dev;
    });
}

// =============================================================================================
// Création / boucle

Viewer::~Viewer() { m_loader.reset(); }

bool Viewer::Create(HINSTANCE inst, int show, const std::wstring& initialPath) {
    m_inst = inst;
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    m_exeDir = ParentOf(exe);

    m_iniPath = SettingsIniPath();

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(m_factory.GetAddressOf()))) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf())))) {
        MessageBoxW(nullptr, T("viewer.err.direct2d"), L"CyImgView", MB_ICONERROR);
        return false;
    }

    // Géométrie du spinner : arc de 270° de rayon 1
    m_factory->CreatePathGeometry(&m_spinnerGeo);
    ComPtr<ID2D1GeometrySink> sink;
    if (m_spinnerGeo && SUCCEEDED(m_spinnerGeo->Open(&sink))) {
        sink->BeginFigure(D2D1::Point2F(1, 0), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(0, -1), D2D1::SizeF(1, 1), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
    }
    D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
    m_factory->CreateStrokeStyle(ssp, nullptr, 0, &m_roundStroke);

    Trace(L"Create: début");
    m_decoder.Init(m_exeDir);
    Trace(L"Decoder::Init (plugins + codecs WIC)");

    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        m_budget = size_t(std::clamp<uint64_t>(ms.ullTotalPhys / 5, 512ull << 20, 3ull << 30));
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    m_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClassName, L"CyImgView", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, MulDiv(1280, dpi, 96), MulDiv(820, dpi, 96), nullptr, nullptr, inst, this);
    if (!m_hwnd) return false;
    Trace(L"fenêtre créée");

    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(m_hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(m_hwnd, 19, &dark, sizeof(dark));

    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    CreateTextFormats();

    m_loader = std::make_unique<Loader>(m_decoder, m_hwnd, WM_APP_LOADED);
    m_loader->SetThumbSize(std::min(320u, UINT(160 * m_scale)));

    LoadSettings();
    if (m_startMaximized && (show == SW_SHOWDEFAULT || show == SW_SHOWNORMAL)) show = SW_SHOWMAXIMIZED;
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;  // la position restaurée peut être sur un autre écran
    CreateTextFormats();
    m_lastActivity = NowSeconds();
    m_lastFrame = NowSeconds();

    // Le décodage démarre avant même que la fenêtre ne s'affiche.
    if (!initialPath.empty()) OpenPath(initialPath);
    Trace(L"OpenPath (listing du dossier)");

    ShowWindow(m_hwnd, show);
    UpdateWindow(m_hwnd);
    Trace(L"premier rendu (fenêtre affichée)");
    SetTimer(m_hwnd, TIMER_HUD, 200, nullptr);

    // Mises à jour : nettoyage d'une installation précédente, puis recherche automatique
    // (au plus une fois toutes les 20 h, quelques secondes après le démarrage pour ne rien ralentir)
    CleanupUpdateLeftovers(m_exeDir);
    if (!m_iniPath.empty() && m_autoUpdate) {
        wchar_t last[32] = {};
        GetPrivateProfileStringW(L"update", L"last", L"0", last, 32, m_iniPath.c_str());
        if (std::time(nullptr) - _wtoi64(last) > 20 * 3600) SetTimer(m_hwnd, kTimerUpdate, 4000, nullptr);
    }
    return true;
}

int Viewer::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

void Viewer::LoadSettings() {
    if (m_iniPath.empty()) return;
    const wchar_t* f = m_iniPath.c_str();
    m_showStrip = GetPrivateProfileIntW(L"view", L"strip", 1, f) != 0;
    m_singleInstance = GetPrivateProfileIntW(L"settings", L"single_instance", 0, f) != 0;
    m_autoUpdate = GetPrivateProfileIntW(L"update", L"auto", 1, f) != 0;
    m_slideDelay = std::clamp(int(GetPrivateProfileIntW(L"slideshow", L"delay", 5, f)), 1, 60);
    m_slideShuffle = GetPrivateProfileIntW(L"slideshow", L"shuffle", 0, f) != 0;
    m_filter = std::clamp(int(GetPrivateProfileIntW(L"view", L"filter", 0, f)), 0, 5);
    int x = GetPrivateProfileIntW(L"window", L"x", INT_MIN, f);
    int y = GetPrivateProfileIntW(L"window", L"y", INT_MIN, f);
    int w = GetPrivateProfileIntW(L"window", L"w", 0, f);
    int h = GetPrivateProfileIntW(L"window", L"h", 0, f);
    int mx = GetPrivateProfileIntW(L"window", L"max", 0, f);
    if (x == INT_MIN || y == INT_MIN || w < 200 || h < 150) return;
    RECT r{x, y, x + w, y + h};
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    wp.rcNormalPosition = r;
    wp.showCmd = SW_HIDE;  // la fenêtre est affichée plus tard par Create()
    SetWindowPlacement(m_hwnd, &wp);
    m_startMaximized = mx != 0;
}

void Viewer::SaveSettings() {
    if (m_iniPath.empty()) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (m_fullscreen) wp = m_wpPrev;
    else GetWindowPlacement(m_hwnd, &wp);
    const wchar_t* f = m_iniPath.c_str();
    auto put = [&](const wchar_t* sec, const wchar_t* k, int v) {
        WritePrivateProfileStringW(sec, k, std::to_wstring(v).c_str(), f);
    };
    RECT r = wp.rcNormalPosition;
    put(L"window", L"x", r.left);
    put(L"window", L"y", r.top);
    put(L"window", L"w", r.right - r.left);
    put(L"window", L"h", r.bottom - r.top);
    put(L"window", L"max", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    put(L"view", L"strip", m_showStrip ? 1 : 0);
}

// =============================================================================================
// Ressources graphiques

void Viewer::CreateTextFormats() {
    auto make = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, DWRITE_TEXT_ALIGNMENT align,
                    ComPtr<IDWriteTextFormat>& out, bool trim) {
        out.Reset();
        m_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   size * m_scale, L"fr-fr", &out);
        if (!out) return;
        out->SetTextAlignment(align);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (trim) {
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ell;
            m_dwrite->CreateEllipsisTrimmingSign(out.Get(), &ell);
            out->SetTrimming(&t, ell.Get());
        }
    };
    const auto L = DWRITE_TEXT_ALIGNMENT_LEADING;
    const auto C = DWRITE_TEXT_ALIGNMENT_CENTER;
    make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 15, L, m_tfTitle, true);
    make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 12.5f, L, m_tfSub, true);
    make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 13, L, m_tfSmall, true);
    make(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 13, C, m_tfCenter, false);
    make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13, C, m_tfLabel, true);
    make(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 22, C, m_tfToast, true);
    make(L"Segoe UI Light", DWRITE_FONT_WEIGHT_LIGHT, 22, C, m_tfBig, false);
    make(L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 15, C, m_tfIcon, false);
    make(L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 18, C, m_tfIconMid, false);
    make(L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 52, C, m_tfIconBig, false);
}

bool Viewer::CreateTargetBitmap() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(m_swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &m_target))) return false;
    m_dc->SetTarget(m_target.Get());
    return true;
}

void Viewer::ResizeSwapChain(UINT w, UINT h) {
    if (!m_swap || !w || !h) return;
    m_dc->SetTarget(nullptr);
    m_target.Reset();
    if (FAILED(m_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)) || !CreateTargetBitmap()) DiscardDeviceResources();
}

bool Viewer::CreateDeviceResources() {
    if (m_rt) return true;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;

    m_d3d = TakeWarmDevice(m_hwnd, m_usingWarp);
    Trace(m_usingWarp ? L"  device WARP (GPU pas encore prêt)" : L"  device GPU");
    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> dxgiFactory;
    if (!m_d3d || FAILED(m_d3d.As(&dxgiDevice)) || FAILED(m_factory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice)) ||
        FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dc)) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
        DiscardDeviceResources();
        return false;
    }
    dxgiDevice->SetMaximumFrameLatency(1);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(rc.right);
    sd.Height = UINT(rc.bottom);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(dxgiFactory->CreateSwapChainForHwnd(m_d3d.Get(), m_hwnd, &sd, nullptr, nullptr, &m_swap))) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // Windows plus ancien
        if (FAILED(dxgiFactory->CreateSwapChainForHwnd(m_d3d.Get(), m_hwnd, &sd, nullptr, nullptr, &m_swap))) {
            DiscardDeviceResources();
            return false;
        }
    }
    dxgiFactory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!CreateTargetBitmap()) {
        DiscardDeviceResources();
        return false;
    }
    m_dc->SetDpi(96.0f, 96.0f);
    m_rt = m_dc;
    m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    m_rt->CreateSolidColorBrush(Rgb(kWhite), &m_brush);

    D2D1_GRADIENT_STOP stops[2] = {{0.0f, D2D1::ColorF(0, 0, 0, 0.72f)}, {1.0f, D2D1::ColorF(0, 0, 0, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    if (SUCCEEDED(m_rt->CreateGradientStopCollection(stops, 2, &coll))) {
        m_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(0, 1)),
                                        coll.Get(), &m_shade);
    }

    // Damier pour les images transparentes
    uint32_t px[16 * 16];
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) px[y * 16 + x] = ((x / 8 + y / 8) & 1) ? 0xFF2E2F33 : 0xFF3A3B40;
    ComPtr<ID2D1Bitmap> cb;
    if (SUCCEEDED(m_rt->CreateBitmap(D2D1::SizeU(16, 16), px, 16 * 4,
                                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                                     &cb))) {
        m_rt->CreateBitmapBrush(cb.Get(),
                                D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                &m_checker);
        if (m_checker) m_checker->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
    }
    return true;
}

void Viewer::DiscardDeviceResources() {
    DropEditGpu();  // perte du GPU : le document (textures des calques) est perdu avec lui
    m_gpu.clear();
    m_preuploadsLeft = 2;
    m_gpuThumbs.clear();
    m_channelFx.Reset();
    m_logo.Reset();
    m_chanPreview.Reset();
    m_chanPreviewSrc.reset();
    m_checker.Reset();
    m_shade.Reset();
    m_brush.Reset();
    if (m_dc) m_dc->SetTarget(nullptr);
    m_rt.Reset();
    m_target.Reset();
    m_dc.Reset();
    m_swap.Reset();
    m_d2dDevice.Reset();
    m_d3d.Reset();
}

Viewer::GpuImage* Viewer::GetGpuImage(const ImagePtr& img, const std::wstring& key) {
    for (auto& g : m_gpu) {
        if (g.key == key && g.src == img) {
            g.lastUse = ++m_tick;
            return &g;
        }
    }
    if (!m_rt || !img || !img->pixels.Data()) return nullptr;

    // Une seule image courante en vidéo + 2 voisines préchargées.
    const std::wstring curKey = Current() ? Key(Current()->path) : std::wstring();
    while (m_gpu.size() >= 3) {
        auto victim = m_gpu.end();
        for (auto it = m_gpu.begin(); it != m_gpu.end(); ++it)
            if (it->key != curKey && (victim == m_gpu.end() || it->lastUse < victim->lastUse)) victim = it;
        if (victim == m_gpu.end()) break;
        m_gpu.erase(victim);
    }

    GpuImage g;
    g.key = key;
    g.src = img;
    g.lastUse = ++m_tick;
    const UINT32 maxSz = std::max<UINT32>(1024, m_dc->GetMaximumBitmapSize());
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, img->hasAlpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE));
    for (UINT32 y = 0; y < img->height; y += maxSz) {
        for (UINT32 x = 0; x < img->width; x += maxSz) {
            UINT32 tw = std::min(maxSz, img->width - x), th = std::min(maxSz, img->height - y);
            Tile t;
            const uint8_t* p = img->pixels.Data() + size_t(y) * img->stride + size_t(x) * 4;
            if (FAILED(m_rt->CreateBitmap(D2D1::SizeU(tw, th), p, img->stride, bp, &t.bmp))) return nullptr;
            t.dst = D2D1::RectF(float(x), float(y), float(x + tw), float(y + th));
            g.tiles.push_back(std::move(t));
        }
    }
    m_gpu.push_back(std::move(g));
    return &m_gpu.back();
}

ID2D1Bitmap* Viewer::GetGpuThumb(const std::wstring& key) {
    auto tc = m_thumbCache.find(key);
    if (tc == m_thumbCache.end() || !tc->second.img) return nullptr;
    tc->second.lastUse = ++m_tick;
    const ImagePtr& img = tc->second.img;
    auto it = m_gpuThumbs.find(key);
    if (it != m_gpuThumbs.end() && it->second.src == img) {
        it->second.lastUse = m_tick;
        return it->second.bmp.Get();
    }
    if (!m_rt) return nullptr;
    if (m_gpuThumbs.size() > 500) {
        std::vector<std::pair<uint64_t, std::wstring>> v;
        for (auto& [k, t] : m_gpuThumbs) v.emplace_back(t.lastUse, k);
        std::nth_element(v.begin(), v.begin() + 150, v.end());
        for (size_t i = 0; i < 150; ++i) m_gpuThumbs.erase(v[i].second);
    }
    GpuThumb t;
    t.src = img;
    t.lastUse = m_tick;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, img->hasAlpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(m_rt->CreateBitmap(D2D1::SizeU(img->width, img->height), img->pixels.Data(), img->stride, bp, &t.bmp)))
        return nullptr;
    ID2D1Bitmap* raw = t.bmp.Get();
    m_gpuThumbs[key] = std::move(t);
    return raw;
}

void Viewer::PreUploadNeighbor() {
    m_preuploadPending = false;
    if (!m_rt || m_index < 0 || m_preuploadsLeft <= 0) return;
    // Courante + 2 voisines = la capacité du cache GPU (3) : sinon chaque envoi évince une voisine
    // que le message suivant renverrait, en boucle.
    for (int d : {m_lastDir, -m_lastDir}) {
        int i = m_index + d;
        if (i < 0 || i >= int(m_files.size())) continue;
        std::wstring key = Key(m_files[i].path);
        auto it = m_fullCache.find(key);
        if (it == m_fullCache.end()) continue;
        bool present = std::any_of(m_gpu.begin(), m_gpu.end(), [&](const GpuImage& g) { return g.key == key && g.src == it->second; });
        if (present) continue;
        if (uint64_t(it->second->width) * it->second->height > 80'000'000ull) continue;
        GetGpuImage(it->second, key);
        --m_preuploadsLeft;
        m_preuploadPending = true;
        PostMessageW(m_hwnd, WM_APP_PREUPLOAD, 0, 0);  // une image par message : l'UI reste réactive
        return;
    }
}

// =============================================================================================
// Rendu

void Viewer::SetColor(uint32_t rgb, float a) { m_brush->SetColor(Rgb(rgb, a)); }

void Viewer::FillRound(const D2D1_RECT_F& r, float radius, uint32_t rgb, float a) {
    SetColor(rgb, a);
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), m_brush.Get());
}

void Viewer::Text(const std::wstring& s, IDWriteTextFormat* tf, const D2D1_RECT_F& r, uint32_t rgb, float a) {
    if (!tf || s.empty()) return;
    SetColor(rgb, a);
    m_rt->DrawTextW(s.c_str(), UINT32(s.size()), tf, r, m_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

bool Viewer::HudWanted() const {
    if (m_slideshow) {  // diaporama : barre visible seulement quand la souris bouge ou la survole
        if (NowSeconds() - m_lastActivity < 2.0) return true;
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        return m_mouseInside && m_mouse.y > rc.bottom - 150 * m_scale;
    }
    if (m_edit.active || m_showSettings) return true;
    if (!m_cur && !m_placeholder && m_files.empty()) return true;
    if (m_dragging) return false;
    if (NowSeconds() - m_lastActivity < 2.0) return true;
    if (m_mouseInside) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        if (m_mouse.y < 52 * m_scale) return true;
        if (StripVisible() && m_mouse.y > rc.bottom - 110 * m_scale) return true;
    }
    return false;
}

void Viewer::Render() {
    if (!m_rt) {
        Trace(L"  création du render target...");
        if (!CreateDeviceResources()) return;
        Trace(L"  render target créé");
    }
    const double now = NowSeconds();
    const double dt = std::clamp(now - m_lastFrame, 0.0, 0.1);
    m_lastFrame = now;

    bool animating = false;
    const double k = 1.0 - std::exp(-dt * 16.0);
    auto approach = [&](double& v, double t, double eps) {
        if (std::fabs(v - t) > eps) {
            v += (t - v) * k;
            if (std::fabs(v - t) <= eps) v = t;
            else animating = true;
        } else {
            v = t;
        }
    };
    approach(m_zoom, m_zoomT, m_zoomT * 5e-4);
    approach(m_panX, m_panXT, 0.1);
    approach(m_panY, m_panYT, 0.1);
    const bool viewAnimating = animating;

    const double ks = 1.0 - std::exp(-dt * 14.0);
    if (std::fabs(m_stripPos - m_stripPosT) > 0.002) {
        m_stripPos += (m_stripPosT - m_stripPos) * ks;
        animating = true;
    } else {
        m_stripPos = m_stripPosT;
    }

    const float hudT = HudWanted() ? 1.0f : 0.0f;
    if (m_hud != hudT) {
        float step = float(dt / 0.18);
        m_hud = hudT > m_hud ? std::min(hudT, m_hud + step) : std::max(hudT, m_hud - step);
        animating = true;
    }
    if (now - m_toastTime < m_toastDur) animating = true;
    if (m_loading) animating = true;  // spinner
    if (m_edit.textActive || m_edit.renaming >= 0) animating = true;  // curseur de texte clignotant
    m_prevEditButtons.swap(m_editButtons);    // survol : boutons de l'image précédente
    m_editButtons.clear();                    // reconstruits à chaque image (overlay + barres)

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float cw = float(rc.right), ch = float(rc.bottom);

    if (m_showChannels && !m_edit.active) UpdateChannelPreview();
    if (m_edit.active) PrepareEditFrame();  // composition des calques, aperçu d'effet, miniatures
    m_rt->BeginDraw();
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    m_rt->Clear(Rgb(kBg));
    if (m_slideshow && !m_edit.active) {
        if (DrawSlideshow(cw, ch)) animating = true;  // fondu en cours
    } else {
        DrawImage(cw, ch, viewAnimating);
    }
    DrawHud(cw, ch);
    HRESULT hr = m_rt->EndDraw();
    HRESULT hp = SUCCEEDED(hr) ? m_swap->Present(1, 0) : S_OK;
    static bool firstShown = false;
    if (m_cur && !firstShown) {
        firstShown = true;
        Trace(L"première image affichée en pleine résolution");
    }
    if (hr == D2DERR_RECREATE_TARGET || hp == DXGI_ERROR_DEVICE_REMOVED || hp == DXGI_ERROR_DEVICE_RESET) {
        DiscardDeviceResources();
        animating = true;
    }
    // Fenêtre invisible (réduite, écran verrouillé) : Present ne bloque plus, inutile d'animer.
    m_occluded = hp == DXGI_STATUS_OCCLUDED;
    if (m_occluded) return;

    if (animating) {
        Invalidate();
    } else if (!m_preuploadPending) {
        m_preuploadPending = true;
        PostMessageW(m_hwnd, WM_APP_PREUPLOAD, 0, 0);
    }
}

void Viewer::DrawSpinner(float cx, float cy, float r, float alpha) {
    if (!m_spinnerGeo) return;
    float angle = float(std::fmod(NowSeconds() * 400.0, 360.0));
    SetColor(kWhite, 0.85f * alpha);
    m_rt->SetTransform(D2D1::Matrix3x2F::Scale(r, r) * D2D1::Matrix3x2F::Rotation(angle) *
                       D2D1::Matrix3x2F::Translation(cx, cy));
    m_rt->DrawGeometry(m_spinnerGeo.Get(), m_brush.Get(), 3.0f * m_scale / r, m_roundStroke.Get());
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Viewer::DrawCenteredMessage(float cw, float ch, const wchar_t* glyph, const std::wstring& line1,
                                 const std::wstring& line2) {
    const float s = m_scale;
    float cy = ch / 2;
    Text(glyph, m_tfIconBig.Get(), D2D1::RectF(0, cy - 110 * s, cw, cy - 30 * s), kGrey, 0.6f);
    Text(line1, m_tfBig.Get(), D2D1::RectF(20 * s, cy - 20 * s, cw - 20 * s, cy + 16 * s), kWhite, 0.9f);
    Text(line2, m_tfCenter.Get(), D2D1::RectF(20 * s, cy + 18 * s, cw - 20 * s, cy + 44 * s), kGrey, 0.9f);
}

void Viewer::DrawImage(float cw, float ch, bool animating) {
    if (m_edit.active && !m_edit.layers.empty()) {  // document en cours d'édition
        float left, top, z;
        ImageRect(left, top, z);
        if (!animating) {
            left = std::round(left);
            top = std::round(top);
        }
        D2D1_RECT_F r = D2D1::RectF(left, top, left + m_edit.w * z, top + m_edit.h * z);
        if (m_checker) m_rt->FillRectangle(r, m_checker.Get());  // calques transparents, gomme
        // pendant un tracé : filtrage linéaire (pas de recalcul des niveaux de réduction à chaque segment)
        D2D1_INTERPOLATION_MODE mode = FilterFor(z, animating || m_dragging || m_edit.drawing);
        DrawEditDocument(r, mode);
        DrawEditOverlay(left, top, z);
        return;
    }
    ImagePtr img = Shown();
    const float s = m_scale;
    if (!img) {
        if (!m_error.empty()) {
            DrawCenteredMessage(cw, ch, L"\uE783", T("viewer.msg.open_failed"), m_error);
        } else if (m_loading) {
            if (NowSeconds() - m_loadStart > 0.12) DrawSpinner(cw / 2, ch / 2, 18 * s, 1.0f);
        } else if (m_files.empty()) {
            if (ID2D1Bitmap* logo = Logo()) {  // accueil : logo à la place de l'icône
                const float r = 50 * s, cy = ch / 2 - 80 * s;
                m_dc->DrawBitmap(logo, D2D1::RectF(cw / 2 - r, cy - r, cw / 2 + r, cy + r), 0.95f,
                                 D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                DrawCenteredMessage(cw, ch, L"", T("viewer.msg.drop"), T("viewer.msg.drop_sub"));
            } else {
                DrawCenteredMessage(cw, ch, L"\uE8B9", T("viewer.msg.drop"), T("viewer.msg.drop_sub"));
            }
        }
        return;
    }
    const FileEntry* cur = Current();
    if (!cur) return;
    const std::wstring key = Key(cur->path);
    const bool isFull = img == m_cur;
    SyncAnimation();

    GpuImage* g = nullptr;
    ID2D1Bitmap* single = nullptr;
    if (isFull) g = GetGpuImage(img, key);
    else single = GetGpuThumb(key);
    if (!g && !single) return;
    if (g) SyncGpuFrame(g);

    float W = float(isFull ? img->width : (img->fullWidth ? img->fullWidth : img->width));
    float H = float(isFull ? img->height : (img->fullHeight ? img->fullHeight : img->height));
    bool flip;
    int rot;
    FlipRot(flip, rot);
    const float dw = (rot & 1) ? H : W, dh = (rot & 1) ? W : H;
    const float z = float(m_zoom);

    double left = cw / 2.0 + m_panX - dw * z / 2.0;
    double top = ch / 2.0 + m_panY - dh * z / 2.0;
    if (!animating) {  // alignement au pixel : net à 100 %
        left = std::round(left);
        top = std::round(top);
    }
    const float tx = float(left + dw * z / 2.0), ty = float(top + dh * z / 2.0);

    if (img->hasAlpha && m_checker && (ChannelFull(m_chanMask) || ChannelKeepsAlpha(m_chanMask))) {
        m_rt->FillRectangle(D2D1::RectF(float(left), float(top), float(left + dw * z), float(top + dh * z)), m_checker.Get());
    }

    const D2D1_INTERPOLATION_MODE mode = FilterFor(z, !isFull || animating || m_dragging);

    m_dc->SetTransform(ImageTransform(W, H, flip, rot, z, tx, ty));
    if (g) {
        for (auto& t : g->tiles) DrawTile(t.bmp.Get(), t.dst, mode);
    } else {
        DrawTile(single, D2D1::RectF(0, 0, W, H), m_filter == 1 ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_LINEAR);
    }
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());

    if (m_loading && !isFull && NowSeconds() - m_loadStart > 0.12) DrawSpinner(cw / 2, ch / 2, 16 * s, 1.0f);
}

void Viewer::DrawStrip(float cw, float ch, float alpha) {
    const float s = m_scale;
    const float cellW = 92 * s, cellH = 66 * s, gap = 6 * s, pitch = cellW + gap;
    const float y0 = ch - 14 * s - cellH;
    const int n = int(m_files.size());
    const int half = int(cw / 2 / pitch) + 2;
    const int lo = std::max(0, int(std::floor(m_stripPos)) - half);
    const int hi = std::min(n - 1, int(std::ceil(m_stripPos)) + half);
    const int hover = (m_mouseInside && !m_dragging) ? HitStrip(m_mouse) : -1;

    for (int i = lo; i <= hi; ++i) {
        float cx = cw / 2 + float(i - m_stripPos) * pitch;
        D2D1_RECT_F r = D2D1::RectF(cx - cellW / 2, y0, cx + cellW / 2, y0 + cellH);
        if (r.right < 0 || r.left > cw) continue;
        const bool isCur = i == m_index;
        float op = alpha * (isCur ? 1.0f : (i == hover ? 0.95f : 0.62f));
        FillRound(r, 4 * s, 0x26272B, 0.9f * alpha);

        std::wstring key = Key(m_files[i].path);
        if (ID2D1Bitmap* bmp = GetGpuThumb(key)) {
            const ImagePtr& t = m_thumbCache[key].img;
            bool flip;
            int rot;
            OrientationToFlipRot(t->orientation, flip, rot);
            float w = float(t->width), h = float(t->height);
            float dw = (rot & 1) ? h : w, dh = (rot & 1) ? w : h;
            float sc = std::max(cellW / dw, cellH / dh);  // remplissage recadré
            m_rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            m_dc->SetTransform(ImageTransform(w, h, flip, rot, sc, cx, y0 + cellH / 2));
            m_dc->DrawBitmap(bmp, D2D1::RectF(0, 0, w, h), op, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
            m_rt->PopAxisAlignedClip();
        }
        if (isCur) {
            SetColor(kAccent, alpha);
            D2D1_RECT_F b = D2D1::RectF(r.left - 2 * s, r.top - 2 * s, r.right + 2 * s, r.bottom + 2 * s);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(b, 5 * s, 5 * s), m_brush.Get(), 2.5f * s);
        }
    }
}

void Viewer::DrawHud(float cw, float ch) {
    const float s = m_scale;
    const float a = m_hud;
    m_buttons.clear();
    const double now = NowSeconds();

    if (m_edit.active) DrawEditUi(cw, ch);
    else if (m_slideshow) {
        if (a > 0.004f) DrawSlideBar(cw, ch, a);
    } else if (a > 0.004f && (m_cur || m_placeholder || !m_files.empty() || !m_error.empty())) {
        const float barH = 52 * s;
        // Dégradé haut
        if (m_shade) {
            m_shade->SetStartPoint(D2D1::Point2F(0, 0));
            m_shade->SetEndPoint(D2D1::Point2F(0, barH * 1.5f));
            m_shade->SetOpacity(a);
            m_rt->FillRectangle(D2D1::RectF(0, 0, cw, barH * 1.5f), m_shade.Get());
        }

        // Boutons, alignés à droite
        struct Def { Btn id; const wchar_t* glyph; const wchar_t* tip; };
        m_chanLabel = ChannelLabel(m_chanMask);  // texte du bouton Couches (doit survivre au tableau)
        const Def defs[] = {
            {Btn::Open, L"\uE8E5", T("viewer.btn.open")},
            {Btn::SaveAs, L"\uE74E", T("viewer.btn.save_as")},
            {Btn::OpenWith, L"\uE7AC", T("openwith.btn")},
            {Btn::Edit, L"\uE70F", T("viewer.btn.edit")},
            {Btn::None, nullptr, nullptr},
            {Btn::ZoomOut, L"\uE71F", T("viewer.btn.zoom_out")},
            {Btn::ZoomIn, L"\uE8A3", T("viewer.btn.zoom_in")},
            {Btn::Fit, L"\uE9A6", T("viewer.btn.fit")},
            {Btn::Actual, L"1:1", T("viewer.btn.actual")},
            {Btn::Rotate, L"\uE7AD", T("viewer.btn.rotate")},
            {m_cur ? Btn::Channel : Btn::None, m_chanLabel.c_str(), T("viewer.btn.channels")},
            {Btn::None, nullptr, nullptr},
            {Btn::Strip, L"\uECA5", T("viewer.btn.strip")},
            {Btn::Info, L"\uE946", T("viewer.btn.info")},
            {Btn::Help, L"\uE897", T("viewer.btn.help")},
            {Btn::Slideshow, L"\uE786", T("viewer.btn.slideshow")},
            {Btn::Settings, L"\uE713", T("viewer.btn.settings")},
            {Btn::Full, m_fullscreen ? L"\uE73F" : L"\uE740", T("viewer.btn.fullscreen")},
        };
        const float bs = 36 * s, gap = 2 * s, sep = 10 * s;
        float x = cw - 10 * s;
        for (int i = int(ARRAYSIZE(defs)) - 1; i >= 0; --i) {
            if (defs[i].id == Btn::None) {
                x -= sep;
                continue;
            }
            x -= bs;
            m_buttons.push_back({defs[i].id, D2D1::RectF(x, 8 * s, x + bs, 8 * s + bs), defs[i].glyph, defs[i].tip});
            x -= gap;
        }
        // Pastille « mise à jour disponible »
        D2D1_RECT_F updRect{};
        std::wstring updLabel;
        if (m_updateAvailable) {
            updLabel = Tf("viewer.update.badge", m_release.version.c_str());
            updRect = D2D1::RectF(x - 8 * s - 178 * s, 12 * s, x - 8 * s, 12 * s + 28 * s);
            m_buttons.push_back({Btn::Update, updRect, nullptr, T("viewer.btn.update")});
            x = updRect.left;
        }
        const float titleRight = x - 12 * s;
        Btn hovered = m_mouseInside && !m_dragging ? HitButton(m_mouse) : Btn::None;
        if (m_updateAvailable) {
            FillRound(updRect, 14 * s, kAccent, (hovered == Btn::Update ? 1.0f : 0.85f) * a);
            IconLabel(updRect, L"", updLabel, 0x0B1A2A, a);
        }
        for (auto& b : m_buttons) {
            if (b.id == Btn::Update) continue;
            bool active = (b.id == Btn::Strip && m_showStrip) || (b.id == Btn::Info && m_showInfo) ||
                          (b.id == Btn::Help && m_showHelp) || (b.id == Btn::Fit && m_fit) ||
                          (b.id == Btn::Settings && m_showSettings) || (b.id == Btn::Channel && (!ChannelFull(m_chanMask) || m_showChannels));
            if (b.id == hovered) FillRound(b.rc, 5 * s, kWhite, 0.14f * a);
            else if (active) FillRound(b.rc, 5 * s, kWhite, 0.07f * a);
            uint32_t col = active ? kAccent : kWhite;
            if (b.id == Btn::Actual || b.id == Btn::Channel) Text(b.glyph, m_tfLabel.Get(), b.rc, col, 0.92f * a);
            else Text(b.glyph, m_tfIcon.Get(), b.rc, col, 0.92f * a);
        }

        // Titre + ligne d'état
        if (const FileEntry* cur = Current()) {
            Text(cur->name, m_tfTitle.Get(), D2D1::RectF(16 * s, 7 * s, titleRight, 28 * s), kWhite, 0.96f * a);
            std::wstring sub = std::to_wstring(m_index + 1) + L" / " + std::to_wstring(m_files.size());
            float lw, lh;
            if (LogicalSize(lw, lh) && m_cur) sub += L"   ·   " + std::to_wstring(int(lw)) + L" × " + std::to_wstring(int(lh));
            sub += L"   ·   " + FormatBytes(cur->size);
            if (m_cur && !m_cur->format.empty()) sub += L"   ·   " + m_cur->format;
            if (Shown()) sub += L"   ·   " + Percent(m_zoomT);
            if (m_cur && m_cur->refinable) sub += std::wstring(L"   ·   ") + T("viewer.status.raw");
            if (Animated() && m_animImg == m_cur)
                sub += L"   ·   " + Tf("viewer.status.frame", m_frame + 1, int(m_cur->FrameCount())) +
                       (m_playing ? std::wstring() : std::wstring(L"  ") + T("viewer.status.paused"));
            if (m_cur && !ChannelFull(m_chanMask)) sub += L"   ·   " + Tf("viewer.status.channels", ChannelDescription(m_chanMask).c_str());
            Text(sub, m_tfSub.Get(), D2D1::RectF(16 * s, 28 * s, titleRight, 46 * s), kGrey, a);
        }

        // Info-bulle du bouton survolé
        for (auto& b : m_buttons) {
            if (b.id != hovered) continue;
            std::wstring tip = b.tip;
            ComPtr<IDWriteTextLayout> lay;
            m_dwrite->CreateTextLayout(tip.c_str(), UINT32(tip.size()), m_tfSmall.Get(), 400 * s, 30 * s, &lay);
            if (!lay) break;
            DWRITE_TEXT_METRICS tm;
            lay->GetMetrics(&tm);
            float w = tm.width + 20 * s;
            float l = std::clamp((b.rc.left + b.rc.right) / 2 - w / 2, 6 * s, cw - w - 6 * s);
            D2D1_RECT_F r = D2D1::RectF(l, b.rc.bottom + 6 * s, l + w, b.rc.bottom + 32 * s);
            FillRound(r, 4 * s, kPanel, 0.95f * a);
            Text(tip, m_tfCenter.Get(), r, kWhite, a);
        }

        // Flèches de navigation latérales
        int zone = (m_mouseInside && !m_dragging) ? HitNavZone(m_mouse) : 0;
        if (zone != 0) {
            float cx = zone < 0 ? 42 * s : cw - 42 * s;
            float r = 23 * s;
            SetColor(0x000000, 0.45f * a);
            m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, ch / 2), r, r), m_brush.Get());
            Text(zone < 0 ? L"\uE76B" : L"\uE76C", m_tfIconMid.Get(), D2D1::RectF(cx - r, ch / 2 - r, cx + r, ch / 2 + r),
                 kWhite, a);
        }

        // Pellicule
        if (StripVisible()) {
            if (m_shade) {
                const float h = 130 * s;
                m_shade->SetStartPoint(D2D1::Point2F(0, ch));
                m_shade->SetEndPoint(D2D1::Point2F(0, ch - h));
                m_shade->SetOpacity(a);
                m_rt->FillRectangle(D2D1::RectF(0, ch - h, cw, ch), m_shade.Get());
            }
            DrawStrip(cw, ch, a);
        }
    }

    m_channelRows.clear();
    if (m_slideshow) {
    } else if (m_showInfo && !m_showSettings) DrawInfoPanel(cw, ch);
    else if (m_showChannels && !m_showSettings && !m_edit.active) DrawChannelPanel(cw, ch);
    if (m_showSettings && !m_edit.active && !m_slideshow) DrawSettings(cw, ch);
    if (m_showHelp && !m_slideshow) DrawHelp(cw, ch);

    // Toast (niveau de zoom, messages)
    double age = now - m_toastTime;
    if (age < m_toastDur && !m_toast.empty()) {
        float ta = age < m_toastDur - 0.35 ? 1.0f : float((m_toastDur - age) / 0.35);
        ComPtr<IDWriteTextLayout> lay;
        m_dwrite->CreateTextLayout(m_toast.c_str(), UINT32(m_toast.size()), m_tfToast.Get(), cw, 60 * s, &lay);
        float w = 140 * s;
        if (lay) {
            DWRITE_TEXT_METRICS tm;
            lay->GetMetrics(&tm);
            w = std::max(w, tm.width + 44 * s);
        }
        D2D1_RECT_F r = D2D1::RectF(cw / 2 - w / 2, ch / 2 - 30 * s, cw / 2 + w / 2, ch / 2 + 30 * s);
        FillRound(r, 10 * s, 0x000000, 0.6f * ta);
        Text(m_toast, m_tfToast.Get(), r, kWhite, ta);
    }
}

void Viewer::DrawInfoPanel(float cw, float ch) {
    const float s = m_scale;
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    const FileEntry* cur = Current();
    if (!cur) return;
    rows.emplace_back(T("viewer.info.file"), cur->name);
    rows.emplace_back(T("viewer.info.folder"), m_folder);
    rows.emplace_back(T("viewer.info.size"), FormatBytes(cur->size));
    rows.emplace_back(T("viewer.info.modified"), FormatDate(cur->modified));
    if (m_cur) {
        float w, h;
        LogicalSize(w, h);
        wchar_t buf[96];
        rows.emplace_back(T("viewer.info.dimensions"), LocalDecimal(Tf("viewer.info.dims", int(w), int(h), w * h / 1e6)));
        rows.emplace_back(T("viewer.info.format"), m_cur->format);
        swprintf_s(buf, L"%.0f ms", m_cur->decodeMs);
        rows.emplace_back(T("viewer.info.decode"), buf);
        if (!m_cur->info.empty()) rows.emplace_back(L"", L"");
        for (auto& kv : m_cur->info) rows.push_back(kv);
    }

    const float rowH = 23 * s, pad = 16 * s, keyW = 104 * s, width = 360 * s;
    const float top = 58 * s;
    float height = pad * 2 + 30 * s + rowH * rows.size();
    height = std::min(height, ch - top - 12 * s);
    D2D1_RECT_F p = D2D1::RectF(cw - width - 12 * s, top, cw - 12 * s, top + height);
    FillRound(p, 8 * s, kPanel, 0.93f);
    Text(T("viewer.info.title"), m_tfTitle.Get(), D2D1::RectF(p.left + pad, p.top + pad - 4 * s, p.right - pad, p.top + pad + 22 * s), kWhite, 1);
    float y = p.top + pad + 30 * s;
    m_rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
    for (auto& [k, v] : rows) {
        if (y + rowH > p.bottom) break;
        Text(k, m_tfSmall.Get(), D2D1::RectF(p.left + pad, y, p.left + pad + keyW, y + rowH), kGrey, 1);
        Text(v, m_tfSmall.Get(), D2D1::RectF(p.left + pad + keyW, y, p.right - pad, y + rowH), kWhite, 1);
        y += rowH;
    }
    m_rt->PopAxisAlignedClip();
}

void Viewer::DrawHelp(float cw, float ch) {
    const float s = m_scale;
    // Libellé de la touche : clé de traduction (touches nommées) ou texte fixe (lettres, Ctrl+...)
    struct HelpRow {
        const char* keyName;
        const wchar_t* keyFixed;
        const char* desc;
    };
    static const HelpRow keys[] = {
        {"viewer.help.k.nav", nullptr, "viewer.help.d.nav"},
        {nullptr, L"N", "viewer.help.d.filter"},
        {nullptr, L"D", "viewer.help.d.slideshow"},
        {"viewer.help.k.copy", nullptr, "viewer.help.d.copy"},
        {"viewer.help.k.channels", nullptr, "viewer.help.d.channels"},
        {"viewer.help.k.anim", nullptr, "viewer.help.d.anim"},
        {"viewer.help.k.home_end", nullptr, "viewer.help.d.home_end"},
        {"viewer.help.k.zoom", nullptr, "viewer.help.d.zoom"},
        {"viewer.help.k.drag", nullptr, "viewer.help.d.drag"},
        {"viewer.help.k.dblclick", nullptr, "viewer.help.d.dblclick"},
        {nullptr, L"0 / 1", "viewer.help.d.fit"},
        {"viewer.help.k.rotate", nullptr, "viewer.help.d.rotate"},
        {"viewer.help.k.fullscreen", nullptr, "viewer.help.d.fullscreen"},
        {nullptr, L"T", "viewer.help.d.strip"},
        {nullptr, L"I", "viewer.help.d.info"},
        {nullptr, L"Ctrl+O", "viewer.help.d.open"},
        {nullptr, L"Ctrl+S", "viewer.help.d.save_as"},
        {"openwith.help.key", nullptr, "openwith.help.desc"},
        {nullptr, L"Ctrl+U", "viewer.help.d.update"},
        {nullptr, L"P", "viewer.help.d.settings"},
        {nullptr, L"E", "viewer.help.d.edit"},
        {nullptr, L"F5", "viewer.help.d.reload"},
        {"viewer.help.k.mouse_side", nullptr, "viewer.help.d.mouse_side"},
        {"viewer.help.k.esc", nullptr, "viewer.help.d.esc"},
    };
    const auto& plugins = m_decoder.Plugins();
    EnsureEditPlugins();
    // Deux colonnes de raccourcis, puis les plugins sur deux colonnes : tient dans un écran 1080p
    const float rowH = 24 * s, pad = 22 * s, w = std::min(1080 * s, cw - 24 * s);
    const size_t keyRows = (ARRAYSIZE(keys) + 1) / 2, plugRows = (std::max<size_t>(1, plugins.size()) + 1) / 2;
    float h = pad * 2 + 36 * s + rowH * keyRows + 40 * s + rowH * plugRows + EditPluginsHelpHeight(rowH);
    h = std::min(h, ch - 24 * s);
    D2D1_RECT_F p = D2D1::RectF(cw / 2 - w / 2, ch / 2 - h / 2, cw / 2 + w / 2, ch / 2 + h / 2);
    SetColor(0x000000, 0.45f);
    m_rt->FillRectangle(D2D1::RectF(0, 0, cw, ch), m_brush.Get());
    FillRound(p, 10 * s, kPanel, 0.97f);
    m_rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
    float titleX = p.left + pad;
    if (ID2D1Bitmap* logo = Logo()) {
        const float ls = 26 * s;
        m_dc->DrawBitmap(logo, D2D1::RectF(titleX, p.top + pad - 4 * s, titleX + ls, p.top + pad - 4 * s + ls), 1.0f,
                         D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        titleX += ls + 10 * s;
    }
    Text(std::wstring(T("viewer.help.title")) + L"   ·   CyImgView " CYIMG_VERSION_WSTR, m_tfTitle.Get(), D2D1::RectF(titleX, p.top + pad - 6 * s, p.right - pad, p.top + pad + 22 * s), kWhite, 1);
    const float top = p.top + pad + 36 * s;
    const float half = (w - 2 * pad) / 2, col = std::min(200 * s, half * 0.42f);
    for (size_t i = 0; i < ARRAYSIZE(keys); ++i) {
        const float x = p.left + pad + (i < keyRows ? 0 : half + 12 * s), y = top + rowH * float(i % keyRows);
        std::wstring keyText = keys[i].keyName ? T(keys[i].keyName) : keys[i].keyFixed;
        if (keyText.rfind(L"Ctrl", 0) == 0) keyText = T("viewer.key.ctrl") + keyText.substr(4);  // « Strg » en allemand
        Text(keyText, m_tfSmall.Get(), D2D1::RectF(x, y, x + col - 8 * s, y + rowH), kAccent, 1);
        Text(T(keys[i].desc), m_tfSmall.Get(), D2D1::RectF(x + col, y, x + half - 12 * s, y + rowH), kWhite, 0.95f);
    }
    float y = top + rowH * keyRows + 12 * s;
    Text(Tf("viewer.help.plugins", int(plugins.size())), m_tfTitle.Get(), D2D1::RectF(p.left + pad, y, p.right - pad, y + 24 * s), kWhite, 1);
    y += 28 * s;
    if (plugins.empty()) {
        Text(T("viewer.help.no_plugins"), m_tfSmall.Get(), D2D1::RectF(p.left + pad, y, p.right - pad, y + rowH), kGrey, 1);
    }
    for (size_t i = 0; i < plugins.size(); ++i) {
        const auto& pl = plugins[i];
        const float x = p.left + pad + (i < plugRows ? 0 : half + 12 * s), yy = y + rowH * float(i % plugRows);
        std::wstring exts = pl.extensions;
        std::replace(exts.begin(), exts.end(), L';', L' ');
        // Nom affiché traduit si une clé « pluginname.<fichier> » existe (plugins livrés avec CyImgView)
        std::string nameKey = "pluginname.";
        for (wchar_t c : FileNameOf(pl.file).substr(0, FileNameOf(pl.file).find_last_of(L'.'))) nameKey += char(towlower(c));
        Text(TrOr(nameKey.c_str(), pl.name.c_str()), m_tfSmall.Get(), D2D1::RectF(x, yy, x + col - 8 * s, yy + rowH), kAccent, 1);
        Text(exts, m_tfSmall.Get(), D2D1::RectF(x + col, yy, x + half - 12 * s, yy + rowH), kGrey, 1);
    }
    DrawEditPluginsHelp(p.left + pad, p.right - pad, y + rowH * plugRows + 8 * s, rowH, col);
    m_rt->PopAxisAlignedClip();
}

// =============================================================================================
// Vue : zoom / déplacement / rotation

void Viewer::FlipRot(bool& flip, int& rot) const {
    if (m_edit.active) {  // le document édité est déjà orienté
        flip = false;
        rot = 0;
        return;
    }
    ImagePtr img = Shown();
    OrientationToFlipRot(img ? img->orientation : 1, flip, rot);
    rot = (rot + m_userRot) & 3;
}

bool Viewer::LogicalSize(float& w, float& h) const {
    if (m_edit.active) {
        w = float(m_edit.w);
        h = float(m_edit.h);
        return w > 0 && h > 0;
    }
    ImagePtr img = Shown();
    if (!img) return false;
    bool isFull = img == m_cur;
    w = float(isFull ? img->width : (img->fullWidth ? img->fullWidth : img->width));
    h = float(isFull ? img->height : (img->fullHeight ? img->fullHeight : img->height));
    bool flip;
    int rot;
    FlipRot(flip, rot);
    if (rot & 1) std::swap(w, h);
    return w > 0 && h > 0;
}

POINT Viewer::ClientCenter() const {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    return POINT{rc.right / 2, rc.bottom / 2};
}

// Zone où l'image est ajustée : toute la fenêtre, sauf en édition (barres d'outils toujours visibles).
D2D1_RECT_F Viewer::ViewArea() const {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (!m_edit.active) return D2D1::RectF(0, 0, float(rc.right), float(rc.bottom));
    return EditViewArea(float(rc.right), float(rc.bottom));  // marges du mode d'édition actif
}

double Viewer::FitZoom() const {
    float w, h;
    if (!LogicalSize(w, h)) return 1.0;
    D2D1_RECT_F a = ViewArea();
    double f = std::min((a.right - a.left) / double(w), (a.bottom - a.top) / double(h));
    return std::max(1e-4, std::min(1.0, f));  // pas d'agrandissement des petites images
}

void Viewer::ApplyFit(bool animate) {
    m_fit = true;
    m_zoomT = FitZoom();
    m_panXT = m_panYT = 0;
    ClampPan(m_panXT, m_panYT, m_zoomT);  // centre dans la zone d'affichage
    if (!animate) {
        m_zoom = m_zoomT;
        m_panX = m_panXT;
        m_panY = m_panYT;
    }
    Invalidate();
}

void Viewer::ClampPan(double& px, double& py, double zoom) const {
    float w, h;
    if (!LogicalSize(w, h)) return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const D2D1_RECT_F a = ViewArea();
    const double aw = a.right - a.left, ah = a.bottom - a.top;
    const double ox = (a.left + a.right) / 2.0 - rc.right / 2.0, oy = (a.top + a.bottom) / 2.0 - rc.bottom / 2.0;
    const double ew = w * zoom, eh = h * zoom;
    if (ew <= aw) px = ox;
    else px = std::clamp(px, ox - (ew - aw) / 2, ox + (ew - aw) / 2);
    if (eh <= ah) py = oy;
    else py = std::clamp(py, oy - (eh - ah) / 2, oy + (eh - ah) / 2);
}

void Viewer::ZoomTo(double z, POINT anchor, bool animate) {
    float w, h;
    if (!LogicalSize(w, h)) return;
    const double fz = FitZoom();
    z = std::clamp(z, std::min(fz, 1.0) * 0.5, 40.0);
    POINT c = ClientCenter();
    // Le point de l'image sous le curseur reste sous le curseur.
    double qx = (anchor.x - c.x - m_panXT) / m_zoomT;
    double qy = (anchor.y - c.y - m_panYT) / m_zoomT;
    m_panXT = anchor.x - c.x - qx * z;
    m_panYT = anchor.y - c.y - qy * z;
    m_zoomT = z;
    m_fit = std::fabs(z - fz) < 1e-9;
    ClampPan(m_panXT, m_panYT, z);
    if (!animate) {
        m_zoom = m_zoomT;
        m_panX = m_panXT;
        m_panY = m_panYT;
    }
    ShowToast(Percent(z));
    Invalidate();
}

void Viewer::ZoomStep(double factor, POINT anchor) {
    if (!Shown()) return;
    double target = m_zoomT * factor;
    // Crans magnétiques sur « ajusté » et 100 %
    for (double snap : {1.0, FitZoom()}) {
        if ((m_zoomT < snap - 1e-9 && target > snap) || (m_zoomT > snap + 1e-9 && target < snap)) target = snap;
    }
    ZoomTo(target, anchor, true);
}

void Viewer::ToggleActualFit(POINT anchor) {
    float w, h;
    if (!LogicalSize(w, h)) return;
    const double fz = FitZoom();
    if (m_fit || std::fabs(m_zoomT - fz) < 1e-9) {
        if (fz >= 1.0 - 1e-9) {  // petite image : agrandir pour remplir
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            double fill = std::min(rc.right / double(w), rc.bottom / double(h));
            if (fill > 1.0 + 1e-6) ZoomTo(fill, ClientCenter(), true);
        } else {
            ZoomTo(1.0, anchor, true);
        }
    } else {
        ApplyFit(true);
        ShowToast(T("viewer.toast.fit"));
    }
}

void Viewer::Rotate(int quarter) {
    if (!Shown()) return;
    m_userRot = (m_userRot + quarter) & 3;
    if (m_fit) {
        ApplyFit(false);
    } else {
        ClampPan(m_panXT, m_panYT, m_zoomT);
        m_panX = m_panXT;
        m_panY = m_panYT;
    }
    Invalidate();
}

void Viewer::ShowToast(const std::wstring& s, double seconds) {
    m_toast = s;
    m_toastTime = NowSeconds();
    m_toastDur = seconds;
    Invalidate();
}

void Viewer::ToggleFullscreen() {
    DWORD style = GetWindowLongW(m_hwnd, GWL_STYLE);
    if (!m_fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(m_hwnd, &m_wpPrev) && GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(m_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            m_fullscreen = true;
        }
    } else {
        SetWindowLongW(m_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(m_hwnd, &m_wpPrev);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        m_fullscreen = false;
    }
    Invalidate();
}

// =============================================================================================
// Fichiers et navigation

void Viewer::OpenPath(std::wstring path) {
    if (m_edit.active && !EndEdit(true)) return;
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
    if (n && n < ARRAYSIZE(full)) path = full;
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        ShowToast(T("viewer.toast.not_found"));
        return;
    }
    std::wstring folder;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        folder = path;
        path.clear();
    } else {
        folder = ParentOf(path);
        m_loader->SetFullQueue({path});  // démarre le décodage pendant qu'on liste le dossier
    }
    if (folder.size() == 3 && folder[1] == L':') folder.pop_back();  // "D:\" -> "D:"

    m_folder = folder;
    m_files = ListImages(folder, m_decoder);
    int index = 0;
    if (!path.empty()) {
        const std::wstring key = Key(path);
        auto it = std::find_if(m_files.begin(), m_files.end(), [&](const FileEntry& e) { return Key(e.path) == key; });
        if (it == m_files.end()) {  // extension inconnue : on tente quand même (sondage des plugins)
            FileEntry e;
            e.path = path;
            e.name = FileNameOf(path);
            e.size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            e.modified = fa.ftLastWriteTime;
            m_files.push_back(std::move(e));
            index = int(m_files.size()) - 1;
        } else {
            index = int(it - m_files.begin());
        }
    }
    m_pathIndex.clear();
    for (int i = 0; i < int(m_files.size()); ++i) m_pathIndex[Key(m_files[i].path)] = i;

    m_index = -1;
    m_cur.reset();
    m_placeholder.reset();
    m_error.clear();
    m_loading = false;
    m_thumbReqLo = m_thumbReqHi = m_thumbReqIndex = -1;
    if (m_files.empty()) {
        UpdateTitle();
        ShowToast(T("viewer.toast.no_images"));
        Invalidate();
        return;
    }
    m_stripPos = m_stripPosT = index;
    GoTo(index);
}

void Viewer::OpenDialog() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    std::wstring pattern = m_decoder.DialogPattern();
    COMDLG_FILTERSPEC spec[] = {{T("viewer.dlg.images"), pattern.c_str()}, {T("viewer.dlg.all_files"), L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetTitle(T("viewer.dlg.open_title"));
    if (!m_folder.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName((m_folder + L"\\").c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dlg->SetFolder(folder.Get());
    }
    if (FAILED(dlg->Show(m_hwnd))) return;
    ComPtr<IShellItem> item;
    PWSTR p = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        OpenPath(p);
        CoTaskMemFree(p);
    }
}

void Viewer::GoTo(int index) {
    if (m_files.empty()) return;
    if (m_edit.active && !EndEdit(true)) return;  // modifications en cours : on demande
    index = std::clamp(index, 0, int(m_files.size()) - 1);
    if (m_index >= 0 && index != m_index) m_lastDir = index > m_index ? 1 : -1;
    m_index = index;
    m_preuploadsLeft = 2;
    const std::wstring key = Key(m_files[index].path);

    auto fc = m_fullCache.find(key);
    m_cur = fc != m_fullCache.end() ? fc->second : nullptr;
    m_placeholder.reset();
    if (!m_cur) {
        auto tc = m_thumbCache.find(key);
        if (tc != m_thumbCache.end()) m_placeholder = tc->second.img;
    }
    auto fe = m_failed.find(key);
    m_error = (!m_cur && fe != m_failed.end()) ? fe->second : L"";
    m_loading = !m_cur && m_error.empty();
    m_loadStart = NowSeconds();
    m_userRot = 0;
    ApplyFit(false);
    m_stripPosT = index;
    RequestLoads();
    UpdateTitle();
    Invalidate();
}

void Viewer::Navigate(int delta) {
    if (m_files.empty()) return;
    int target = m_index + delta;
    if (target < 0) {
        ShowToast(T("viewer.toast.first"));
        return;
    }
    if (target >= int(m_files.size())) {
        ShowToast(T("viewer.toast.last"));
        return;
    }
    m_lastDir = delta > 0 ? 1 : -1;
    GoTo(target);
}

void Viewer::RequestLoads() {
    if (!m_loader || m_index < 0) return;
    std::vector<std::wstring> want;
    auto add = [&](int i) {
        if (i < 0 || i >= int(m_files.size())) return;
        std::wstring key = Key(m_files[i].path);
        if (m_fullCache.count(key) || m_failed.count(key)) return;
        want.push_back(m_files[i].path);
    };
    add(m_index);
    add(m_index + m_lastDir);
    add(m_index + 2 * m_lastDir);
    add(m_index - m_lastDir);
    m_loader->SetFullQueue(want);

    // Développement complet (RAW) de l'image courante seulement : c'est coûteux.
    std::vector<std::wstring> refine;
    auto fc = m_fullCache.find(Key(m_files[m_index].path));
    if (fc != m_fullCache.end() && fc->second->refinable) refine.push_back(m_files[m_index].path);
    m_loader->SetRefineQueue(refine);
    m_thumbReqLo = -1;
    RequestThumbs();
}

void Viewer::RequestThumbs() {
    if (!m_loader || m_files.empty()) return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float pitch = 98 * m_scale;
    int half = StripVisible() ? int(rc.right / 2 / pitch) + 3 : 2;
    int center = int(std::lround(m_stripPosT));
    int lo = std::max(0, center - half), hi = std::min(int(m_files.size()) - 1, center + half);
    if (lo == m_thumbReqLo && hi == m_thumbReqHi && m_index == m_thumbReqIndex) return;
    m_thumbReqLo = lo;
    m_thumbReqHi = hi;
    m_thumbReqIndex = m_index;

    std::vector<std::wstring> want;
    auto add = [&](int i) {
        if (i < lo || i > hi || i < 0 || i >= int(m_files.size())) return;
        if (m_thumbCache.count(Key(m_files[i].path))) return;
        want.push_back(m_files[i].path);
    };
    if (m_index >= 0 && !m_fullCache.count(Key(m_files[m_index].path))) {
        // vignette de l'image courante d'abord : sert d'aperçu instantané
        if (!m_thumbCache.count(Key(m_files[m_index].path))) want.push_back(m_files[m_index].path);
    }
    for (int d = 0; d <= half; ++d) {
        if (center + d != m_index) add(center + d);
        if (d && center - d != m_index) add(center - d);
    }
    m_loader->SetThumbQueue(want);
}

void Viewer::EnforceBudget() {
    size_t total = 0;
    for (auto& [k, v] : m_fullCache) total += v->pixels.Size();
    const std::wstring curKey = Current() ? Key(Current()->path) : std::wstring();
    while (total > m_budget && m_fullCache.size() > 1) {
        auto victim = m_fullCache.end();
        int best = -1;
        for (auto it = m_fullCache.begin(); it != m_fullCache.end(); ++it) {
            if (it->first == curKey) continue;
            auto pi = m_pathIndex.find(it->first);
            int dist = pi == m_pathIndex.end() ? INT_MAX : std::abs(pi->second - m_index);
            if (dist > best) {
                best = dist;
                victim = it;
            }
        }
        if (victim == m_fullCache.end()) break;
        total -= victim->second->pixels.Size();
        const std::wstring vk = victim->first;
        m_gpu.erase(std::remove_if(m_gpu.begin(), m_gpu.end(), [&](const GpuImage& g) { return g.key == vk; }), m_gpu.end());
        m_fullCache.erase(victim);
    }
    if (m_thumbCache.size() > 4000) {
        std::vector<std::pair<uint64_t, std::wstring>> v;
        for (auto& [k, t] : m_thumbCache) v.emplace_back(t.lastUse, k);
        std::nth_element(v.begin(), v.begin() + 1000, v.end());
        for (size_t i = 0; i < 1000; ++i) m_thumbCache.erase(v[i].second);
    }
}

void Viewer::OnLoaded(LoadResult* raw) {
    std::unique_ptr<LoadResult> r(raw);
    const std::wstring key = Key(r->path);
    const FileEntry* cur = Current();
    const bool isCurrent = cur && Key(cur->path) == key;

    if (r->kind == JobKind::Thumb) {
        if (!r->image && r->cancelled) return;
        m_thumbCache[key] = {r->image, ++m_tick};
        if (isCurrent && !m_cur && r->image) {
            m_placeholder = r->image;
            if (m_fit) ApplyFit(false);
        }
        EnforceBudget();
        Invalidate();
        return;
    }

    if (r->kind == JobKind::Refine && !r->image) {
        auto fc = m_fullCache.find(key);
        if (!r->cancelled) {
            if (fc != m_fullCache.end()) fc->second->refinable = false;  // on garde l'aperçu, sans réessayer
            Invalidate();
        } else if (isCurrent) {
            RequestLoads();  // annulé puis redemandé entre-temps
        }
        return;
    }
    if (r->kind == JobKind::Full && r->image) {
        auto fc = m_fullCache.find(key);
        if (fc != m_fullCache.end() && !fc->second->refinable) return;  // déjà la version complète
    }

    if (r->image) {
        m_fullCache[key] = r->image;
        m_failed.erase(key);
    } else if (!r->cancelled) {
        m_failed[key] = r->error;
    }

    if (isCurrent && m_edit.active) {  // le document édité ne change pas ; on met juste à jour l'original
        if (r->image) m_cur = r->image;
        return;
    }
    if (isCurrent) {
        if (r->image) {
            Trace(L"image courante décodée");
            float ow = 0, oh = 0;
            bool had = LogicalSize(ow, oh);
            m_cur = r->image;
            m_placeholder.reset();
            m_loading = false;
            m_error.clear();
            float nw, nh;
            if (m_fit || !had) {
                ApplyFit(false);
            } else if (LogicalSize(nw, nh) && nw > 0) {
                double ratio = ow / nw;  // même taille à l'écran qu'avec l'aperçu
                m_zoom *= ratio;
                m_zoomT *= ratio;
            }
            UpdateTitle();
            if (m_cur->refinable) RequestLoads();  // lance le développement complet
        } else if (!r->cancelled) {
            m_loading = false;
            m_error = r->error;
            m_placeholder.reset();
        } else {
            RequestLoads();  // annulé puis redemandé entre-temps
        }
    }
    EnforceBudget();
    Invalidate();
}

void Viewer::UpdateTitle() {
    const FileEntry* cur = Current();
    std::wstring t = cur ? cur->name + L" — CyImgView" : L"CyImgView";
    SetWindowTextW(m_hwnd, t.c_str());
}

// =============================================================================================
// Interaction

bool Viewer::StripVisible() const { return m_showStrip && m_files.size() > 1 && !m_edit.active && !m_slideshow; }

Viewer::Btn Viewer::HitButton(POINT p) const {
    if (m_hud < 0.3f) return Btn::None;
    for (auto& b : m_buttons)
        if (Contains(b.rc, p)) return b.id;
    return Btn::None;
}

int Viewer::HitStrip(POINT p) const {
    if (!StripVisible() || m_hud < 0.3f) return -1;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float s = m_scale;
    const float cellW = 92 * s, cellH = 66 * s, pitch = cellW + 6 * s;
    const float y0 = rc.bottom - 14 * s - cellH;
    if (p.y < y0 - 4 * s || p.y > y0 + cellH + 4 * s) return -1;
    double f = (p.x - rc.right / 2.0) / pitch + m_stripPos;
    int i = int(std::lround(f));
    if (i < 0 || i >= int(m_files.size())) return -1;
    double cx = rc.right / 2.0 + (i - m_stripPos) * pitch;
    if (std::fabs(p.x - cx) > cellW / 2) return -1;
    return i;
}

int Viewer::HitNavZone(POINT p) const {
    if (m_files.size() < 2 || m_hud < 0.3f || m_showHelp || m_edit.active) return 0;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float s = m_scale;
    float top = 60 * s, bottom = rc.bottom - (StripVisible() ? 110 * s : 20 * s);
    if (p.y < top || p.y > bottom) return 0;
    for (const ChannelRow& r : m_channelRows)
        if (Contains(r.rc, p)) return 0;  // panneau Couches par-dessus la flèche
    float zw = 84 * s;
    if (p.x < zw && m_index > 0) return -1;
    if (p.x > rc.right - zw && m_index < int(m_files.size()) - 1) return 1;
    return 0;
}

void Viewer::Poke() {
    m_lastActivity = NowSeconds();
    if (m_hud < 1.0f) Invalidate();
}

void Viewer::OnMouseMove(POINT p, WPARAM) {
    if (!m_mouseInside) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
        TrackMouseEvent(&tme);
        m_mouseInside = true;
    }
    bool moved = p.x != m_mouse.x || p.y != m_mouse.y;
    m_mouse = p;
    if (!moved) return;
    if (m_dragging) {
        m_panXT = m_dragPanX + (p.x - m_dragStart.x);
        m_panYT = m_dragPanY + (p.y - m_dragStart.y);
        ClampPan(m_panXT, m_panYT, m_zoomT);
        m_panX = m_panXT;
        m_panY = m_panYT;
        m_zoom = m_zoomT;
        Invalidate();
        return;
    }
    Poke();
    Invalidate();  // survol des boutons / pellicule
}

void Viewer::OnButton(Btn b) {
    POINT c = ClientCenter();
    switch (b) {
    case Btn::Open: OpenDialog(); break;
    case Btn::SaveAs: SaveAs(); break;
    case Btn::OpenWith: ShowOpenWithForCurrent(); break;
    case Btn::Edit: BeginEdit(); break;
    case Btn::Update: ShowUpdateDialog(); break;
    case Btn::Prev: Navigate(-1); break;
    case Btn::Next: Navigate(1); break;
    case Btn::ZoomOut: ZoomStep(1 / 1.25, c); break;
    case Btn::ZoomIn: ZoomStep(1.25, c); break;
    case Btn::Fit: ApplyFit(true); ShowToast(T("viewer.toast.fit")); break;
    case Btn::Actual: ZoomTo(1.0, c, true); break;
    case Btn::Rotate: Rotate(1); break;
    case Btn::Strip: m_showStrip = !m_showStrip; m_thumbReqLo = -1; RequestThumbs(); break;
    case Btn::Info:
        m_showInfo = !m_showInfo;
        m_showSettings = false;
        m_showChannels = false;
        break;
    case Btn::Settings:
        m_showSettings = !m_showSettings;
        m_showInfo = false;
        m_showChannels = false;
        m_showHelp = false;
        break;
    case Btn::Help: m_showHelp = !m_showHelp; break;
    case Btn::Full: ToggleFullscreen(); break;
    case Btn::Slideshow: StartSlideshow(); break;
    case Btn::SlidePrev: SlideGo(-1, false); break;
    case Btn::SlideNext: SlideGo(1, false); break;
    case Btn::SlidePlay: ToggleSlidePause(); break;
    case Btn::SlideLess: ChangeSlideDelay(-1); break;
    case Btn::SlideMore: ChangeSlideDelay(1); break;
    case Btn::SlideShuffle:
        m_slideShuffle = !m_slideShuffle;
        SaveSetting(L"slideshow", L"shuffle", m_slideShuffle);
        ShowToast(T(m_slideShuffle ? "viewer.toast.shuffle_on" : "viewer.toast.shuffle_off"));
        break;
    case Btn::SlideExit: StopSlideshow(); break;
    case Btn::Channel:
        m_showChannels = !m_showChannels;
        m_showInfo = false;
        m_showSettings = false;
        break;
    default: break;
    }
    Invalidate();
}

void Viewer::OnKey(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    POINT c = ClientCenter();
    if (m_slideshow) {  // diaporama : seules ses touches (et la copie) sont actives
        if (!SlideKey(vk) && ctrl && vk == 'C') CopyToClipboard();
        return;
    }
    switch (vk) {
    case 'D': if (!ctrl) StartSlideshow(); break;
    case 'N':  // pixels bruts <-> filtrage choisi
        if (!ctrl) SetFilter(m_filter == 1 ? (m_filterBeforeRaw == 1 ? 0 : m_filterBeforeRaw) : 1);
        break;
    case VK_SPACE:
        if (Animated()) TogglePlay();
        else Navigate(1);
        break;
    case VK_OEM_COMMA: StepFrame(-1); break;
    case VK_OEM_PERIOD: StepFrame(1); break;
    case 'C':
        if (ctrl && shift) CopyPathToClipboard();
        else if (ctrl) CopyToClipboard();
        else if (m_cur) CycleChannel(shift ? -1 : 1);
        break;
    case VK_INSERT: if (ctrl) CopyToClipboard(); break;
    case '2': case '3': case '4': case '5': case '6': if (ctrl) SetChannel(int(vk - '2')); break;
    case VK_RIGHT: case VK_NEXT: Navigate(1); break;
    case VK_LEFT: case VK_PRIOR: case VK_BACK: Navigate(-1); break;
    case VK_HOME: GoTo(0); break;
    case VK_END: GoTo(int(m_files.size()) - 1); break;
    case VK_UP: ZoomStep(1.25, c); break;
    case VK_DOWN: ZoomStep(1 / 1.25, c); break;
    case '0': case VK_NUMPAD0: case VK_MULTIPLY: if (Shown()) { ApplyFit(true); ShowToast(T("viewer.toast.fit")); } break;
    case '1': case VK_NUMPAD1: case VK_DIVIDE: if (!ctrl && Shown()) ZoomTo(1.0, c, true); break;
    case 'Z': ToggleActualFit(c); break;
    case 'F': case VK_F11: case VK_RETURN: ToggleFullscreen(); break;
    case 'R': Rotate(shift ? -1 : 1); break;
    case 'L': Rotate(-1); break;
    case 'T': OnButton(Btn::Strip); break;
    case 'I': OnButton(Btn::Info); break;
    case 'P': OnButton(Btn::Settings); break;
    case 'H': case VK_F1: OnButton(Btn::Help); break;
    case 'O':
        if (ctrl && shift) ShowOpenWithForCurrent();
        else if (ctrl) OpenDialog();
        break;
    case 'S': if (ctrl) SaveAs(); break;
    case 'U':
        if (ctrl) {
            if (m_updateAvailable) ShowUpdateDialog();
            else StartUpdateCheck(true);
        }
        break;
    case 'E': if (!ctrl) BeginEdit(); break;
    case VK_F5:
        if (const FileEntry* cur = Current()) {
            std::wstring p = cur->path;
            m_fullCache.erase(Key(p));
            m_failed.erase(Key(p));
            m_gpu.clear();
            OpenPath(p);
        }
        break;
    case VK_ESCAPE:
        if (m_edit.active) EndEdit(true);
        else if (m_showHelp) m_showHelp = false;
        else if (m_showSettings) m_showSettings = false;
        else if (m_showInfo) m_showInfo = false;
        else if (m_showChannels) m_showChannels = false;
        else if (m_fullscreen) ToggleFullscreen();
        else PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        Invalidate();
        break;
    default: break;
    }
}

LRESULT CALLBACK Viewer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Viewer* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Viewer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Viewer::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        EndPaint(m_hwnd, &ps);
        Render();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        ResizeSwapChain(LOWORD(lp), HIWORD(lp));
        if (m_fit) ApplyFit(false);
        else {
            ClampPan(m_panXT, m_panYT, m_zoomT);
            m_panX = m_panXT;
            m_panY = m_panYT;
        }
        m_thumbReqLo = -1;
        RequestThumbs();
        Invalidate();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {LONG(480 * m_scale), LONG(320 * m_scale)};
        return 0;
    }
    case WM_DPICHANGED: {
        m_scale = HIWORD(wp) / 96.0f;
        CreateTextFormats();
        if (m_checker) m_checker->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale));
        if (m_loader) m_loader->SetThumbSize(std::min(320u, UINT(160 * m_scale)));
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Invalidate();
        return 0;
    }
    case WM_APP_LOADED: OnLoaded(reinterpret_cast<LoadResult*>(lp)); return 0;
    case WM_APP_PREUPLOAD: PreUploadNeighbor(); return 0;
    case WM_APP_GPU_READY:
        if (m_usingWarp) {  // bascule du rendu logiciel vers le GPU
            if (m_edit.active) {
                m_gpuSwitchPending = true;  // le document vit sur ce device : bascule à la sortie du mode
            } else {
                DiscardDeviceResources();
                Invalidate();
            }
        }
        return 0;
    case kMsgUpdate: OnUpdateResult(reinterpret_cast<UpdateResult*>(lp)); return 0;
    case kMsgPlugJob: OnPluginJobs(); return 0;
    case WM_COPYDATA: {  // « une seule fenêtre » : fichier transmis par un nouveau lancement
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != kCopyDataOpen || cds->cbData < sizeof(wchar_t) || cds->cbData > 65536) break;
        auto* path = new std::wstring(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1);
        PostMessageW(m_hwnd, kMsgOpenPath, 0, reinterpret_cast<LPARAM>(path));  // ouverture après la réponse
        if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
        SetForegroundWindow(m_hwnd);
        return TRUE;
    }
    case kMsgOpenPath: {
        std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lp));
        OpenPath(*path);
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerUpdate) {
            KillTimer(m_hwnd, kTimerUpdate);
            StartUpdateCheck(false);
            return 0;
        }
        if (wp == kTimerSlide) {
            OnSlideTimer();
            return 0;
        }
        if (wp == kTimerAnim) {
            OnAnimTimer();
            return 0;
        }
        if (wp == TIMER_HUD && m_hud > 0.0f && !HudWanted()) Invalidate();
        if (wp == TIMER_HUD && m_edit.active && (m_edit.selValid || m_edit.drawing)) Invalidate();  // pointillés de la sélection
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            LPCWSTR cur = IDC_ARROW;
            if (m_edit.active) {
                if (const wchar_t* mc = EditModeCursor()) cur = mc;  // Mode Développement (ou autre mode)
                else if (const EditButton* b = HitEdit(m_mouse)) cur = b->cmd == EditCmd::Panel ? IDC_ARROW : IDC_HAND;
                else if (m_dragging || m_edit.spacePan || m_edit.tool == Tool::Hand || m_edit.tool == Tool::Move) cur = IDC_SIZEALL;
                else if (m_edit.fx >= 0) cur = IDC_ARROW;
                else if (m_edit.tool == Tool::Text) cur = IDC_IBEAM;
                else if (m_edit.tool == Tool::Brush || m_edit.tool == Tool::Eraser) {
                    SetCursor(nullptr);  // remplacé par le cercle de taille du pinceau
                    return TRUE;
                } else cur = IDC_CROSS;
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            if (m_fullscreen && m_hud < 0.05f && !m_showHelp && !m_showInfo) SetCursor(nullptr);
            else if (m_dragging) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            else if (std::any_of(m_channelRows.begin(), m_channelRows.end(),
                                 [&](const ChannelRow& r) { return r.ch >= 0 && Contains(r.rc, m_mouse); }))
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
            else if (HitButton(m_mouse) != Btn::None || HitStrip(m_mouse) >= 0 || HitNavZone(m_mouse) != 0)
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
            else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (m_edit.active && !m_dragging) {
            EditMouseMove(p);
            if (m_mouse.x != p.x || m_mouse.y != p.y) Invalidate();  // cercle du pinceau, survol
        }
        OnMouseMove(p, wp);
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (m_edit.active) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            m_mouse = p;
            EditMouseDown(p, true, false);
        }
        return 0;
    case WM_RBUTTONUP:
        if (m_edit.active) EditMouseUp(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        else ShowContextMenu(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_CONTEXTMENU:  // touche Menu / Maj+F10
        if (!m_edit.active && lp == -1) ShowContextMenu(ClientCenter());
        return 0;
    case WM_MOUSELEAVE:
        m_mouseInside = false;
        Invalidate();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        m_mouse = p;
        Poke();
        if (m_showHelp) {
            m_showHelp = false;
            Invalidate();
            return 0;
        }
        if (m_slideshow) {  // diaporama : seulement les boutons de la barre
            if (Btn b = HitButton(p); b != Btn::None) OnButton(b);
            return 0;
        }
        if (m_showSettings && !m_edit.active) {  // panneau Paramètres : clic sur une option, ou en dehors pour fermer
            for (const SettingRow& r : m_settingRows) {
                if (Contains(r.rc, p) && Contains(m_settingsPanel, p)) {  // lignes défilées hors du panneau : ignorées
                    OnSettingClicked(r.id);
                    return 0;
                }
            }
            if (!Contains(m_settingsPanel, p) && HitButton(p) == Btn::None) {
                m_showSettings = false;
                Invalidate();
                return 0;
            }
        }
        if (!m_edit.active) {  // panneau Couches : choix du canal affiché
            for (const ChannelRow& r : m_channelRows) {
                if (Contains(r.rc, p)) {
                    if (r.ch >= 10) ToggleChannel(r.ch - 11);  // case à cocher (11..14 = R, V, B, A)
                    else if (r.ch >= 0) SetChannel(r.ch);      // ligne : ce canal seul ; -1 : fond du panneau
                    return 0;
                }
            }
        }
        if (m_edit.active) {
            if (EditMouseDown(p, false, msg == WM_LBUTTONDBLCLK)) return 0;
            // sinon : outil « Déplacer » ou Espace maintenu -> glisser la vue (ci-dessous)
        } else if (Btn b = HitButton(p); b != Btn::None) {
            OnButton(b);
            return 0;
        }
        if (int i = HitStrip(p); i >= 0) {
            if (i != m_index) GoTo(i);
            return 0;
        }
        if (int z = HitNavZone(p); z != 0) {
            Navigate(z);
            return 0;
        }
        if (msg == WM_LBUTTONDBLCLK && !m_edit.active) {
            ToggleActualFit(p);
            return 0;
        }
        m_dragging = true;
        m_dragStart = p;
        m_zoom = m_zoomT;
        m_panX = m_panXT;
        m_panY = m_panYT;
        m_dragPanX = m_panXT;
        m_dragPanY = m_panYT;
        SetCapture(m_hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
        if (m_dragging) {
            m_dragging = false;
            ReleaseCapture();
            Poke();
            Invalidate();
        } else if (m_edit.active && msg == WM_LBUTTONUP) {
            EditMouseUp(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        }
        return 0;
    case WM_CAPTURECHANGED:
        m_dragging = false;
        if (m_edit.active && (m_edit.drawing || m_edit.slider >= 0) && reinterpret_cast<HWND>(lp) != m_hwnd) {
            m_edit.slider = -1;
            if (m_edit.drawing) EditMouseUp(m_mouse);  // capture perdue : on termine le tracé
        }
        return 0;
    case WM_MBUTTONDOWN:
        if (m_edit.active) {  // édition : clic molette = déplacer la vue
            m_dragging = true;
            m_dragStart = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            m_zoom = m_zoomT;
            m_panX = m_dragPanX = m_panXT;
            m_panY = m_dragPanY = m_panYT;
            SetCapture(m_hwnd);
        } else {
            ToggleFullscreen();
        }
        return 0;
    case WM_XBUTTONUP:
        if (m_slideshow) {
            SlideGo(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : 1, false);
            return TRUE;
        }
        Navigate(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : 1);
        return TRUE;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(m_hwnd, &p);
        double notches = GET_WHEEL_DELTA_WPARAM(wp) / double(WHEEL_DELTA);
        if (m_slideshow) {  // diaporama : la molette change d'image
            SlideGo(notches < 0 ? 1 : -1, false);
            Poke();
            return 0;
        }
        if (m_edit.active && EditWheel(p, notches)) return 0;  // liste des calques
        if (m_showSettings && !m_edit.active && Contains(m_settingsPanel, p)) {  // défilement du panneau Paramètres
            m_settingsScroll -= float(notches) * 60 * m_scale;
            Invalidate();
            return 0;
        }
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        if (StripVisible() && m_hud > 0.3f && p.y > rc.bottom - 100 * m_scale) {
            m_stripPosT = std::clamp(m_stripPosT - notches * 3, 0.0, double(m_files.size() - 1));
            RequestThumbs();
            Invalidate();
        } else {
            ZoomStep(std::pow(1.2, notches), p);
        }
        Poke();
        return 0;
    }
    case WM_KEYDOWN:
        Poke();
        if (m_edit.active && EditKey(wp)) return 0;
        OnKey(wp);
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && m_edit.spacePan) {
            m_edit.spacePan = false;
            Invalidate();
        }
        return 0;
    case WM_CHAR:
        if (m_edit.active && EditChar(wchar_t(wp))) return 0;
        if (wp == L'+' || wp == L'=') ZoomStep(1.25, ClientCenter());
        else if (wp == L'-' || wp == L'_') ZoomStep(1 / 1.25, ClientCenter());
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) {  // Alt+Entrée
            ToggleFullscreen();
            return 0;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        wchar_t file[MAX_PATH * 4];
        if (DragQueryFileW(drop, 0, file, ARRAYSIZE(file))) OpenPath(file);
        DragFinish(drop);
        SetForegroundWindow(m_hwnd);
        return 0;
    }
    case WM_CLOSE:
        if (!EndEdit(true)) return 0;  // modifications non enregistrées : l'utilisateur a annulé
        DestroyWindow(m_hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(m_hwnd, TIMER_HUD);
        SaveSettings();
        m_loader.reset();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
