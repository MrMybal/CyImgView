#include "save.h"

#include "i18n.h"

#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>

namespace {

bool HasExt(const SaveFormat& f, const std::wstring& ext) {
    return std::find(f.exts.begin(), f.exts.end(), ext) != f.exts.end();
}

int Rank(const GUID& g) {  // ordre d'affichage : les plus courants d'abord
    const GUID order[] = {GUID_ContainerFormatJpeg, GUID_ContainerFormatPng, GUID_ContainerFormatWebp,
                          GUID_ContainerFormatHeif, GUID_ContainerFormatTiff, GUID_ContainerFormatBmp,
                          GUID_ContainerFormatGif,  GUID_ContainerFormatWmp};
    for (int i = 0; i < int(ARRAYSIZE(order)); ++i)
        if (order[i] == g) return i;
    return 100;
}

}  // namespace

std::vector<SaveFormat> EnumerateSaveFormats() {
    std::vector<SaveFormat> out;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    ComPtr<IEnumUnknown> en;
    if (FAILED(f->CreateComponentEnumerator(WICEncoder, WICComponentEnumerateDefault, &en))) return out;
    ComPtr<IUnknown> u;
    ULONG n = 0;
    while (en->Next(1, &u, &n) == S_OK) {
        ComPtr<IWICBitmapEncoderInfo> info;
        if (SUCCEEDED(u.As(&info))) {
            SaveFormat s;
            info->GetContainerFormat(&s.container);
            if (s.container == GUID_ContainerFormatDds || s.container == GUID_ContainerFormatIco) {
                u.Reset();
                continue;  // formats de textures/icônes : peu utiles ici
            }
            UINT len = 0;
            if (SUCCEEDED(info->GetFileExtensions(0, nullptr, &len)) && len > 1) {
                std::wstring e(len, L'\0');
                info->GetFileExtensions(len, e.data(), &len);
                std::wstring cur;
                for (wchar_t c : e + L",") {
                    if (c == L',' || c == L';' || c == L'\0') {
                        while (!cur.empty() && (cur.front() == L'.' || cur.front() == L' ')) cur.erase(0, 1);
                        if (!cur.empty()) s.exts.push_back(ToLower(cur));
                        cur.clear();
                        if (c == L'\0') break;
                    } else {
                        cur += c;
                    }
                }
            }
            if (s.container == GUID_ContainerFormatJpeg) s.name = L"JPEG";
            else if (s.container == GUID_ContainerFormatPng) s.name = L"PNG";
            else if (s.container == GUID_ContainerFormatTiff) s.name = L"TIFF";
            else if (s.container == GUID_ContainerFormatBmp) s.name = L"BMP";
            else if (s.container == GUID_ContainerFormatGif) s.name = L"GIF";
            else if (s.container == GUID_ContainerFormatWmp) s.name = L"JPEG XR";
            else if (s.container == GUID_ContainerFormatHeif) s.name = L"HEIF";
            else if (s.container == GUID_ContainerFormatWebp) s.name = L"WebP";
            else {
                UINT nl = 0;
                info->GetFriendlyName(0, nullptr, &nl);
                std::wstring fn(nl, L'\0');
                if (nl) info->GetFriendlyName(nl, fn.data(), &nl);
                fn.resize(wcslen(fn.c_str()));
                s.name = fn;
            }
            s.lossy = s.container == GUID_ContainerFormatJpeg || s.container == GUID_ContainerFormatWmp ||
                      s.container == GUID_ContainerFormatHeif || s.container == GUID_ContainerFormatWebp;
            s.alpha = s.container == GUID_ContainerFormatPng || s.container == GUID_ContainerFormatTiff ||
                      s.container == GUID_ContainerFormatWmp || s.container == GUID_ContainerFormatGif ||
                      s.container == GUID_ContainerFormatWebp || s.container == GUID_ContainerFormatHeif;
            // Encodeur déclaré mais inutilisable (ex. HEIF sans l'extension HEVC) : on ne le propose pas
            ComPtr<IWICBitmapEncoder> probe;
            if (!s.exts.empty() && SUCCEEDED(f->CreateEncoder(s.container, nullptr, &probe))) out.push_back(std::move(s));
        }
        u.Reset();
    }
    std::stable_sort(out.begin(), out.end(), [](const SaveFormat& a, const SaveFormat& b) { return Rank(a.container) < Rank(b.container); });
    return out;
}

