/* CyImgView metadata providers, C ABI v1. Drop the DLL in plugins/.
 * UTF-16 strings, borrowed for the duration of a callback; the host copies them.
 * Calls to one provider are serialized. Do not retain host/context pointers.
 * read() must report every supported field, including duplicates and raw values.
 * write() receives a PRIVATE COPY, never the user's original. Preserve pixels and
 * unrelated metadata, validate all edits, and return failure for unsupported edits.
 * A provider may be read-only (write == NULL). No allocation crosses the ABI.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CYIMG_METADATA_API_VERSION 1
#define CYIMG_METADATA_ENTRY "CyImg_GetMetadataPlugin"
#define CYMETA_OK 0
#define CYMETA_UNSUPPORTED -2
#define CYMETA_FAILED -1
#define CYMETA_CANCELLED -3
#define CYMETA_EDITABLE 1u
#define CYMETA_BINARY 2u
#define CYMETA_STRUCTURAL 4u
#define CYMETA_SET 0u
#define CYMETA_DELETE 1u
#define CYMETA_ADD 2u

typedef struct CyImgMetadataField {
    uint32_t struct_size;
    const wchar_t *key, *label, *group, *type, *value, *description;
    uint32_t occurrence; /* zero based among fields with the same key */
    uint32_t flags;
} CyImgMetadataField;
typedef struct CyImgMetadataEdit {
    uint32_t struct_size;
    const wchar_t *key, *type, *value;
    uint32_t occurrence, operation;
} CyImgMetadataEdit;
typedef struct CyImgMetadataHost {
    uint32_t api_version, struct_size;
    /* Return zero to stop (resource limit/cancellation). */
    int (*field)(void* context, const CyImgMetadataField* field);
    /* A writable namespace, e.g. "Exif", "Iptc", "Xmp", "Comment", "ICC".
       Emit even if the document currently has no tags of that kind. */
    void (*writable_group)(void* context, const wchar_t* group);
    void (*error)(void* context, const wchar_t* message);
    int (*cancelled)(void* context);
} CyImgMetadataHost;
typedef struct CyImgMetadataPlugin {
    uint32_t api_version, struct_size;
    const wchar_t *id, *name, *version;
    int (*read)(const CyImgMetadataHost* host, void* context, const wchar_t* path);
    int (*write)(const CyImgMetadataHost* host, void* context, const wchar_t* private_copy,
                 const CyImgMetadataEdit* edits, size_t count);
} CyImgMetadataPlugin;
typedef const CyImgMetadataPlugin* (*CyImgGetMetadataPluginFn)(void);
#ifdef __cplusplus
}
#endif
