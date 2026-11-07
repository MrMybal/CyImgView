[English](README.md) · **Français** · [Documentation](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/overview) · [Site web](https://mrmybal.github.io/CyImgView/?lang=fr)

<p align="center">
  <img src="docs/logo.png" alt="Logo CyImgView" width="160">
</p>

<h1 align="center">CyImgView</h1>

Visionneuse d'images pour Windows, pensée pour la vitesse : C++ natif, Direct2D (rendu GPU), WIC,
aucun runtime externe pour l’exécutable principal. La distribution inclut les plugins de formats et un manuel complet hors ligne.

![CyImgView affichant un paysage, avec la barre d’outils et la pellicule du dossier](.CyDocBuild/assets/screenshots/viewer-fr.png)

## Modes d’édition et effets

Cliquez sur une capture pour l’ouvrir en grand.

<table>
  <tr>
    <th>Retouche et calques</th>
    <th>Développement photo</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/editor-fr.png"><img src=".CyDocBuild/assets/screenshots/editor-fr.png" alt="Retouche et calques" width="100%"></a>
      <p>Textes et formes modifiables, avec les commandes des calques.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/develop-fr.png"><img src=".CyDocBuild/assets/screenshots/develop-fr.png" alt="Développement photo" width="100%"></a>
      <p>Histogramme, balance des blancs, tonalité et couleurs.</p>
    </td>
  </tr>
  <tr>
    <th>Noir et blanc</th>
    <th>Galerie d’effets</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/black-white-fr.png"><img src=".CyDocBuild/assets/screenshots/black-white-fr.png" alt="Noir et blanc" width="100%"></a>
      <p>Préréglages, mélangeur de couleurs et contraste.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/effects-fr.png"><img src=".CyDocBuild/assets/screenshots/effects-fr.png" alt="Galerie d’effets" width="100%"></a>
      <p>Comparez les aperçus et choisissez le calque ou l’image entière.</p>
    </td>
  </tr>
  <tr>
    <th>Réglage d’un effet en direct</th>
    <th>Modèles de nouveaux documents</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/effect-preview-fr.png"><img src=".CyDocBuild/assets/screenshots/effect-preview-fr.png" alt="Réglage d’un effet en direct" width="100%"></a>
      <p>Ajustez l’effet avec un aperçu, puis appliquez ou annulez.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/new-document-fr.png"><img src=".CyDocBuild/assets/screenshots/new-document-fr.png" alt="Modèles de nouveaux documents" width="100%"></a>
      <p>Formats écran, photo et impression, avec dimensions personnalisées.</p>
    </td>
  </tr>
</table>

## Installation

Téléchargez l’installeur ou le ZIP portable depuis les [releases GitHub](https://github.com/MrMybal/CyImgView/releases/latest).
L’installation ne nécessite pas de droits administrateur. En version portable, gardez `plugins/` et **tout le dossier `Documentation/`** à côté de l’exécutable.
[Installation, intégration, mises à jour et désinstallation](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/installation).

## Manuel utilisateur

**[Documentation](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/overview)** — Consulter le manuel visuel complet en ligne.

Appuyez sur **F1** ou cliquez sur **?** dans l’application pour ouvrir l’aide du contexte courant.
Le manuel s’ouvre localement depuis `Documentation/Documentation.html`, avec captures, schémas, recherche et comparaisons d’images interactives.
Il fonctionne hors ligne, sans serveur. Le manuel est complet en français et en anglais. L’application conserve ses 12 langues d’interface ; l’aide s’ouvre en anglais pour les autres langues.

- [Bien démarrer](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/quickstart) · [Voir et parcourir les images](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/viewer)
- [Créer un document vierge](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/new-document) · [Peindre et modifier les objets](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/edit)
- [Calques, fusion et masques](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/layers)
- [Galerie d’effets](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/effects) · [Développement](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/develop) · [Noir et blanc](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/black-white)
- [Enregistrer et exporter](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/export) · [Raccourcis](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/keyboard-reference)
- [Paramètres](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/settings) · [Résoudre un problème](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/troubleshooting)

Ces liens ouvrent le lecteur HTML publié, avec navigation, recherche, images et schémas. Le même lecteur est également fourni hors ligne avec l’application.

## Compiler

Visual Studio 2022 avec le développement C++ Desktop, CMake/Ninja et **Node.js 22+** pour générer le manuel :

```powershell
npm --prefix .CyDocBuild ci
./build.ps1
```

L’application et son aide complète sont produites dans `build/Release/bin`.
[Compilation et distribution](.CyDocBuild/internal/previous-manual/fr/building.md) · [Outillage du manuel](.CyDocBuild/README.md).

## Références

Les guides utilisateur sont dans le manuel ; les notes de compilation et de mesure restent séparées. Les anciennes adresses de section restent accessibles ici.

<a id="fonctionnalités"></a>
- [Formats et fonctionnalités](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/formats)
<a id="mises-à-jour-github-releases"></a>
- [Mises à jour et releases GitHub](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/updates)
<a id="utilisation"></a>
- [Commandes de la visionneuse](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/keyboard-reference)
<a id="mode-édition"></a>
- [Commandes de l’éditeur](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/keyboard-reference) · [Référence des documents](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/editing-reference)
<a id="paramètres-touche-p-ou-bouton-engrenage"></a>
- [Paramètres](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/settings)
<a id="écrire-un-plugin"></a>
- [API et exemple de plugin de décodage](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/decoder-api)
<a id="écrire-un-plugin-dédition"></a>
- [API et exemples de plugins d’édition](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/editor-api) · [Effets HLSL](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/hlsl)
<a id="mesurer-les-performances"></a>
- [Mesurer les performances](.CyDocBuild/internal/previous-manual/fr/performance.md)
<a id="confidentialité"></a>
- [Confidentialité et fichiers locaux](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/privacy)
<a id="licence"></a>
- [Licences et crédits](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/licenses) · [GNU GPL v3.0](LICENSE)
