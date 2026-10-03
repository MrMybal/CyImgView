// Modes du mode édition : Retouche (le mode historique, ses fonctions sont dans viewer_edit.cpp),
// aiguillage des entrées et du dessin vers le mode actif, changement de mode.

#include "viewer_modes.h"
#include "viewer_util.h"
#include "i18n.h"

#include <algorithm>

using namespace ui;

namespace {
constexpr int kRetouch = 0;
}

// Retouche : calques, outils, effets
class Viewer::RetouchMode final : public Viewer::EditMode {
public:
    using EditMode::EditMode;
    const char* NameKey() const override { return "develop.mode.retouch"; }
    const wchar_t* Glyph() const override { return L""; }
    void Leave() override { Commit(); }
    void Commit() override {
        if (v.m_edit.textActive) v.CommitText();
        if (v.m_edit.fx >= 0) v.CloseEffect(false);
        v.m_edit.cropValid = false;
        v.m_edit.menu = EditMenu::None;
    }
    void DrawOverlay(float left, float top, float z) override { v.DrawRetouchOverlay(left, top, z); }
    float DrawTopButtons(float right, float y, float size) override { return v.DrawRetouchTopButtons(right, y, size); }
    void DrawOptionsBar(float cw, float y, float h) override { v.DrawOptionsBar(cw, y, h); }
    void DrawSide(float cw, float ch) override { v.DrawRetouchSide(cw, ch); }
    void DrawMenus(float cw, float ch) override { v.DrawEditMenu(cw, ch); }
    D2D1_RECT_F ViewMargins() const override {
        const float s = v.m_scale;
        return D2D1::RectF(64 * s, 100 * s, (v.m_edit.showLayers || v.m_edit.fx >= 0) ? 256 * s : 12 * s, 74 * s);
    }
    bool MouseDown(POINT p, bool right, bool dbl) override { return v.RetouchMouseDown(p, right, dbl); }
    void MouseMove(POINT p) override { v.RetouchMouseMove(p); }
    void MouseUp(POINT p) override { v.RetouchMouseUp(p); }
    bool Key(WPARAM vk) override { return v.RetouchKey(vk); }
    bool Char(wchar_t c) override { return v.RetouchChar(c); }
    bool Wheel(POINT p, double notches) override { return v.RetouchWheel(p, notches); }
};

void Viewer::InitEditModes() {
    if (!m_modes.empty()) return;
    m_modes.push_back(std::make_shared<RetouchMode>(*this));
    m_modes.push_back(NewDevelopMode());
}

Viewer::EditMode& Viewer::Mode() const { return *m_modes[size_t(std::clamp(m_mode, 0, int(m_modes.size()) - 1))]; }

void Viewer::SwitchEditMode(int i) {
    if (!m_edit.active || i < 0 || i >= int(m_modes.size()) || i == m_mode) return;
    if (m_edit.drawing || m_edit.slider >= 0) return;  // glissement en cours
    Mode().Leave();
    const int old = m_mode;
    m_mode = i;
    if (!Mode().Enter()) {
        m_mode = old;
        Mode().Enter();
    }
    m_edit.menu = EditMenu::None;
    m_edit.spacePan = false;
    m_editHover.reset();
    if (m_fit) ApplyFit(false);
    else {
        ClampPan(m_panXT, m_panYT, m_zoomT);
        m_panX = m_panXT;
        m_panY = m_panYT;
    }
    Invalidate();
}

D2D1_RECT_F Viewer::EditViewArea(float cw, float ch) const {
    const D2D1_RECT_F m = Mode().ViewMargins();
    const float s = m_scale;
    return D2D1::RectF(m.left, m.top, std::max(m.left + s, cw - m.right), std::max(m.top + s, ch - m.bottom));
}

const wchar_t* Viewer::EditModeCursor() { return m_modes.empty() ? nullptr : Mode().Cursor(m_mouse); }

// =============================================================================================
// Aiguillage vers le mode actif

bool Viewer::EditMouseDown(POINT p, bool right, bool dbl) {
    if (const EditButton* b = HitEdit(p); b && b->cmd == EditCmd::ModeTab) {
        if (!right) SwitchEditMode(b->arg);
        return true;
    }
    return Mode().MouseDown(p, right, dbl);
}

void Viewer::EditMouseMove(POINT p) { Mode().MouseMove(p); }
void Viewer::EditMouseUp(POINT p) { Mode().MouseUp(p); }

bool Viewer::EditKey(WPARAM vk) {
    if (vk == VK_TAB && GetKeyState(VK_CONTROL) < 0 && !m_edit.textActive && m_edit.renaming < 0) {  // Ctrl+Tab : mode suivant
        SwitchEditMode((m_mode + (GetKeyState(VK_SHIFT) < 0 ? int(m_modes.size()) - 1 : 1)) % int(m_modes.size()));
        return true;
    }
    if (vk == 'O' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0 && !m_edit.textActive && m_edit.renaming < 0) {
        EditOpenWith();  // Ctrl+Maj+O : Ouvrir avec (avant l'outil Ellipse de la touche O)
        return true;
    }
    return Mode().Key(vk);
}

bool Viewer::EditChar(wchar_t c) { return Mode().Char(c); }
bool Viewer::EditWheel(POINT p, double notches) { return Mode().Wheel(p, notches); }
void Viewer::DrawEditOverlay(float left, float top, float z) { Mode().DrawOverlay(left, top, z); }

// Onglets des modes (barre du haut, à gauche) ; retourne le bord droit
float Viewer::DrawModeTabs(float x, float y, float h) {
    const float s = m_scale;
    for (int i = 0; i < int(m_modes.size()); ++i) {
        const std::wstring label = T(m_modes[size_t(i)]->NameKey());
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(m_dwrite->CreateTextLayout(label.c_str(), UINT32(label.size()), m_tfLabel.Get(), 1000, 100, &lay)))
            lay->GetMetrics(&tm);
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + tm.width + 52 * s, y + h);
        const bool active = i == m_mode;
        const bool hov = EditBtn(EditCmd::ModeTab, i, r, T(m_modes[size_t(i)]->NameKey()), active);
        if (active) {
            SetColor(kAccent, 1);
            m_rt->FillRectangle(D2D1::RectF(r.left + 8 * s, r.bottom - 2.5f * s, r.right - 8 * s, r.bottom), m_brush.Get());
        }
        IconLabel(r, m_modes[size_t(i)]->Glyph(), label, active ? kAccent : kWhite, active || hov ? 1.0f : 0.8f);
        x = r.right + 4 * s;
    }
    return x;
}
