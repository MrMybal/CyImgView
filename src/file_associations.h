#pragma once

#include <string>

namespace cyimg::shell {
inline constexpr wchar_t kProjectProgId[] = L"CyImgView.Project";

// Per-user registration for native editing documents only. Never writes Explorer's UserChoice.
bool RegisterProjectAssociation(const std::wstring& executable);
void UnregisterProjectAssociation();
// Repair the registration after an in-app update, only for the installed copy.
void RefreshInstalledProjectAssociation(const std::wstring& executable);
}
