---
id: metadata
title: Lire et modifier les métadonnées
description: Recherchez les champs EXIF, GPS, IPTC et XMP, modifiez leurs valeurs typées et enregistrez avec une sauvegarde.
section: Guides / Visionneuse
parent: viewer
order: 24
sourceRevision: 1
---

## Ouvrir l’espace de métadonnées {#metadata}

Ouvrez une image, puis utilisez **I** ou **ⓘ Informations**. **Ctrl+I** fonctionne aussi depuis l’éditeur d’image. Cet espace modifie le **fichier source sur le disque**. Les retouches, calques et effets non enregistrés sont indépendants ; exportez un nouveau document vierge avant de modifier ses métadonnées intégrées.

```image
{"id":"metadata-workspace","src":"assets/screenshots/metadata-fr.png","alt":"Éditeur natif de métadonnées avec recherche et titre XMP multilingue","title":"Espace de métadonnées","controls":"minimal"}
```

Le fournisseur natif **Exiv2** livré avec l’application lit EXIF, les notes propriétaires des appareils, GPS, IPTC et XMP, les commentaires et les profils ICC dans les formats qu’il prend en charge. Le groupe File présente les informations du système de fichiers. Windows Imaging Component ajoute les chemins de requête du conteneur et des images sous WIC, y compris des champs absents du fournisseur standard. Ces champs supplémentaires restent en lecture seule.

Le groupe PNG expose aussi les textes arbitraires `tEXt`, `zTXt` et `iTXt`, notamment les paramètres de génération d’image, la langue, le mot-clé traduit et la compression. Le groupe Decoder conserve les informations des anciens plugins de formats ; rouvrez l’image pour les actualiser après une modification sur disque.

## Rechercher et examiner les champs {#inspect}

La recherche porte sur les noms, les clés complètes, les groupes et les valeurs. Choisissez un groupe pour filtrer le tableau. Sélectionnez une ligne pour consulter sa valeur brute complète, son type, son fournisseur et ses possibilités de modification. Les tags répétés possèdent des occurrences distinctes : deux mots-clés IPTC ne sont pas regroupés en un seul champ. **Tout copier** et **Exporter en JSON** récupèrent les métadonnées courantes complètes, modifications en attente comprises ; l’export UTF-8 conserve clés, types, fournisseurs et occurrences.

La liste et l’éditeur de valeur défilent indépendamment. Tab passe d’un contrôle à l’autre, les flèches parcourent le tableau, **Ctrl+F** active la recherche et **Échap** ferme la fenêtre en demandant confirmation si des changements sont encore en attente. **F1** ouvre ce guide.

## Modifier, ajouter ou supprimer des métadonnées {#edit}

1. Sélectionnez une ligne **Modifiable** et changez sa valeur brute. **Appliquer** prépare la modification ; sélectionner une autre ligne prépare également la valeur modifiée.
2. **Ajouter** propose des clés standard pour le titre, la description, l’auteur, les droits, les dates, les mots-clés et le GPS. Choisissez un fournisseur ou saisissez une clé complète avancée qu’il accepte.
3. **Supprimer** prépare le retrait de l’occurrence sélectionnée. **Rétablir le champ** la restaure ; **Tout rétablir** abandonne tous les changements en attente.
4. **Enregistrer** / **Ctrl+S** applique le lot sur disque. **Enregistrer une copie** / **Ctrl+Maj+S** écrit un autre fichier du même format.

Aucune écriture n’a lieu pendant la saisie ou l’application d’un champ. Une clé ou une valeur typée invalide produit une erreur à l’enregistrement et laisse la source intacte. Une valeur vide et un champ supprimé sont deux opérations différentes. Pour renommer une clé, supprimez l’ancien champ et ajoutez le nouveau.

