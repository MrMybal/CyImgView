#pragma once
#include "common.h"
namespace metadata {
namespace controls {
enum Id {Search=600,Groups,List,Key,Type,Value,ProviderBox,Preset,Add,Apply,Remove,Revert,Reset,Save,SaveCopy,Export,Copy,Plugins,
    Title,Subtitle,Count,KeyLabel,TypeLabel,ValueLabel,ProviderLabel,Description,Status,Help};
}
// Opens a resizable metadata workspace. True if the source file was updated.
bool ShowDialog(HWND owner, const std::wstring& exeDir, const std::wstring& path,
                const std::vector<std::pair<std::wstring,std::wstring>>& decoderInfo = {});
}
