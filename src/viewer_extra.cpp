// Presse-papier, menu contextuel (clic droit), vue d'un canal seul et lecture des animations.

#include "viewer.h"
#include "viewer_util.h"

#include <d2d1effects.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

using namespace ui;

namespace {

enum MenuId {
    kMenuCopy = 1, kMenuCopyPath, kMenuOpen, kMenuSaveAs, kMenuEdit, kMenuReveal, kMenuPlay, kMenuRotate,
    kMenuFull, kMenuInfo, kMenuSettings, kMenuSlideshow, kMenuChannel0 = 100, kMenuFilter0 = 200  // + canal
};

// Menus Win32 sombres, comme le reste de l'interface (uxtheme, fonctions non documentées mais
// stables depuis Windows 10 1903 : SetPreferredAppMode = n° 135, FlushMenuThemes = n° 136).
void EnableDarkMenus() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetModeFn = int(WINAPI*)(int);
    using FlushFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(2);  // ForceDark
    if (flush) flush();
}

bool SetClipboardBlob(UINT format, const void* data, size_t size) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    void* p = GlobalLock(mem);
    memcpy(p, data, size);
    GlobalUnlock(mem);
    if (!SetClipboardData(format, mem)) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

// Encode en PNG (alpha conservé) pour les logiciels qui lisent le format « PNG » du presse-papier.
bool EncodePng(const uint8_t* straightBgra, uint32_t w, uint32_t h, uint32_t stride, std::vector<uint8_t>& out) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h)))
        return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&pf)) || pf != GUID_WICPixelFormat32bppBGRA ||
        FAILED(frame->WritePixels(h, stride, UINT(size_t(stride) * h), const_cast<BYTE*>(straightBgra))) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    out.resize(size_t(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    return SUCCEEDED(stream->Read(out.data(), ULONG(out.size()), &read)) && read == out.size();
}

}  // namespace

// =============================================================================================
// Presse-papier

void Viewer::CopyToClipboard() {
    PixelBuffer px;
    uint32_t w = 0, h = 0, stride = 0;
    bool alpha = false;
    if (m_edit.active) {
        if (m_edit.textActive) CommitText();
        if (!ReadCanvas(px, stride)) {
            ShowToast(L"Lecture de l'image impossible");
            return;
        }
        w = m_edit.w;
        h = m_edit.h;
        alpha = m_edit.hasAlpha;
    } else {
        if (!m_cur) {
            ShowToast(Shown() ? L"Image en cours de chargement…" : L"Aucune image à copier");
            return;
        }
        bool flip;
        int rot;
        FlipRot(flip, rot);
        if (!OrientPixels(CurPixels(), m_cur->width, m_cur->height, m_cur->stride, flip, rot, px, w, h, stride)) {
            ShowToast(L"Mémoire insuffisante");
            return;
        }
        alpha = m_cur->hasAlpha;
    }

    // Les applications attendent un alpha « droit » (non prémultiplié)
    if (alpha) {
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* p = px.Data() + size_t(y) * stride;
            for (uint32_t x = 0; x < w; ++x, p += 4) {
                const uint32_t a = p[3];
                if (a == 255) continue;
                if (a == 0) {
                    p[0] = p[1] = p[2] = 0;
                    continue;
                }
                for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::min<uint32_t>(255, (p[c] * 255 + a / 2) / a));
            }
        }
    }

    // CF_DIBV5 32 bits (Windows en déduit CF_DIB / CF_BITMAP pour les applications anciennes)
    const size_t rowBytes = size_t(w) * 4;
    const size_t total = sizeof(BITMAPV5HEADER) + rowBytes * h;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!dib) {
        ShowToast(L"Mémoire insuffisante");
        return;
    }
    auto* hdr = static_cast<BITMAPV5HEADER*>(GlobalLock(dib));
    ZeroMemory(hdr, sizeof(*hdr));
    hdr->bV5Size = sizeof(BITMAPV5HEADER);
    hdr->bV5Width = LONG(w);
    hdr->bV5Height = LONG(h);  // positif : lignes de bas en haut, le plus compatible
    hdr->bV5Planes = 1;
    hdr->bV5BitCount = 32;
    hdr->bV5Compression = BI_BITFIELDS;
    hdr->bV5SizeImage = DWORD(rowBytes * h);
    hdr->bV5RedMask = 0x00FF0000;
    hdr->bV5GreenMask = 0x0000FF00;
    hdr->bV5BlueMask = 0x000000FF;
    hdr->bV5AlphaMask = 0xFF000000;
    hdr->bV5CSType = LCS_sRGB;
    hdr->bV5Intent = LCS_GM_IMAGES;
    uint8_t* bits = reinterpret_cast<uint8_t*>(hdr) + sizeof(BITMAPV5HEADER);
    for (uint32_t y = 0; y < h; ++y) memcpy(bits + (h - 1 - y) * rowBytes, px.Data() + size_t(y) * stride, rowBytes);
    GlobalUnlock(dib);

    // PNG en plus quand il y a de la transparence (Photoshop, GIMP, navigateurs, Office...)
    std::vector<uint8_t> png;
    if (alpha) EncodePng(px.Data(), w, h, stride, png);

    if (!OpenClipboard(m_hwnd)) {
        GlobalFree(dib);
        ShowToast(L"Presse-papier occupé, réessayez");
        return;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIBV5, dib) != nullptr;
    if (!ok) GlobalFree(dib);
    if (!png.empty()) SetClipboardBlob(RegisterClipboardFormatW(L"PNG"), png.data(), png.size());
    CloseClipboard();
    ShowToast(ok ? L"Image copiée" : L"Copie impossible");
}

