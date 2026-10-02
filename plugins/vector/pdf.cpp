// PDF (et Illustrator .ai enregistré avec la compatibilité PDF) : moteur PDF de Windows
// (Windows.Data.Pdf), rendu de la page 1 par IPdfRendererNative sur un contexte Direct2D.
// Le périphérique Direct3D est WARP (logiciel) : créé en quelques millisecondes et sans dépendre
// du pilote graphique (la création d'un périphérique matériel coûte des centaines de ms).

#include "vector.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shcore.h>
#include <windows.data.pdf.interop.h>

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <mutex>


namespace vec {

namespace {

using namespace winrt::Windows::Data::Pdf;

struct PdfDevice {
    ID3D11Device* d3d = nullptr;
    IDXGIDevice* dxgi = nullptr;
    ID2D1Device* d2d = nullptr;
};

// Créé une fois, jamais libéré (comme les fabriques)
const PdfDevice* SharedDevice() {
    static PdfDevice d;
    static std::once_flag once;
    std::call_once(once, [] {
        if (!D2DFactory()) return;
        ID3D11Device* dev = nullptr;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
            return;
        IDXGIDevice* dxgi = nullptr;
        ID2D1Device* d2d = nullptr;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(D2DFactory()->CreateDevice(dxgi, &d2d))) {
            if (dxgi) dxgi->Release();
            dev->Release();
            return;
        }
        d.d3d = dev, d.dxgi = dxgi, d.d2d = d2d;
    });
    return d.d2d ? &d : nullptr;
}

struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

int Render(const Call& c, const wchar_t* path, bool ai) {
    // Flux WinRT ouvert directement sur le fichier (plus rapide que StorageFile)
    winrt::Windows::Storage::Streams::IRandomAccessStream stream{nullptr};
    if (FAILED(CreateRandomAccessStreamOnFile(path, 0 /* FileAccessMode::Read */,
                                              winrt::guid_of<winrt::Windows::Storage::Streams::IRandomAccessStream>(),
                                              winrt::put_abi(stream))))
        return c.Fail(L"Ouverture du PDF impossible");
    PdfDocument doc{nullptr};
    try {
        doc = PdfDocument::LoadFromStreamAsync(stream).get();
    } catch (const winrt::hresult_error& e) {
        if (e.code() == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) || e.code() == HRESULT_FROM_WIN32(ERROR_INVALID_PASSWORD))
            return c.Fail(L"PDF protégé par un mot de passe");
        return c.Fail(Fmt(L"PDF illisible ou endommagé (erreur 0x%08X)", unsigned(e.code().value)).c_str());
    }
    const uint32_t pages = doc.PageCount();
    if (!pages) return c.Fail(L"PDF sans aucune page");
    if (c.Cancelled()) return CYIMG_E_CANCELLED;
    PdfPage page = doc.GetPage(0);
    const auto size = page.Size();  // DIP (1/96 pouce), rotation comprise
    double w = size.Width, h = size.Height;
    if (!(w > 0) || !(h > 0)) return c.Fail(L"Page PDF de taille nulle");
    uint32_t rw = 0, rh = 0;
    RenderSize(c, w, h, rw, rh);

    const PdfDevice* dev = SharedDevice();
    if (!dev) return c.Fail(L"Direct3D 11 (WARP) / Direct2D indisponible");
    ComPtr<IPdfRendererNative> renderer;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(PdfCreateRenderer(dev->dxgi, &renderer)) ||
        FAILED(dev->d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
        return c.Fail(L"Moteur de rendu PDF indisponible");
    const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> target, readback;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(rw, rh), nullptr, 0,
                                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf), &target)) ||
        FAILED(dc->CreateBitmap(D2D1::SizeU(rw, rh), nullptr, 0,
                                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf),
                                &readback)))
        return c.Fail(L"Mémoire graphique insuffisante pour le rendu PDF");
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
    PDF_RENDER_PARAMS params = PdfRenderParams(D2D1::RectF(0, 0, 0, 0), rw, rh, D2D1::ColorF(D2D1::ColorF::White));
    HRESULT hr = renderer->RenderPageToDeviceContext(winrt::get_unknown(page), dc.Get(), &params);
    const HRESULT hrEnd = dc->EndDraw();
    if (FAILED(hr) || FAILED(hrEnd))
        return c.Fail(Fmt(L"Rendu de la page PDF impossible (erreur 0x%08X)", unsigned(FAILED(hr) ? hr : hrEnd)).c_str());
    dc->SetTarget(nullptr);
    D2D1_MAPPED_RECT map{};
    if (FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)) || FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &map)))
        return c.Fail(L"Lecture du rendu PDF impossible");
    const int r = DeliverPixels(c, map.bits, map.pitch, rw, rh, true);
    readback->Unmap();
    if (r != CYIMG_OK) return r;

    c.host->set_format(c.ctx, ai ? L"Adobe Illustrator (PDF)" : L"PDF");
    c.Info(L"Pages", Fmt(L"%u (page 1 affichée)", pages));
    c.Info(L"Taille de page", Fmt(L"%.0f × %.0f mm", w * 25.4 / 96, h * 25.4 / 96));
    return CYIMG_OK;
}

}  // namespace

int DecodePdf(const Call& c, const wchar_t* path, bool ai) {
    ComInit com;
    try {
        return Render(c, path, ai);
    } catch (const winrt::hresult_error& e) {
        return c.Fail(Fmt(L"Erreur du moteur PDF de Windows (0x%08X)", unsigned(e.code().value)).c_str());
    } catch (const std::bad_alloc&) {
        return CYIMG_E_OUTOFMEMORY;
    } catch (...) {
        return c.Fail(L"Erreur inattendue du moteur PDF");
    }
}

}  // namespace vec
