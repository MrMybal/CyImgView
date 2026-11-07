---
id: metadata-api
title: API des fournisseurs de métadonnées
description: Étendez la lecture et l’écriture avec une ABI C native versionnée, indépendante du décodage des images.
section: API
parent: plugins
order: 205
sourceRevision: 1
---

## Contrat de la DLL de métadonnées {#metadata-api}

Incluez `cyimg_metadata_plugin.h` et exportez `CyImg_GetMetadataPlugin`. Placez la DLL x64 dans `plugins`, près de l’application. Le descripteur fournit `CYIMG_METADATA_API_VERSION` et `struct_size`, un identifiant stable unique, un nom et une version lisibles, un rappel `read` obligatoire et un rappel `write` facultatif. Les ABI des décodeurs et de l’édition ne changent pas ; une DLL peut exporter plusieurs familles de points d’entrée.

L’hôte copie les chaînes de chaque champ pendant le rappel. Elles sont en UTF-16 et doivent rester valides jusqu’à son retour. Ne conservez pas les pointeurs d’hôte ou de contexte, ne faites pas traverser l’ABI à des objets C++ et ne libérez pas la mémoire d’un autre module. Les appels à un fournisseur sont sérialisés par son moteur ; plusieurs instances de moteur peuvent travailler simultanément. Protégez l’état partagé des bibliothèques si nécessaire.

## Lire les champs et déclarer les capacités {#read}

`read(host, context, path)` examine le vrai format et renvoie `CYMETA_UNSUPPORTED` s’il ne s’applique pas. Pour chaque champ reconnu, appelez `host->field` avec sa clé complète stable, son libellé, son groupe, son type brut et sa représentation modifiable complète, une description, une occurrence à partir de zéro et ses indicateurs. Émettez les tags répétés séparément. Arrêtez-vous si `field` renvoie zéro ou si `cancelled` est vrai. `field` et `writable_group` concernent la lecture ; `error` et `cancelled` s’utilisent aussi pendant l’écriture.

| Indicateur | Signification |
|---|---|
| `CYMETA_EDITABLE` | Ce champ précis peut être modifié ou supprimé par le moteur d’écriture de ce format |
| `CYMETA_BINARY` | La représentation décrit des données binaires |
| `CYMETA_STRUCTURAL` | Structure des pixels encodés ou offsets ; conservez la lecture seule |

Appelez `writable_group` pour chaque espace de noms auquel ce format peut ajouter des champs, même s’il n’en contient pas encore. Un lecteur sans moteur d’écriture reste en lecture seule. Une lecture partiellement échouée peut servir à l’inspection, mais n’autorise pas l’écriture. L’hôte limite la sortie à 50 000 champs, 4 Mi caractères UTF-16 par valeur et 32 Mi caractères par résultat de fournisseur ; signalez une erreur au lieu de tronquer silencieusement.

```c
CyImgMetadataField field = {
    sizeof(CyImgMetadataField), L"Example.Author", L"Auteur", L"Example",
    L"String", L"Alice", L"", 0, CYMETA_EDITABLE
};
host->writable_group(context, L"Example");
if (!host->field(context, &field)) return CYMETA_CANCELLED;
```

## Écrire un lot sans modifier les pixels {#write}

`write` reçoit une **copie de travail privée** et le lot complet de son fournisseur. Ne rouvrez jamais l’original et ne créez pas un autre chemin de sortie. Validez chaque clé, type, valeur et capacité du format. Les opérations sont `CYMETA_SET`, `CYMETA_DELETE` et `CYMETA_ADD` ; une chaîne vide ne supprime pas le champ. Les occurrences de SET/DELETE visent l’état lu à l’origine : supprimer un doublon ne doit pas décaler les modifications suivantes. L’hôte refuse les changements contradictoires sur la même occurrence.

Conservez les pixels encodés et les autres métadonnées, rouvrez le résultat pour vérifier les valeurs demandées, puis renvoyez `CYMETA_OK`. Utilisez `host->error` et un échec pour les clés non acceptées ou les écritures incomplètes. L’hôte ne valide qu’après la réussite de tous les fournisseurs, vérifie de nouveau l’empreinte de la source et conserve une sauvegarde lors du remplacement de l’original. N’implémentez pas de repli silencieux vers un fichier annexe. Une violation d’accès native désactive le fournisseur pour ce moteur et provoque l’abandon de sa copie ; une DLL exécute néanmoins du code natif, sans bac à sable.

Le fournisseur Exiv2 livré utilise ses clés et types canoniques. Les tableaux sont des lignes échappées sans perte, les alternatives de langue des lignes `langue=valeur`, les composantes rationnelles des fractions séparées par des espaces et les données ICC de l’hexadécimal. Conservez ces représentations si vous créez des outils compatibles. Le [guide d’édition destiné aux utilisateurs](doc:metadata#edit) donne des exemples.

## Compiler et distribuer un fournisseur {#build}

Utilisez un compilateur x64 capable d’exporter une DLL C. Un autre fournisseur n’a pas besoin d’Exiv2. Distribuez ses dépendances et leurs licences avec le plugin ; ne téléchargez pas de moteur pendant la lecture des fichiers. Rouvrez l’espace de métadonnées après l’ajout d’une DLL et consultez **Plugins de métadonnées** pour vérifier son nom, sa version et ses éventuelles erreurs. Testez les chemins Windows Unicode, les champs absents ou invalides, les doublons, la conservation des pixels, les écritures échouées et les modifications externes du fichier.