void Viewer::CopyPathToClipboard() {
    const FileEntry* cur = Current();
    if (!cur || !OpenClipboard(m_hwnd)) return;
    EmptyClipboard();
    SetClipboardBlob(CF_UNICODETEXT, cur->path.c_str(), (cur->path.size() + 1) * sizeof(wchar_t));
    CloseClipboard();
    ShowToast(L"Chemin copié");
}

void Viewer::RevealInExplorer() {
    const FileEntry* cur = Current();
    if (!cur) return;
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(cur->path.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

// =============================================================================================
// Menu contextuel

void Viewer::ShowContextMenu(POINT client) {
    EnableDarkMenus();
    const bool has = m_cur != nullptr;
    const bool file = Current() != nullptr;
    HMENU menu = CreatePopupMenu();
    auto add = [&](UINT id, const wchar_t* text, bool enabled = true, bool checked = false) {
        AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, text);
    };
    add(kMenuCopy, L"Copier l'image\tCtrl+C", has);
    add(kMenuCopyPath, L"Copier le chemin du fichier\tCtrl+Maj+C", file);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(kMenuOpen, L"Ouvrir…\tCtrl+O");
    add(kMenuSaveAs, L"Enregistrer sous…\tCtrl+S", has);
    add(kMenuEdit, L"Modifier\tE", has);
    add(kMenuReveal, L"Afficher dans l'Explorateur", file);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU channels = CreatePopupMenu();
    for (int c = 0; c <= 4; ++c) {
        static const wchar_t* labels[] = {L"RVB (toutes les couleurs)\tCtrl+2", L"Rouge\tCtrl+3", L"Vert\tCtrl+4",
                                          L"Bleu\tCtrl+5", L"Alpha (transparence)\tCtrl+6"};
        const bool enabled = has && (c != 4 || m_cur->hasAlpha);
        AppendMenuW(channels, MF_STRING | (enabled ? 0 : MF_GRAYED), kMenuChannel0 + c, labels[c]);
    }
    CheckMenuRadioItem(channels, kMenuChannel0, kMenuChannel0 + 4, kMenuChannel0 + m_channel, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP | (has ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(channels), L"Canal affiché\tC");
    HMENU filters = CreatePopupMenu();
    for (int f = 0; f <= 5; ++f) AppendMenuW(filters, MF_STRING, kMenuFilter0 + f, f == 1 ? L"Pixels bruts\tN" : FilterName(f));
    CheckMenuRadioItem(filters, kMenuFilter0, kMenuFilter0 + 5, kMenuFilter0 + m_filter, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(filters), L"Filtrage");
    if (Animated()) add(kMenuPlay, m_playing ? L"Pause\tEspace" : L"Lecture\tEspace");
    add(kMenuRotate, L"Pivoter\tR", has);
    add(kMenuSlideshow, m_slideshow ? L"Quitter le diaporama\tÉchap" : L"Diaporama\tD", m_files.size() > 1);
    add(kMenuFull, L"Plein écran\tF", true, m_fullscreen);
    add(kMenuInfo, L"Informations\tI", true, m_showInfo);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(kMenuSettings, L"Paramètres\tP");

    POINT sp = client;
    ClientToScreen(m_hwnd, &sp);
    SetForegroundWindow(m_hwnd);  // sinon le menu ne se ferme pas en cliquant ailleurs
    const UINT cmd = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, sp.x, sp.y, 0, m_hwnd, nullptr));
    DestroyMenu(menu);  // détruit aussi le sous-menu
    switch (cmd) {
    case kMenuCopy: CopyToClipboard(); break;
    case kMenuCopyPath: CopyPathToClipboard(); break;
    case kMenuOpen: OpenDialog(); break;
    case kMenuSaveAs: SaveAs(); break;
    case kMenuEdit: BeginEdit(); break;
    case kMenuReveal: RevealInExplorer(); break;
    case kMenuPlay: TogglePlay(); break;
    case kMenuRotate: Rotate(1); break;
    case kMenuFull: ToggleFullscreen(); break;
    case kMenuSlideshow: m_slideshow ? StopSlideshow() : StartSlideshow(); break;
    case kMenuInfo: OnButton(Btn::Info); break;
    case kMenuSettings: OnButton(Btn::Settings); break;
    default:
        if (cmd >= kMenuChannel0 && cmd <= kMenuChannel0 + 4) SetChannel(int(cmd - kMenuChannel0));
        else if (cmd >= kMenuFilter0 && cmd <= kMenuFilter0 + 5) SetFilter(int(cmd - kMenuFilter0));
        break;
    }
    Invalidate();
}

