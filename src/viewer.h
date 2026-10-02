#pragma once

#include "common.h"
#include "decoder.h"
#include "folder.h"
#include "loader.h"
#include "save.h"
#include "updater.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <functional>
#include <unordered_map>

// Lance la création du device Direct3D sur un thread dès le démarrage du processus :
// c'est l'étape la plus lente du lancement (pilote GPU), elle se fait pendant le reste.
void StartGpuWarmup();

// %APPDATA%\CyImgView\settings.ini (créé au besoin)
std::wstring SettingsIniPath();

// Option « une seule fenêtre » : transmet le fichier à une fenêtre CyImgView déjà ouverte.
// Retourne true si elle l'a pris en charge (ce processus peut alors se terminer).
bool ForwardToRunningInstance(const std::wstring& path);

class Viewer {
public:
    Viewer() = default;
    ~Viewer();

    bool Create(HINSTANCE inst, int show, const std::wstring& initialPath);
    int Run();

private:
    enum class Btn { None, Open, Edit, SaveAs, Prev, Next, ZoomOut, ZoomIn, Fit, Actual, Rotate, Strip, Info, Help, Full, Update, Settings };
    struct SettingRow {
        int id;
        D2D1_RECT_F rc;
    };

    // --- Mode édition (viewer_edit.cpp) ---
    enum class Tool { Hand, Brush, Eraser, Line, Arrow, Rect, Ellipse, Text, Fill, Picker, Crop };
    enum class EditCmd {
        None, Tool, Color, Color2, SwapColors, CustomColor, Undo, Redo, Slider, FillToggle,
        RotL, RotR, FlipH, FlipV, ResizeMenu, ResizeOpt, SaveAs, Done, CropApply, CropCancel
    };
    struct EditButton {
        EditCmd cmd;
        int arg;
        D2D1_RECT_F rc;
        const wchar_t* tip;
    };
    struct EditState {
        bool active = false;
        ComPtr<ID2D1Bitmap1> canvas;  // document, sur le GPU
        uint32_t w = 0, h = 0;
        bool hasAlpha = false;
        bool dirty = false;
        std::vector<ComPtr<ID2D1Bitmap1>> undo, redo;
        Tool tool = Tool::Brush, prevTool = Tool::Brush;
        uint32_t color = 0xE53935, color2 = 0xFFFFFF;
        float size = 8, fontSize = 48;
        bool fill = false;
        // interaction en cours (coordonnées image)
        bool drawing = false, secondary = false, shift = false;
        D2D1_POINT_2F start{}, cur{};
        bool sliderDrag = false, spacePan = false, resizeMenu = false;
        bool cropValid = false;
        D2D1_RECT_F crop{};
        bool textActive = false;
        D2D1_POINT_2F textPos{};
        std::wstring text;
    };
    struct ButtonRect {
        Btn id;
        D2D1_RECT_F rc;
        const wchar_t* glyph;
        const wchar_t* tip;
    };
    struct Tile {
        ComPtr<ID2D1Bitmap> bmp;
        D2D1_RECT_F dst;
    };
    struct GpuImage {
        std::wstring key;
        ImagePtr src;
        std::vector<Tile> tiles;
        uint64_t lastUse = 0;
    };
    struct GpuThumb {
        ComPtr<ID2D1Bitmap> bmp;
        ImagePtr src;
        uint64_t lastUse = 0;
    };
    struct ThumbEntry {
        ImagePtr img;
        uint64_t lastUse = 0;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // Ressources graphiques
    bool CreateDeviceResources();
    void DiscardDeviceResources();
    bool CreateTargetBitmap();
    void ResizeSwapChain(UINT w, UINT h);
    void CreateTextFormats();
    void Render();
    void DrawImage(float cw, float ch, bool animating);
    void DrawHud(float cw, float ch);
    void DrawStrip(float cw, float ch, float alpha);
    void DrawInfoPanel(float cw, float ch);
    void DrawHelp(float cw, float ch);
    void DrawSettings(float cw, float ch);
    void OnSettingClicked(int id);
    void SaveSetting(const wchar_t* section, const wchar_t* key, int value);
    void DrawSpinner(float cx, float cy, float r, float alpha);
    void DrawCenteredMessage(float cw, float ch, const wchar_t* glyph, const std::wstring& line1, const std::wstring& line2);
    GpuImage* GetGpuImage(const ImagePtr& img, const std::wstring& key);
    ID2D1Bitmap* GetGpuThumb(const std::wstring& key);
    void PreUploadNeighbor();
    void SetColor(uint32_t rgb, float a);
    void FillRound(const D2D1_RECT_F& r, float radius, uint32_t rgb, float a);
    void Text(const std::wstring& s, IDWriteTextFormat* tf, const D2D1_RECT_F& r, uint32_t rgb, float a);