| Exemple de clé | Type | Exemple de valeur brute |
|---|---|---|
| `Png.Text.parameters/text` | `PNG-UTF8` | Texte multiligne stocké directement dans un bloc iTXt du PNG |
| `Exif.Image.Artist` | `Ascii` | `Alice` |
| `Exif.Photo.DateTimeOriginal` | `Ascii` | `2026:11:07 14:30:00` |
| `Iptc.Application2.Keywords` | `String` | `paysage` (ajoutez une autre occurrence pour un second mot-clé) |
| `Xmp.dc.title` | `LangAlt` | `x-default=Lake` et `fr-FR=Lac`, sur deux lignes |
| `Xmp.dc.subject` | `XmpBag` | Un mot-clé par ligne ; la virgule fait partie de l’élément |
| `Exif.GPSInfo.GPSLatitude` | `Rational` | `48/1 51/1 24/1` (degrés, minutes, secondes) |
| `Exif.GPSInfo.GPSLatitudeRef` | `Ascii` | `N` ou `S` |
| `Exif.GPSInfo.GPSLongitudeRef` | `Ascii` | `E` ou `W` |

Les coordonnées GPS utilisent des champs distincts pour latitude, longitude et hémisphère. Une altitude utilise une seule fraction et son champ de référence. Les tableaux XMP contiennent un élément échappé par ligne ; `LangAlt` utilise `langue=valeur` par ligne. Dans ces valeurs, `\n`, `\r` et `\\` représentent un saut de ligne, un retour chariot et une barre oblique inverse ; `\e` représente un élément vide. Les profils ICC sont exposés en octets hexadécimaux, sans outil de conception de profil colorimétrique. Les anciennes chaînes d’octets qui ne sont pas en UTF-8 utilisent `Hex:Ascii` ou `Hex:String` : aucun octet original n’est perdu par remplacement de caractères.

Pour les textes PNG, `/text` modifie ou supprime une occurrence du mot-clé. `/language` et `/translated` modifient les attributs iTXt ; `/compression` vaut `0` ou `1`. `PNG-Latin1` conserve l’encodage historique tEXt/zTXt ; `PNG-UTF8` utilise l’Unicode iTXt. Seuls les blocs texte modifiés sont reconstruits ; les blocs d’image compressée restent identiques octet pour octet.

## Enregistrer et récupérer l’original {#save}

L’enregistrement commence par copier la source dans un fichier de travail privé. Le fournisseur valide et écrit le lot ; celui livré avec l’application rouvre ensuite le résultat pour vérifier les valeurs demandées. L’application contrôle que la source n’a pas changé depuis sa lecture, puis la remplace de façon atomique et conserve **`fichier.ext.metadata-backup`**. Les sauvegardes suivantes sont numérotées. Pour retrouver un original déjà enregistré, conservez séparément le fichier modifié et restaurez la sauvegarde sous son nom d’origine. L’historique Annuler de l’éditeur d’image n’annule pas les écritures de métadonnées sur disque.

```diagram
{"type":"flow","title":"Transaction d’enregistrement des métadonnées","nodes":[{"id":"source","label":"Source + changements en attente","x":0,"y":20,"width":210},{"id":"copy","label":"Copie privée + validation","x":240,"y":20,"width":230},{"id":"commit","label":"Écriture atomique + sauvegarde","x":500,"y":20,"width":265}],"edges":[{"from":"source","to":"copy"},{"from":"copy","to":"commit"}]}
```

Enregistrer une copie conserve la source octet pour octet et garde les modifications en attente dans son espace de travail ; fermer puis abandonner ces changements ne supprime pas la copie enregistrée. L’écriture des métadonnées conserve les données d’image encodées et ne convertit pas le format. Les informations du système de fichiers et les champs décrivant le stockage des pixels, comme les dimensions, offsets et compressions, restent en lecture seule. Un format ou un tag propriétaire peut aussi être non modifiable ; l’éditeur présente les capacités réelles du fournisseur.

## Ajouter des plugins de métadonnées {#plugins}

**Plugins de métadonnées** présente les fournisseurs et l’emplacement du dossier local `plugins`. Ajoutez une DLL de fournisseur, puis fermez et rouvrez cet espace. Un fournisseur en lecture seule complète l’inspection ; un fournisseur doté d’un moteur d’écriture peut aussi ajouter l’édition de ses formats. Les plugins de décodage conservent leur API. Les formats non reconnus nécessitent un fournisseur de métadonnées adapté ; l’application ne bascule pas silencieusement vers un service externe et ne prétend pas qu’un fichier annexe modifie l’original.

Consultez [l’API des plugins de métadonnées](doc:metadata-api#metadata-api). L’interface, le moteur et le fournisseur livré fonctionnent entièrement en local, sans serveur ni connexion Internet.