// =============================================================================================
// Canaux : affiche R, V, B ou l'alpha seul en niveaux de gris (comme la palette Couches de Photoshop)

const wchar_t* Viewer::ChannelName(int ch) {
    static const wchar_t* names[] = {L"RVB", L"R", L"V", L"B", L"A"};
    return names[std::clamp(ch, 0, 4)];
}

void Viewer::SetChannel(int ch) {
    ch = std::clamp(ch, 0, 4);
    if (ch == 4 && m_cur && !m_cur->hasAlpha) {
        ShowToast(L"Cette image n'a pas de transparence");
        return;
    }
    m_channel = ch;
    static const wchar_t* toast[] = {L"Canaux : RVB", L"Canal rouge", L"Canal vert", L"Canal bleu", L"Canal alpha"};
    ShowToast(toast[ch]);
    Invalidate();
}

void Viewer::CycleChannel(int delta) {
    const int n = m_cur && m_cur->hasAlpha ? 5 : 4;
    SetChannel(((m_channel + delta) % n + n) % n);
}

void Viewer::DrawTile(ID2D1Bitmap* bmp, const D2D1_RECT_F& dst, D2D1_INTERPOLATION_MODE mode, int channel) {
    if (channel < 0) channel = m_edit.active ? 0 : m_channel;
    if (channel == 0) {
        m_dc->DrawBitmap(bmp, dst, 1.0f, mode, nullptr, nullptr);
        return;
    }
    if (!m_channelFx && FAILED(m_dc->CreateEffect(CLSID_D2D1ColorMatrix, &m_channelFx))) {
        m_dc->DrawBitmap(bmp, dst, 1.0f, mode, nullptr, nullptr);
        return;
    }
    // Lignes de la matrice = canal d'entrée (R, V, B, A, constante), colonnes = sortie (R, V, B, A).
    // L'effet travaille en alpha « droit » : on voit la vraie valeur du canal, opaque.
    D2D1_MATRIX_5X4_F m{};
    float* f = &m._11;
    const int in = channel - 1;  // 0..3
    f[in * 4 + 0] = f[in * 4 + 1] = f[in * 4 + 2] = 1.0f;
    m._54 = 1.0f;
    m_channelFx->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
    m_channelFx->SetInput(0, bmp);

    // L'effet se dessine à sa taille en pixels : mise à l'échelle vers dst si besoin (vignette)
    const D2D1_SIZE_U sz = bmp->GetPixelSize();
    D2D1_MATRIX_3X2_F old;
    m_dc->GetTransform(&old);
    const float sx = (dst.right - dst.left) / float(sz.width), sy = (dst.bottom - dst.top) / float(sz.height);
    m_dc->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(dst.left, dst.top) *
                       D2D1::Matrix3x2F(old._11, old._12, old._21, old._22, old._31, old._32));
    m_dc->DrawImage(m_channelFx.Get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE(mode), D2D1_COMPOSITE_MODE_SOURCE_OVER);
    m_dc->SetTransform(old);
    m_channelFx->SetInput(0, nullptr);
}

