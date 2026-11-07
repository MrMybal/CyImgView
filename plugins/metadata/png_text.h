#pragma once
#include "cyimg_metadata_plugin.h"
#include <string>
#include <vector>
// Native PNG ancillary text. IDAT/fdAT and all unrelated chunks are copied verbatim.
bool ReadPngText(const CyImgMetadataHost*,void*,const wchar_t* path);
void WritePngText(const CyImgMetadataHost*,void*,const wchar_t* path,const std::vector<CyImgMetadataEdit>&);
inline bool IsPngText(const wchar_t* key){return wcsncmp(key,L"Png.Text.",9)==0;}