    // Fichiers / navigation
    void OpenPath(std::wstring path);
    void OpenDialog();
    void GoTo(int index);
    void Navigate(int delta);
    void RequestLoads();
    void RequestThumbs();
    void OnLoaded(LoadResult* r);
    void EnforceBudget();
    std::wstring Key(const std::wstring& path) const { return ToLower(path); }
    const FileEntry* Current() const { return m_index >= 0 && m_index < int(m_files.size()) ? &m_files[m_index] : nullptr; }
    void UpdateTitle();

    // Vue
    ImagePtr Shown() const { return m_cur ? m_cur : m_placeholder; }
    bool LogicalSize(float& w, float& h) const;      // taille affichée (rotation incluse)
    void FlipRot(bool& flip, int& rot) const;
    double FitZoom() const;
    D2D1_RECT_F ViewArea() const;
    void ApplyFit(bool animate);
    void ZoomTo(double z, POINT anchor, bool animate);
    void ZoomStep(double factor, POINT anchor);
    void ToggleActualFit(POINT anchor);
    void ClampPan(double& px, double& py, double zoom) const;
    void Rotate(int quarter);
    void ShowToast(const std::wstring& s);
    void ToggleFullscreen();
    void Invalidate() { InvalidateRect(m_hwnd, nullptr, FALSE); }
    POINT ClientCenter() const;

    // Interaction
    Btn HitButton(POINT p) const;
    int HitStrip(POINT p) const;
    int HitNavZone(POINT p) const;  // -1 précédente, +1 suivante, 0 rien
    bool StripVisible() const;
    void OnMouseMove(POINT p, WPARAM wp);
    void OnButton(Btn b);
    void OnKey(WPARAM vk);
    void Poke();  // activité souris : réaffiche le HUD
    bool HudWanted() const;

    void LoadSettings();
    void SaveSettings();

    // Mises à jour GitHub (viewer_update.cpp)
public:
    struct UpdateResult {
        UpdateStatus status;
        ReleaseInfo release;
        std::wstring error;
        bool manual;
    };

private:
    void StartUpdateCheck(bool manual);
    void OnUpdateResult(UpdateResult* r);
    void ShowUpdateDialog();
    void InstallUpdateNow();

    // Enregistrer sous (viewer_edit.cpp)
    void SaveAs();
    void ReloadList();
    void ForgetFile(const std::wstring& path);

    // Mode édition (viewer_edit.cpp)
    bool BeginEdit();
    bool EndEdit(bool ask);  // false si l'utilisateur annule
    void DrawEditUi(float cw, float ch);
    void DrawEditOverlay(float left, float top, float z);
    void DrawToolIcon(Tool t, const D2D1_RECT_F& r, uint32_t col, float a);
    void IconLabel(const D2D1_RECT_F& r, const wchar_t* glyph, const std::wstring& label, uint32_t col, float a);
    void DrawShape(Tool t, D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t col, float size, bool fill);
    bool ImageRect(float& left, float& top, float& z) const;
    D2D1_POINT_2F ScreenToImage(POINT p) const;
    const EditButton* HitEdit(POINT p) const;
    bool EditMouseDown(POINT p, bool right, bool dbl);
    void EditMouseMove(POINT p);
    void EditMouseUp(POINT p);
    bool EditKey(WPARAM vk);
    bool EditChar(wchar_t c);
    void EditCommand(EditCmd cmd, int arg);
    void SetTool(Tool t);
    ComPtr<ID2D1Bitmap1> NewCanvas(uint32_t w, uint32_t h);
    bool DrawOn(ID2D1Bitmap1* target, const std::function<void()>& fn);
    bool ReadCanvas(PixelBuffer& out, uint32_t& stride);
    bool PushUndo();
    void ReplaceCanvas(ComPtr<ID2D1Bitmap1> c);
    void Undo();
    void Redo();
    void TransformCanvas(EditCmd kind);
    void ResizeCanvas(uint32_t w, uint32_t h);
    void ApplyCrop();
    void FloodFill(D2D1_POINT_2F p, uint32_t rgb);
    void PickColor(D2D1_POINT_2F p, bool secondary);
    void CommitText();
    void StrokeTo(D2D1_POINT_2F a, D2D1_POINT_2F b, bool dot);
    float& SliderValue();

