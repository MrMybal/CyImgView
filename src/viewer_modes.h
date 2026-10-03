#pragma once

// Modes du mode édition (onglets de la barre du haut) : Retouche (calques et outils, viewer_edit.cpp),
// Développement (réglages photo, viewer_develop.cpp), modes des plugins d'édition (viewer_editplugins.cpp).
//
// Le Viewer garde ce qui est commun : document (calques, historique), barre du haut (onglets,
// Enregistrer sous, Terminer), zoom et déplacement de la vue, info-bulles. Tout le reste passe par le
// mode actif : barre d'options, panneaux, dessin du document et de ses surcouches, souris, clavier.
// Un mode n'a pas à se soucier des autres : en le quittant, Leave() intègre son travail au document
// (avec une entrée d'historique), et le suivant repart du document.

#include "viewer.h"

class Viewer::EditMode {
public:
    explicit EditMode(Viewer& viewer) : v(viewer) {}
    virtual ~EditMode() = default;
    EditMode(const EditMode&) = delete;
    EditMode& operator=(const EditMode&) = delete;

    virtual const char* NameKey() const = 0;   // clé de traduction du nom de l'onglet
    virtual const wchar_t* Name() const;       // nom affiché, valable jusqu'à la fin (par défaut : T(NameKey()))
    virtual const wchar_t* Glyph() const = 0;  // icône (Segoe MDL2) de l'onglet

    // Cycle de vie
    virtual bool Enter() { return true; }        // false : mode indisponible (message déjà affiché)
    virtual void Leave() {}                      // le mode est quitté : son travail est intégré au document
    virtual void Commit() {}                     // Terminer : idem, sans quitter le mode
    virtual void Close() {}                      // fin de l'édition : ressources libérées
    virtual bool BeginExport() { return false; } // Enregistrer / copier : document complété temporairement
    virtual void EndExport() {}
    virtual void DropGpu() {}                    // perte du device : textures perdues

    // Rendu (Prepare : hors BeginDraw, avant chaque image)
    virtual void Prepare() {}
    virtual ID2D1Bitmap1* LayerBitmap(int /*layer*/) const { return nullptr; }  // pixels affichés à la place du calque
    virtual bool DrawLayer(int /*layer*/, const D2D1_RECT_F& /*r*/, D2D1_INTERPOLATION_MODE /*mode*/, float /*opacity*/) {
        return false;  // false : dessin normal du calque
    }
    virtual bool DocTransform(const D2D1_RECT_F& /*r*/, D2D1_MATRIX_3X2_F& /*m*/) const { return false; }
    virtual void DrawOverlay(float left, float top, float z) = 0;
    virtual float DrawTopButtons(float right, float y, float size) = 0;  // boutons à gauche de right ; retourne le bord gauche
    virtual void DrawOptionsBar(float cw, float y, float h) = 0;
    virtual void DrawSide(float cw, float ch) = 0;                       // colonnes et panneaux
    virtual void DrawMenus(float /*cw*/, float /*ch*/) {}
    virtual D2D1_RECT_F ViewMargins() const = 0;  // marges de la zone d'affichage (gauche, haut, droite, bas)

    // Entrées (MouseDown : false = la vue est déplacée)
    virtual bool MouseDown(POINT p, bool right, bool dbl) = 0;
    virtual void MouseMove(POINT p) = 0;
    virtual void MouseUp(POINT p) = 0;
    virtual bool Key(WPARAM vk) = 0;
    virtual bool Char(wchar_t c) = 0;
    virtual bool Wheel(POINT p, double notches) = 0;
    virtual const wchar_t* Cursor(POINT /*p*/) const { return nullptr; }  // nullptr : curseurs de la Retouche

protected:
    Viewer& v;
};
