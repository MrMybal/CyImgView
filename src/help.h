#pragma once
#include <windows.h>
#include <string>

namespace cyhelp {
struct Topic { std::string page = "overview", section, media; };
// The operating system converts the path; CyDoc's route component encoding is UTF-8.
std::wstring Url(const std::wstring& executableDirectory, const std::wstring& locale, const Topic& topic);
bool Available(const std::wstring& executableDirectory);
bool Open(HWND owner, const std::wstring& executableDirectory, const std::wstring& locale, const Topic& topic);
bool OpenForExecutable(HWND owner, const Topic& topic);
}