    HWND m_hwnd = nullptr;
    HINSTANCE m_inst = nullptr;
    std::wstring m_exeDir, m_iniPath;
    Decoder m_decoder;
    std::unique_ptr<Loader> m_loader;

    ComPtr<ID2D1Factory1> m_factory;
    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<ID3D11Device> m_d3d;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<IDXGISwapChain1> m_swap;
    ComPtr<ID2D1Bitmap1> m_target;
    ComPtr<ID2D1DeviceContext> m_dc;
    ComPtr<ID2D1RenderTarget> m_rt;  // = m_dc, interface de dessin commune
    bool m_occluded = false;
    bool m_usingWarp = false;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<ID2D1BitmapBrush> m_checker;
    ComPtr<ID2D1LinearGradientBrush> m_shade;
    ComPtr<ID2D1PathGeometry> m_spinnerGeo;
    ComPtr<ID2D1StrokeStyle> m_roundStroke;
    ComPtr<IDWriteTextFormat> m_tfTitle, m_tfSub, m_tfSmall, m_tfCenter, m_tfLabel, m_tfToast, m_tfBig;
    ComPtr<IDWriteTextFormat> m_tfIcon, m_tfIconMid, m_tfIconBig;
    bool m_startMaximized = false;
    float m_scale = 1.0f;  // DPI / 96

    std::vector<FileEntry> m_files;
    std::unordered_map<std::wstring, int> m_pathIndex;
    std::wstring m_folder;
    int m_index = -1;
    int m_lastDir = 1;

    std::unordered_map<std::wstring, ImagePtr> m_fullCache;
    std::unordered_map<std::wstring, std::wstring> m_failed;
    std::unordered_map<std::wstring, ThumbEntry> m_thumbCache;
    std::vector<GpuImage> m_gpu;
    std::unordered_map<std::wstring, GpuThumb> m_gpuThumbs;
    size_t m_budget = 1ull << 30;
    uint64_t m_tick = 0;

    ImagePtr m_cur, m_placeholder;
    std::wstring m_error;
    bool m_loading = false;
    double m_loadStart = 0;

    double m_zoom = 1, m_zoomT = 1;
    double m_panX = 0, m_panY = 0, m_panXT = 0, m_panYT = 0;
    bool m_fit = true;
    int m_userRot = 0;

    bool m_fullscreen = false;
    WINDOWPLACEMENT m_wpPrev{sizeof(WINDOWPLACEMENT)};
    bool m_showStrip = true, m_showInfo = false, m_showHelp = false;
    float m_hud = 1.0f;
    double m_lastActivity = 0;
    POINT m_mouse{-1, -1};
    bool m_mouseInside = false;
    bool m_dragging = false;
    POINT m_dragStart{};
    double m_dragPanX = 0, m_dragPanY = 0;
    std::wstring m_toast;
    double m_toastTime = -10;
    double m_stripPos = 0, m_stripPosT = 0;
    int m_thumbReqLo = -1, m_thumbReqHi = -1, m_thumbReqIndex = -1;
    double m_lastFrame = 0;
    bool m_preuploadPending = false;
    int m_preuploadsLeft = 2;  // envois GPU anticipés autorisés depuis le dernier changement d'image
    std::vector<ButtonRect> m_buttons;

    EditState m_edit;
    std::vector<EditButton> m_editButtons;
    D2D1_RECT_F m_sliderRect{};
    bool m_gpuSwitchPending = false;
    float m_editRefLong = 0;

    ReleaseInfo m_release;
    bool m_updateAvailable = false, m_updateChecking = false;

    // Paramètres (panneau, enregistrés dans settings.ini)
    bool m_showSettings = false;
    bool m_singleInstance = false, m_autoUpdate = true;
    std::vector<SettingRow> m_settingRows;
    D2D1_RECT_F m_settingsPanel{};  // grand côté de la dernière image éditée (mise à l'échelle des tailles d'outil)
    std::vector<SaveFormat> m_saveFormats;
};
