# Migration CyDoc — 5 novembre 2026

## Périmètre vérifié

CyImgView est une application **C++20 native Windows x64**, compilée avec MSVC,
CMake et Ninja. Le rendu utilise Direct2D, DirectWrite et Direct3D 11. Les formats
sont fournis par WIC et des DLL ; l’édition utilise une API C publique et des
effets HLSL. Il n’y avait pas de site d’aide ni de documentation API générée.

Le contenu existant se trouvait dans les README anglais/français, le README HLSL,
les commentaires des en-têtes publics et l’aide dessinée dans l’application. Les
captures des sept espaces de travail dans les deux langues ont été conservées.
Les guides IA CyDoc `ai/README.md`, `ai/INTEGRATION.md`, `ai/AUTHORING.md` et
`ai/API.md` du commit figé ont été lus avant intégration. Le dépôt privé a été
obtenu par Git avec l’authentification disponible, sans identifiant embarqué.

## Inventaire des contenus préservés

| Origine | Destination commune EN/FR |
|---|---|
| Installation des README | `installation` |
| Formats, RAW, textures Unreal, fonctionnalités | `formats` |
| Compilation, sélection des plugins et bibliothèques | `building` |
| Mise à jour et publication GitHub | `updates` |
| Raccourcis visionneuse et édition | `keyboard-reference` |
| Paramètres | `settings` |
| API de décodage et exemple C++ | `decoder-api` |
| API d’édition et exemples effet/outil/mode/action | `editor-api` |
| Mesure des performances et commandes de benchmark | `performance` |
| Confidentialité et fichiers locaux | `privacy` |
| Licences et crédits | `licenses` |
| Notes récentes sur documents, calques, effets, masques et souris | `editing-reference` |
| `plugins/hlsl/README.md`, ses deux exemples et contrat complet | `hlsl` |
| Aide visionneuse intégrée, 24 lignes traduites | `shortcuts`, générée dans les 12 langues, avec F1 |
| Liste dynamique des décodeurs et plugins d’édition | Menu **Plugins installés**, conservé dans l’application |
| Commentaires et déclarations du SDK, exemples distribués | 9 pages source indexées par langue |
| Notes de la version 1.9.0 | `changelog`, notes originales conservées dans `docs/releases/` |

Les nouveaux parcours utilisateurs sont `overview`, `quickstart`, `viewer`,
`new-document`, `edit`, `layers`, `effects`, `develop`, `black-white`, `export`,
`troubleshooting`, `plugins` et `workspaces`. Ils expliquent les fonctions
présentes dans le code et utilisent des captures réelles annotées, une comparaison
et des schémas SVG. Aucun moteur IA ou service de plugins n’a été inventé.

Les longs chapitres techniques des README ont été remplacés par des liens vers
ces sources canoniques. Les anciens identifiants de section des README restent
sous forme d’ancres de redirection dans les fichiers GitHub. Le README HLSL
pointe vers le guide migré et son exemple compilable. Les alias CyDoc assurent
aussi la compatibilité des noms usuels `help`, `usage`, `utilisation`, `editing`,
`mode-edition`, `write-a-plugin`, `write-an-editing-plugin`, `compiler` et
`write-a-shader`.

## Informations corrigées ou précisées

- L’estimation historique de **600 Ko** n’est plus utilisée : la distribution
  contient maintenant les plugins et le lecteur d’aide complet.
- L’ancienne superposition d’aide et sa liste de plugins ont été remplacées par
  le manuel contextuel et une liste dynamique distincte. Les mentions « liste
  dans H » de l’API ont été corrigées.
- L’aide fonctionne aussi pendant la saisie de texte, un effet en cours et les
  modes de plugins, sans valider ni annuler le document.
- Le clic droit déplace la vue en édition ; **V** déplace les objets. Les guides
  expliquent la différence et la conversion explicite en pixels.
- Le nouveau document utilise des modèles, des dimensions et un fond. Les
  modèles d’impression sont des tailles en pixels, sans gestion physique du DPI.
- Noir et blanc propose Appliquer/Annuler ; « Original » est une comparaison.
  Développement applique en quittant le mode : utiliser Réinitialiser tout avant
  de sortir pour abandonner ses réglages.
- `.cyimg` préserve objets, calques et masques. Les exports PNG/JPEG sont aplatis ;
  PSD contient les calques pixellisés avec leur masque appliqué. Les métadonnées
  EXIF ne sont pas reprises à l’export.
- Distribuer uniquement l’exécutable et `plugins/` ne suffit plus : conserver
  aussi tout `help/`. Node est un outil de génération, pas un prérequis utilisateur.

## Modules et intégration

**image** couvre les captures, annotations, zoom et comparaison. **gallery**
réunit les captures des espaces de travail et charge seulement le lecteur image
sélectionné. Les schémas sont statiques : aucun module graph n’est nécessaire.
Sans vidéos, GIF ou scènes 3D, les modules video/gif/mesh sont exclus, ainsi que
Three.js. Les scripts classiques, données, feuilles de style, images, ressources
empaquetées et licences sont exportés localement. L’adaptateur CyImgView utilise
l’API publique du lecteur et les fournisseurs de ressources de CyDoc.

Les entrées suivantes utilisent des identifiants stables et la langue de l’app :

| Entrée | Cible |
|---|---|
| F1 / aide de fenêtre | Vue ou mode courant |
| H, bouton ? et menu Manuel utilisateur | Contexte de la visionneuse |
| ? dans les modes d’édition | Outil, texte, calques, masque, effet, Développement ou Noir et blanc |
| Menu des calques : aide | `layers/layers` |
| F1 dans Nouvelle image | `new-document/templates`, média `new-document-dialog` |

L’anglais est la source/repli et le français est complet. L’aide clavier traduite
reste disponible dans les douze langues. Les dix autres traductions détaillées
n’existaient pas : elles sont signalées comme manquantes dans le rapport CyDoc,
avec repli anglais. Le lecteur lui-même fournit des libellés EN/FR. Les ancres et
médias ont les mêmes identifiants entre traductions.

## Vérifications réalisées et à terminer

- Export CyDoc, liens internes, ancres, ressources et rapport de traduction.
- Tests Node de contenu, alias, routes natives, replis et liste des sources.
- Tests de composants DOM du lecteur réel : navigation, langue, position de
  lecture, thème, clavier, zoom, agrandissement dans la fenêtre, comparaison et
  changement d’image de galerie. Aucun navigateur ou accès réseau dans ces tests.
- Compilation Release et tests natifs de l’éditeur, dont les routes F1 capturées
  par un lanceur de test, les chemins contenant des caractères réservés et
  l’absence de modification du document/historique à l’ouverture de l’aide.
- Intégration de l’export aux builds CMake, au ZIP portable, à la charge utile
  de l’installeur et à la CI de release.

La vérification réelle `file://` dans un navigateur reste **manuelle** : l’outil
de navigation de cette session bloque ce protocole. Ce refus n’a pas été
contourné. Les tests DOM et l’inspection des ressources ne prouvent pas le rendu
final du navigateur ni son comportement de stockage local. Les interactions
tactiles doivent aussi être vérifiées sur un appareil adapté. Windows x64 est
la seule plateforme native disponible et visée par ce dépôt.

L’index CyDoc est lexical ; aucun export Doxygen/Clang n’a été supposé. Les
mesures historiques de temps de décodage restent indicatives et n’ont pas été
rebenchées pour cette migration. Il n’existe pas encore de démonstrations vidéo
ni de guides détaillés dans les dix autres langues.