bool ShowSaveDialog(HWND owner, const std::vector<SaveFormat>& formats, const std::wstring& folder,
                    const std::wstring& baseName, const std::wstring& preferredExt, SaveRequest& out) {
    if (formats.empty()) return false;
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;

    std::vector<std::wstring> labels, patterns;
    for (auto& f : formats) {
        std::wstring pat;
        for (auto& e : f.exts) pat += (pat.empty() ? L"*." : L";*.") + e;
        patterns.push_back(pat);
        labels.push_back(f.name + L" (" + pat + L")");
    }
    std::vector<COMDLG_FILTERSPEC> spec;
    for (size_t i = 0; i < formats.size(); ++i) spec.push_back({labels[i].c_str(), patterns[i].c_str()});
    dlg->SetFileTypes(UINT(spec.size()), spec.data());

    int def = 0;
    for (int i = 0; i < int(formats.size()); ++i)
        if (HasExt(formats[i], preferredExt)) def = i;
    dlg->SetFileTypeIndex(UINT(def + 1));
    // garde l'extension d'origine (.jpg plutôt que .jpeg) si elle appartient au format proposé
    dlg->SetDefaultExtension(HasExt(formats[def], preferredExt) ? preferredExt.c_str() : formats[def].exts[0].c_str());
    dlg->SetFileName(baseName.c_str());
    dlg->SetTitle(T("save.title"));
    if (!folder.empty()) {
        ComPtr<IShellItem> fi;
        if (SUCCEEDED(SHCreateItemFromParsingName((folder + L"\\").c_str(), nullptr, IID_PPV_ARGS(&fi))))
            dlg->SetFolder(fi.Get());
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);

    // Qualité JPEG / HEIF / WebP / JPEG XR
    constexpr DWORD kGroup = 100, kCombo = 101;
    static const int qualities[] = {100, 95, 92, 90, 85, 80, 75, 70, 60, 50};
    ComPtr<IFileDialogCustomize> cust;
    if (SUCCEEDED(dlg.As(&cust))) {
        cust->StartVisualGroup(kGroup, T("save.quality"));
        cust->AddComboBox(kCombo);
        for (int i = 0; i < int(ARRAYSIZE(qualities)); ++i) {
            const std::wstring b = Tf(qualities[i] == 92 ? "save.quality.recommended" : "save.quality.item", qualities[i]);
            cust->AddControlItem(kCombo, DWORD(i), b.c_str());
        }
        cust->SetSelectedControlItem(kCombo, 2);
        cust->EndVisualGroup();
    }

    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out.path = p;
    CoTaskMemFree(p);

    // Format : d'après l'extension saisie, sinon d'après le type choisi
    UINT idx = 1;
    dlg->GetFileTypeIndex(&idx);
    out.format = std::clamp(int(idx) - 1, 0, int(formats.size()) - 1);
    std::wstring ext = ExtensionOf(out.path);
    for (int i = 0; i < int(formats.size()); ++i)
        if (HasExt(formats[i], ext)) out.format = i;

    out.quality = 92;
    DWORD sel = 2;
    if (cust && SUCCEEDED(cust->GetSelectedControlItem(kCombo, &sel)) && sel < ARRAYSIZE(qualities))
        out.quality = qualities[sel];
    return true;
}

