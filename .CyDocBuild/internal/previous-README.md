# Documentation CyImgView

Le même lecteur HTML est publié sur **GitHub Pages** :
[documentation française](https://mrmybal.github.io/CyImgView/#/fr/overview) ·
[English documentation](https://mrmybal.github.io/CyImgView/#/en/overview).
Les README ouvrent directement ces pages dans la bonne langue.

Le manuel destiné aux utilisateurs est produit par **CyDoc**, puis livré dans
`help/` à côté de `CyImgView.exe`. **F1**, le bouton d’aide et les menus ouvrent
la page adaptée au contexte dans le navigateur par défaut.

Pour lire le build local, ouvrir `build/Release/bin/help/index.html` directement.
Le lecteur utilise `file://` : aucun serveur, CDN, compte, Node.js ou accès réseau
n’est nécessaire à la lecture. Garder le dossier `help/` complet lors d’une copie.
Sur GitHub, les [sources françaises](manual/fr/overview.md) et
[anglaises](manual/en/overview.md) restent consultables ; les blocs interactifs
CyDoc et les schémas sont rendus dans le manuel exporté.

## Régénérer et vérifier

Depuis la racine du dépôt, avec **Node.js 22 ou supérieur** :

```powershell
npm --prefix tools/docs ci
node tools/build_docs.mjs --check
npm --prefix tools/docs test
python tools/check_lang.py
```

`--check` reconstruit, contrôle les liens CyDoc, les ressources, les traductions
et les routes natives, puis copie l’export dans `build/Release/bin/help`.
Sans cet argument, les entrées inchangées sont réutilisées après vérification
des ressources. `--force` force la reconstruction. Pour un autre build :

```powershell
node tools/build_docs.mjs --out build/Debug/bin/help
```

La sortie doit être un sous-dossier de `build/` ou `dist/`, hors du répertoire
interne `build/docs/`. Le script remplace entièrement ce dossier de sortie.

```powershell
./build.ps1
./tools/package.ps1 -NoBuild
python tools/check_package.py
```

CMake génère automatiquement l’aide pour les builds Release et Debug. Le script
de packaging vérifie à nouveau l’aide et copie tout l’export dans le ZIP et dans
la charge utile de l’installeur. La CI configure Node 22 et exécute les tests du
manuel avant publication. La version affichée vient de `src/version.h`.

## Organisation et contenu

Le workflow `.github/workflows/documentation.yml` régénère le manuel avec Node 22,
exécute ses tests et déploie uniquement `build/pages/` sur GitHub Pages à chaque
changement pertinent sur `main`. Il peut aussi être lancé manuellement dans
Actions. La source Pages du dépôt est **GitHub Actions**. Aucun dossier de données
locales, exécutable, dépendance npm ou source privée de l’application n’est téléversé.
Le lecteur embarqué reste identique et fonctionne toujours sans serveur ni réseau.

Pour reproduire cet export :

```powershell
node tools/build_docs.mjs --check --out build/pages
npm --prefix tools/docs test
```

- `docs/manual/en/`, `docs/manual/fr/` : guides, tutoriels et références.
- `docs/screenshots/` : captures réelles de la même application, dans les deux langues.
- `docs/assets/` : ressources propres au manuel.
- `cydoc.config.json` : langues, modules, alias et chemins de génération.
- `tools/build_docs.mjs` : préparation, index des sources, export et validation.
- `tools/docs/reader.*` : coque CyImgView et intégration de l’API publique CyDoc.
- `tools/docs/vendor/` : paquet CyDoc figé et provenance.
- `src/help.*`, `src/viewer_help.cpp` : URLs locales et routage contextuel.

Les modules **image** et **gallery** fournissent zoom, annotations, comparaison
couleur/noir et blanc et collection des espaces de travail. La galerie utilise
uniquement des enfants image. Les schémas `diagram` sont compilés en SVG statiques.
Il n’existe aucun média vidéo, GIF de démonstration, modèle 3D ou graphe à nœuds
dans ce manuel : leurs lecteurs ne sont pas inclus. Three.js n’est ni installé
dans l’outillage de l’application ni livré avec l’aide.

Le thème initial `nord` accompagne l’interface sombre ; le lecteur conserve le
choix ultérieur de thème. Agrandir un média reste dans la fenêtre. Recherche
locale, navigation clavier, gestes tactiles, retour à la position de lecture,
signets et notes sont fournis par le lecteur CyDoc. Leur persistance dépend de
l’autorisation de stockage local du navigateur et du chemin du manuel.

## Langues et adresses stables

L’anglais est la source et le repli. Le français est traduit intégralement.
L’aide clavier existante est conservée dans les **12 langues** de l’application ;
ces pages sont générées à partir des clés `viewer.help.*` de `src/lang/`.
Les guides détaillés des dix autres langues utilisent le repli anglais indiqué
par le lecteur. L’interface du lecteur CyDoc lui-même propose actuellement des
libellés français et anglais seulement.

Une page doit conserver son `id`, ses ancres explicites `{#id}` et les identifiants
de médias entre traductions. Les liens internes utilisent `doc:page#section`.
Les URLs distribuées ont la forme :

```text
file:///.../help/index.html#/fr/layers/masks
file:///.../help/index.html#/fr/new-document/templates?media=new-document-dialog
```

Incrémenter `revision` dans la page anglaise lorsqu’elle change de sens, puis
mettre à jour `sourceRevision` dans ses traductions après relecture. Le rapport
`translation-report.json` et sa page dans le manuel rendent visibles les manques
et retards. La génération échoue si le français est incomplet ou si une
traduction déclarée est en retard/non suivie. Les absences connues des dix autres
langues restent signalées, sans fausse traduction.

## Références du code et dépendance CyDoc

Le générateur copie une liste explicite de **9 fichiers publics** : les trois
en-têtes du SDK et six exemples d’effets, outils, modes, actions et HLSL. CyDoc
indexe leur texte, leurs includes et leurs symboles de façon lexicale ; ce n’est
pas un import sémantique Doxygen/Clang. Les sources privées de l’application,
les préférences utilisateur et `third_party/` ne sont pas indexées ou distribuées
par le manuel. Modifier la liste `sdk` après vérification de tout nouveau fichier.

Le paquet CyDoc 0.1.0 est fourni localement depuis le commit
`774f2a7eabf7dd181a98ffc5add115b580b67f42` du dépôt privé. Voir
[la provenance](../tools/docs/vendor/PROVENANCE.md). Les ressources et lecteurs
sont produits par l’exporteur officiel. L’adaptateur de l’application utilise
`mountViewer`, les scripts classiques exportés et le fournisseur de ressources
empaquetées ; il n’altère pas les bundles CyDoc. Les licences CyDoc MIT et
CyImgView GPL sont copiées dans l’export. Le dépôt privé n’est pas requis pour
compiler l’application avec cette dépendance figée.

## Vérifications et limites

Les tests Node contrôlent le contenu exporté, les routes, les alias, les replis,
les modules, les ressources locales et l’index limité au SDK. Des composants DOM
sans navigation ni téléchargement vérifient le lecteur réel, le clavier, le
changement de langue, les positions de lecture, la comparaison et la galerie.
Ces tests ne remplacent pas une vérification dans un navigateur réel.

Les tests natifs (`CYIMG_EDITOR_TESTS=ON`, puis
`build/Release/bin/cyimg_editor_tests.exe`) capturent le lanceur d’aide et
vérifient les routes F1, l’encodage des chemins et la conservation du document.
Ils ne lancent pas le navigateur.

**À vérifier manuellement sur Windows avant diffusion :** ouvrir le manuel par
`file://` hors ligne dans le navigateur par défaut, suivre F1 et le bouton `?`
depuis Retouche, Développement, Noir et blanc et Nouvelle image, puis vérifier
les images, le zoom, le tactile et le retour à la lecture. L’outil de navigation
de la session refuse les URL `file://` ; ce test n’a pas été exécuté et aucune
surface alternative n’a été utilisée pour contourner cette restriction.
CyImgView cible actuellement Windows x64 ; aucune autre plateforme native n’a
été validée.

L’inventaire de migration et les corrections de l’ancienne documentation sont
consignés dans [MIGRATION.md](MIGRATION.md).
