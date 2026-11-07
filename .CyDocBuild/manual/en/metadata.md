---
id: metadata
title: Read and edit metadata
description: Search EXIF, GPS, IPTC and XMP fields, edit typed values and save safely with a backup.
section: Guides / Viewing
parent: viewer
order: 24
revision: 1
---

## Open the metadata workspace {#metadata}

Open an image, then use **I** or **ⓘ Information**. **Ctrl+I** also works from the image editor. The workspace operates on the **source file on disk**. Unsaved paint, layers and effects are separate; export a new blank document before editing its embedded metadata.

```image
{"id":"metadata-workspace","src":"assets/screenshots/metadata-en.png","alt":"Native metadata editor with searchable fields and a multilingual XMP title","title":"Metadata workspace","controls":"minimal"}
```

The bundled native **Exiv2** provider reads EXIF, camera maker notes, GPS, IPTC and XMP, image comments and ICC profiles in the formats it supports. The File group shows filesystem information. Windows Imaging Component adds container/frame query paths under WIC, including fields outside the standard provider. It does not make those extra fields writable.

The PNG group also exposes arbitrary `tEXt`, `zTXt` and `iTXt` text, including image-generation parameters, language tags, translated keywords and compression. The Decoder group retains information supplied by older image-format plugins; reopen the image to refresh that legacy information after a disk edit.

## Find and inspect fields {#inspect}

Search matches names, complete keys, groups and values. Choose a group to narrow the table. Select a row to see the complete raw value, type, provider and editability. Repeated tags have distinct occurrence numbers, so two IPTC keywords are not collapsed into one field. Use **Copy all** or **Export JSON** for the complete current metadata, including staged changes; export is UTF-8 and keeps keys, types, providers and occurrences.

The field list and value editor scroll independently. Tab moves between controls, arrows navigate the field table, **Ctrl+F** focuses search, and **Escape** closes the workspace, asking before discarding pending changes. **F1** opens this guide.

## Modify, add or remove metadata {#edit}

1. Select a **Editable** row and change its raw value. **Apply field** stages the change; selecting another row also stages a modified value.
2. **Add field** offers standard keys for titles, descriptions, author, copyright, dates, keywords and GPS. Choose a provider, or enter a complete advanced key supported by it.
3. **Delete** stages removal of the selected occurrence. **Revert field** restores it, and **Reset changes** discards all pending edits.
4. **Save to file** / **Ctrl+S** applies the pending batch to disk. **Save a copy** / **Ctrl+Shift+S** writes another file of the same format.

No write occurs while typing or applying a field. Invalid keys or typed values produce an error at save time and leave the source intact. An empty value and a deleted field are different operations. To change a field key, remove the old field and add the new one.

| Example key | Type | Raw value example |
|---|---|---|
| `Png.Text.parameters/text` | `PNG-UTF8` | Multiline text stored directly in a PNG iTXt chunk |
| `Exif.Image.Artist` | `Ascii` | `Alice` |
| `Exif.Photo.DateTimeOriginal` | `Ascii` | `2026:11:07 14:30:00` |
| `Iptc.Application2.Keywords` | `String` | `landscape` (add another occurrence for another keyword) |
| `Xmp.dc.title` | `LangAlt` | `x-default=Lake` and `fr-FR=Lac`, on separate lines |
| `Xmp.dc.subject` | `XmpBag` | One keyword per line; a comma remains part of an item |
| `Exif.GPSInfo.GPSLatitude` | `Rational` | `48/1 51/1 24/1` (degrees, minutes, seconds) |
| `Exif.GPSInfo.GPSLatitudeRef` | `Ascii` | `N` or `S` |
| `Exif.GPSInfo.GPSLongitudeRef` | `Ascii` | `E` or `W` |

GPS coordinates use separate latitude/longitude and hemisphere fields. An altitude uses one fraction and its altitude-reference field. XMP arrays use one escaped item per line; `LangAlt` uses `language=value` per line. Within those values, `\n`, `\r` and `\\` represent a newline, carriage return and backslash; `\e` represents an empty item. ICC profiles use hexadecimal bytes, not a color-profile design tool. Legacy byte strings that are not valid UTF-8 use `Hex:Ascii` or `Hex:String`, so no original bytes are lost to replacement characters.

For PNG text, `/text` edits or deletes a keyword occurrence. `/language` and `/translated` edit iTXt attributes; `/compression` is `0` or `1`. `PNG-Latin1` preserves legacy tEXt/zTXt encoding; `PNG-UTF8` uses Unicode iTXt. Only the edited text chunks are rebuilt, and compressed image chunks remain byte-identical.

## Save and recover {#save}

Saving first copies the source to a private working file. The provider validates and writes the batch; the bundled provider reopens it to verify the requested values. The app checks that the source has not changed since reading, then atomically replaces it and retains **`filename.ext.metadata-backup`**. Later saves use numbered backup names. To recover a saved original, preserve the modified file separately and restore the backup under the original name. The image editor’s Undo history does not undo disk metadata writes.

```diagram
{"type":"flow","title":"Metadata save transaction","nodes":[{"id":"source","label":"Source + pending edits","x":0,"y":20,"width":210},{"id":"copy","label":"Private copy + validation","x":240,"y":20,"width":230},{"id":"commit","label":"Atomic save + original backup","x":500,"y":20,"width":265}],"edges":[{"from":"source","to":"copy"},{"from":"copy","to":"commit"}]}
```

Save a copy keeps the source byte-for-byte unchanged and retains the edits in the source workspace; closing and discarding those pending edits does not remove the saved copy. Metadata saving preserves encoded image data and does not convert formats. Filesystem information and pixel-storage fields such as dimensions, offsets and compression remain read-only. A format or proprietary tag may also be read-only; the editor displays the provider’s actual capability instead of pretending it can write it.

## Add metadata plugins {#plugins}

Use **Metadata plugins** to inspect providers and find the local `plugins` directory. Add a provider DLL there, then close and reopen the workspace. A read-only provider adds inspection; a provider with a writer can also add editing for its own formats. Decoding plugins keep their existing API. Unsupported formats require an appropriate metadata provider; the app never silently falls back to an external service or pretends a sidecar changed the original file.

See [the metadata plugin API](doc:metadata-api#metadata-api). The whole workspace, engine and bundled provider run locally without a server or Internet connection.
