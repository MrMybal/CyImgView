---
id: metadata-api
title: Metadata provider API
description: Extend metadata reading and writing with a versioned native C ABI independent of image decoding.
section: API
parent: plugins
order: 205
revision: 1
---

## Metadata DLL contract {#metadata-api}

Include `cyimg_metadata_plugin.h` and export `CyImg_GetMetadataPlugin`. Place the x64 DLL in `plugins` beside the application. The descriptor uses `CYIMG_METADATA_API_VERSION` and `struct_size`, a unique stable provider ID, a human-readable name/version, a required `read` callback and an optional `write` callback. Image-decoder and editing ABIs are unchanged; one DLL may export more than one family’s entry point.

The host copies all field strings during the callback. Strings are UTF-16 and must remain valid until the callback returns. Do not retain the host or context pointers, pass C++ objects across the ABI, or free memory allocated by another module. Calls to a provider are serialized by its engine; different engine instances may execute concurrently. Providers must protect shared library state if necessary.

## Read fields and declare capabilities {#read}

`read(host, context, path)` examines the actual format and returns `CYMETA_UNSUPPORTED` when it does not apply. For each supported field, call `host->field` with its stable full key, label, namespace/group, raw type and complete editable representation, description, zero-based occurrence and flags. Emit repeated tags separately. Stop when `field` returns zero or `cancelled` is true. `field` and `writable_group` are read-time callbacks; `error` and `cancelled` also apply while writing.

| Flag | Meaning |
|---|---|
| `CYMETA_EDITABLE` | This exact field can be edited/deleted by the writer for this format |
| `CYMETA_BINARY` | The representation describes binary data |
| `CYMETA_STRUCTURAL` | Encoded-pixel layout or offsets; keep read-only |

Call `writable_group` for each namespace to which the current format can add fields, even when it contains no tags yet. A reader with no writer is always read-only. A failed read may be shown for inspection but cannot authorize writes. The host limits output to 50,000 fields, 4 Mi UTF-16 characters per value and 32 Mi characters per provider result; return an error instead of silently truncating.

```c
CyImgMetadataField field = {
    sizeof(CyImgMetadataField), L"Example.Author", L"Author", L"Example",
    L"String", L"Alice", L"", 0, CYMETA_EDITABLE
};
host->writable_group(context, L"Example");
if (!host->field(context, &field)) return CYMETA_CANCELLED;
```

## Write a batch without changing image pixels {#write}

`write` receives a **private working copy** and the entire batch for that provider. Never reopen the original or create a second output path. Validate every key, type, value and format capability. Operations are `CYMETA_SET`, `CYMETA_DELETE` and `CYMETA_ADD`; an empty string is not deletion. Occurrences in SET/DELETE refer to the original read snapshot, so process duplicate removals without renumbering later edits. The host rejects conflicting edits to the same occurrence.

Preserve encoded pixels and unrelated metadata, reopen the written file and verify the requested values, then return `CYMETA_OK`. Use `host->error` and a failure result for an unsupported key or incomplete write. The host commits only after every provider succeeds, checks the source fingerprint again, and retains a backup when replacing the original. Do not implement a silent sidecar fallback. Native access violations disable that provider for the current engine and discard its private copy; DLLs still run native code and are not a sandbox.

The bundled Exiv2 provider uses canonical Exiv2 keys and types. Arrays are lossless escaped lines, language alternatives are `language=value` lines, rational components are space-separated fractions and ICC data is hexadecimal. Keep these representations stable if implementing compatible tools. See [the human editing guide](doc:metadata#edit) for examples.

## Build and distribute a provider {#build}

Use an x64 compiler capable of exporting a C DLL. No Exiv2 dependency is required for another provider. Keep required dependencies and their licenses with the plugin; do not download runtimes while reading a user’s files. Reopen the metadata workspace after adding a DLL and inspect **Metadata plugins** to verify its name/version and read errors. Test Unicode Windows paths, missing/invalid fields, duplicates, unchanged pixel data, failed writes and files modified outside the app.