// Miniature orientée de l'image courante, dessinée une fois par image (et par image d'animation)
// puis réutilisée pour les 4 ou 5 aperçus du panneau.
void Viewer::UpdateChannelPreview() {
    const FileEntry* cur = Current();
    if (!m_cur || !m_dc || !cur) return;
    bool flip;
    int rot;
    FlipRot(flip, rot);
    const int orient = rot | (flip ? 4 : 0);
    const size_t frame = m_animImg == m_cur ? m_frame : 0;
    if (m_chanPreview && m_chanPreviewSrc == m_cur && m_chanPreviewFrame == frame && m_chanPreviewRot == orient) return;
    GpuImage* g = GetGpuImage(m_cur, Key(cur->path));
    if (!g) return;
    SyncGpuFrame(g);

    const float W = float(m_cur->width), H = float(m_cur->height);
    const float dw = (rot & 1) ? H : W, dh = (rot & 1) ? W : H;
    const float z = std::min(1.0f, 256.0f / std::max(dw, dh));
    const UINT32 pw = std::max(1u, UINT32(std::ceil(dw * z))), ph = std::max(1u, UINT32(std::ceil(dh * z)));
    if (!m_chanPreview || m_chanPreview->GetPixelSize().width != pw || m_chanPreview->GetPixelSize().height != ph) {
        m_chanPreview.Reset();
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, bp, &m_chanPreview))) return;
    }
    ComPtr<ID2D1Image> oldTarget;
    m_dc->GetTarget(&oldTarget);
    m_dc->SetTarget(m_chanPreview.Get());
    m_dc->BeginDraw();
    m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    m_dc->SetTransform(ImageTransform(W, H, flip, rot, z, pw / 2.0f, ph / 2.0f));
    for (auto& t : g->tiles)
        m_dc->DrawBitmap(t.bmp.Get(), t.dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dc->EndDraw();
    m_dc->SetTarget(oldTarget.Get());
    m_chanPreviewSrc = m_cur;
    m_chanPreviewFrame = frame;
    m_chanPreviewRot = orient;
}