bool SaveImage(const SaveRequest& req, const SaveFormat& fmt, const uint8_t* pixels, uint32_t width, uint32_t height,
               uint32_t stride, bool hasAlpha, std::wstring& error) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        error = T("save.err.no_wic");
        return false;
    }
    const bool keepAlpha = hasAlpha && fmt.alpha;

    // Sans transparence dans le format cible : on compose sur du blanc (comme Paint), pas sur du noir.
    std::vector<uint8_t> flat;
    const uint8_t* src = pixels;
    uint32_t srcStride = stride;
    if (hasAlpha && !keepAlpha) {
        flat.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* s = pixels + size_t(y) * stride;
            uint8_t* d = flat.data() + size_t(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x, s += 4, d += 4) {
                uint8_t inv = uint8_t(255 - s[3]);
                d[0] = uint8_t(std::min(255, s[0] + inv));
                d[1] = uint8_t(std::min(255, s[1] + inv));
                d[2] = uint8_t(std::min(255, s[2] + inv));
                d[3] = 255;
            }
        }
        src = flat.data();
        srcStride = width * 4;
    }

    ComPtr<IWICBitmap> bmp;
    if (FAILED(f->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, srcStride, srcStride * height,
                                         const_cast<BYTE*>(src), &bmp))) {
        error = T("save.err.memory");
        return false;
    }

    // Fichier temporaire dans le même dossier, puis remplacement atomique
    std::wstring tmp = req.path + L".cyimg-tmp";
    bool ok = false;
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> props;
        if (FAILED(f->CreateEncoder(fmt.container, nullptr, &enc))) {
            error = Tf("save.err.no_encoder", fmt.name.c_str());
            return false;
        }
        if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE))) {
            error = T("save.err.cannot_write");
            return false;
        }
        HRESULT hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
        if (SUCCEEDED(hr) && props && fmt.lossy) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = std::clamp(req.quality, 1, 100) / 100.0f;
            props->Write(1, &opt, &v);  // ignoré si l'encodeur ne connaît pas l'option
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
        WICPixelFormatGUID pf = keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pf);  // l'encodeur peut proposer un autre format

        ComPtr<IWICBitmapSource> source = bmp;
        if (SUCCEEDED(hr) && pf != GUID_WICPixelFormat32bppPBGRA) {
            ComPtr<IWICFormatConverter> cv;
            ComPtr<IWICPalette> pal;
            const bool indexed = pf == GUID_WICPixelFormat8bppIndexed || pf == GUID_WICPixelFormat4bppIndexed ||
                                 pf == GUID_WICPixelFormat2bppIndexed || pf == GUID_WICPixelFormat1bppIndexed;
            if (indexed) {  // GIF : palette optimisée de 256 couleurs
                hr = f->CreatePalette(&pal);
                if (SUCCEEDED(hr)) hr = pal->InitializeFromBitmap(bmp.Get(), 256, keepAlpha);
                if (SUCCEEDED(hr)) frame->SetPalette(pal.Get());
            }
            if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&cv);
            if (SUCCEEDED(hr))
                hr = cv->Initialize(bmp.Get(), pf, indexed ? WICBitmapDitherTypeErrorDiffusion : WICBitmapDitherTypeNone,
                                    pal.Get(), 0, indexed ? WICBitmapPaletteTypeCustom : WICBitmapPaletteTypeMedianCut);
            if (SUCCEEDED(hr)) source = cv;
        }
        if (SUCCEEDED(hr)) hr = frame->WriteSource(source.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        ok = SUCCEEDED(hr);
        if (!ok) error = T("save.err.encoding");
    }
    if (ok && !MoveFileExW(tmp.c_str(), req.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = T("save.err.replace");
        ok = false;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool OrientPixels(const uint8_t* src, uint32_t w, uint32_t h, uint32_t stride, bool flip, int rot, PixelBuffer& dst,
                  uint32_t& dw, uint32_t& dh, uint32_t& dstride) {
    // Destination = fonction affine des coordonnées source : dx = ax*x + bx*y + cx, dy = ay*x + by*y + cy
    int64_t ax = 1, bx = 0, cx = 0, ay = 0, by = 1, cy = 0;
    int64_t W = w, H = h;
    if (flip) {
        ax = -ax;
        bx = -bx;
        cx = W - 1 - cx;
    }
    for (int i = 0; i < (rot & 3); ++i) {  // quart de tour horaire : (x, y) -> (H-1-y, x)
        int64_t nax = -ay, nbx = -by, ncx = H - 1 - cy;
        ay = ax;
        by = bx;
        cy = cx;
        ax = nax;
        bx = nbx;
        cx = ncx;
        std::swap(W, H);
    }
    dw = uint32_t(W);
    dh = uint32_t(H);
    dstride = dw * 4;
    if (!dst.Alloc(size_t(dstride) * dh)) return false;
    uint32_t* out = reinterpret_cast<uint32_t*>(dst.Data());
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(src + size_t(y) * stride);
        int64_t dx = bx * y + cx, dy = by * y + cy;
        for (uint32_t x = 0; x < w; ++x, dx += ax, dy += ay) out[size_t(dy) * dw + size_t(dx)] = s[x];
    }
    return true;
}