// Panneau « Couches » (comme Photoshop) : un aperçu par canal, clic pour l'afficher.
void Viewer::DrawChannelPanel(float cw, float ch) {
    if (!m_cur) return;
    const float s = m_scale;
    const bool alpha = m_cur->hasAlpha;
    const int n = alpha ? 5 : 4;
    const float rowH = 70 * s, pad = 12 * s, width = 250 * s, top = 58 * s;
    float height = pad * 2 + 30 * s + rowH * n;
    height = std::min(height, ch - top - 12 * s);
    D2D1_RECT_F p = D2D1::RectF(cw - width - 12 * s, top, cw - 12 * s, top + height);
    FillRound(p, 8 * s, kPanel, 0.95f);
    Text(L"Couches", m_tfTitle.Get(), D2D1::RectF(p.left + pad, p.top + pad - 4 * s, p.right - pad, p.top + pad + 22 * s), kWhite, 1);
    static const wchar_t* names[] = {L"RVB", L"Rouge", L"Vert", L"Bleu", L"Alpha"};
    static const wchar_t* keys[] = {L"Ctrl+2", L"Ctrl+3", L"Ctrl+4", L"Ctrl+5", L"Ctrl+6"};
    m_rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = p.top + pad + 30 * s;
    for (int c = 0; c < n; ++c, y += rowH) {
        const D2D1_RECT_F row = D2D1::RectF(p.left + pad / 2, y, p.right - pad / 2, y + rowH - 4 * s);
        if (c == m_channel) FillRound(row, 6 * s, kAccent, 0.25f);
        else if (m_mouseInside && Contains(row, m_mouse)) FillRound(row, 6 * s, kWhite, 0.07f);
        const D2D1_RECT_F box = D2D1::RectF(row.left + 6 * s, row.top + 5 * s, row.left + 6 * s + 92 * s, row.bottom - 5 * s);
        if (m_chanPreview) {
            const D2D1_SIZE_U sz = m_chanPreview->GetPixelSize();
            const float k = std::min((box.right - box.left) / sz.width, (box.bottom - box.top) / sz.height);
            const float w = sz.width * k, h = sz.height * k;
            const float l = std::round((box.left + box.right - w) / 2), t = std::round((box.top + box.bottom - h) / 2);
            const D2D1_RECT_F r = D2D1::RectF(l, t, l + w, t + h);
            if (c == 0 && alpha && m_checker) m_rt->FillRectangle(r, m_checker.Get());
            DrawTile(m_chanPreview.Get(), r, D2D1_INTERPOLATION_MODE_LINEAR, c);
        }
        const float tx = box.right + 12 * s, mid = (row.top + row.bottom) / 2;
        Text(names[c], m_tfTitle.Get(), D2D1::RectF(tx, mid - 21 * s, row.right - 6 * s, mid + 1 * s), c == m_channel ? kAccent : kWhite, 1);
        Text(keys[c], m_tfSmall.Get(), D2D1::RectF(tx, mid + 1 * s, row.right - 6 * s, mid + 21 * s), kGrey, 1);
        m_channelRows.push_back({c, row});
    }
    m_rt->PopAxisAlignedClip();
    m_channelRows.push_back({-1, p});  // le reste du panneau absorbe les clics
}

// =============================================================================================
// Animation

void Viewer::SyncGpuFrame(GpuImage* g) {
    if (!g || g->src != m_animImg || !m_animImg || g->frame == m_frame || m_frame >= m_animImg->FrameCount()) return;
    const DecodedImage& img = *m_animImg;
    const uint8_t* px = img.FramePixels(m_frame);
    for (auto& t : g->tiles) {
        const uint8_t* p = px + size_t(t.dst.top) * img.stride + size_t(t.dst.left) * 4;
        t.bmp->CopyFromMemory(nullptr, p, img.stride);
    }
    g->frame = m_frame;
}

const uint8_t* Viewer::CurPixels() const {
    if (!m_cur) return nullptr;
    const size_t i = m_animImg == m_cur ? std::min(m_frame, m_cur->FrameCount() - 1) : 0;
    return m_cur->FramePixels(i);
}

static uint32_t FrameDelay(const DecodedImage& img, size_t i) {
    return i < img.delays.size() ? std::max<uint32_t>(img.delays[i], 10) : 100;
}

void Viewer::SyncAnimation() {
    if (m_animImg == m_cur) return;
    m_animImg = m_cur;
    m_frame = 0;
    m_playing = true;
    if (m_animImg && m_animImg->FrameCount() > 1) {
        m_frameDue = NowSeconds() + FrameDelay(*m_animImg, 0) / 1000.0;
        ScheduleFrame();
    } else {
        KillTimer(m_hwnd, kTimerAnim);
    }
}

void Viewer::ScheduleFrame() {
    if (!m_playing || !m_animImg || m_animImg->FrameCount() < 2) {
        KillTimer(m_hwnd, kTimerAnim);
        return;
    }
    const double ms = (m_frameDue - NowSeconds()) * 1000.0;
    SetTimer(m_hwnd, kTimerAnim, UINT(std::clamp(ms, double(USER_TIMER_MINIMUM), 60000.0)), nullptr);
}

void Viewer::OnAnimTimer() {
    if (m_animImg != m_cur) SyncAnimation();
    if (!m_animImg || m_animImg->FrameCount() < 2 || !m_playing) {
        KillTimer(m_hwnd, kTimerAnim);
        return;
    }
    if (m_edit.active) {  // en pause pendant l'édition (le document est une copie fixe)
        m_frameDue = NowSeconds() + 0.25;
        ScheduleFrame();
        return;
    }
    const double now = NowSeconds();
    if (now < m_frameDue - 0.002) {  // minuterie Windows en avance
        ScheduleFrame();
        return;
    }
    const size_t n = m_animImg->FrameCount();
    while (now >= m_frameDue) {
        m_frame = (m_frame + 1) % n;
        m_frameDue += FrameDelay(*m_animImg, m_frame) / 1000.0;
        if (now - m_frameDue > 0.5) m_frameDue = now + FrameDelay(*m_animImg, m_frame) / 1000.0;  // retard (fenêtre bloquée)
    }
    Invalidate();
    ScheduleFrame();
}

void Viewer::StepFrame(int delta) {
    if (!Animated()) return;
    SyncAnimation();
    const size_t n = m_cur->FrameCount();
    m_playing = false;
    KillTimer(m_hwnd, kTimerAnim);
    m_frame = size_t((int64_t(m_frame) + delta + int64_t(n)) % int64_t(n));
    ShowToast(L"Image " + std::to_wstring(m_frame + 1) + L" / " + std::to_wstring(n));
    Invalidate();
}

void Viewer::TogglePlay() {
    if (!Animated()) return;
    SyncAnimation();
    m_playing = !m_playing;
    if (m_playing) m_frameDue = NowSeconds() + FrameDelay(*m_cur, m_frame) / 1000.0;
    ScheduleFrame();
    ShowToast(m_playing ? L"Lecture" : L"Pause");
    Invalidate();
}

// =============================================================================================
// Logo

ID2D1Bitmap* Viewer::Logo() {
    if (m_logo || !m_dc) return m_logo.Get();
    HRSRC res = FindResourceW(m_inst, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(m_inst, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return nullptr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), SizeofResource(m_inst, res))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    m_dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &m_logo);
    return m_logo.Get();
}

// =============================================================================================
// Filtrage

const wchar_t* Viewer::FilterName(int f) {
    static const wchar_t* names[] = {L"Automatique", L"Pixels bruts", L"Linéaire", L"Cubique", L"Cubique HQ", L"Anisotrope"};
    return names[std::clamp(f, 0, 5)];
}

// Automatique : pixel exact à 100 %, bicubique haute qualité en réduction, cubique en agrandissement
// modéré, pixels nets au-delà de 300 %. Les autres choix s'appliquent à tous les niveaux de zoom.
// Pendant un mouvement (zoom animé, glisser), filtrage linéaire, sauf en pixels bruts.
D2D1_INTERPOLATION_MODE Viewer::FilterFor(float z, bool moving) const {
    if (m_filter == 1) return D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    if (moving) return D2D1_INTERPOLATION_MODE_LINEAR;
    switch (m_filter) {
    case 2: return D2D1_INTERPOLATION_MODE_LINEAR;
    case 3: return D2D1_INTERPOLATION_MODE_CUBIC;
    case 4: return D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    case 5: return D2D1_INTERPOLATION_MODE_ANISOTROPIC;
    default: break;
    }
    if (std::fabs(z - 1.0f) < 1e-4f || z >= 3.0f) return D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    return z < 1.0f ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_CUBIC;
}

void Viewer::SetFilter(int f) {
    f = std::clamp(f, 0, 5);
    if (f == 1 && m_filter != 1) m_filterBeforeRaw = m_filter;
    m_filter = f;
    SaveSetting(L"view", L"filter", m_filter);
    ShowToast(std::wstring(L"Filtrage : ") + FilterName(f));
    Invalidate();
}
