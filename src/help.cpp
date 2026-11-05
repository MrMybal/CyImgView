#include "help.h"
#include "i18n.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <vector>

namespace cyhelp {
namespace {
// CyDoc's C++17 route helper algorithm, MIT, commit 774f2a7. License shipped in help/.
std::string Encode(const std::string& value) {
    constexpr char hex[]="0123456789ABCDEF";
    std::string result;
    for (unsigned char c:value) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') result+=char(c);
        else { result+='%';result+=hex[c>>4];result+=hex[c&15]; }
    }
    return result;
}
std::wstring Index(const std::wstring& directory) { return directory+L"\\help\\index.html"; }
}
std::wstring Url(const std::wstring& directory,const std::wstring& locale,const Topic& topic) {
    const auto path=Index(directory);
    std::vector<wchar_t> buffer(32768);
    DWORD capacity=DWORD(buffer.size());
    HRESULT converted=UrlCreateFromPathW(path.c_str(),buffer.data(),&capacity,0);
    if (converted==E_POINTER && capacity>buffer.size()) {
        buffer.resize(size_t(capacity)+1);capacity=DWORD(buffer.size());
        converted=UrlCreateFromPathW(path.c_str(),buffer.data(),&capacity,0);
    }
    if (FAILED(converted)) return {};
    const wchar_t* locales[]={L"en",L"fr",L"de",L"es",L"it",L"pt",L"nl",L"pl",L"ru",L"ja",L"zh",L"ko"};
    const auto known=std::find_if(std::begin(locales),std::end(locales),[&](const wchar_t* item){return locale==item;});
    const std::wstring selected=known==std::end(locales)?L"en":locale;
    std::string code(selected.begin(),selected.end());
    std::string route="#/"+Encode(code)+"/"+Encode(topic.page.empty()?"overview":topic.page);
    if (!topic.section.empty()) route+="/"+Encode(topic.section);
    if (!topic.media.empty()) route+="?media="+Encode(topic.media);
    return std::wstring(buffer.data())+std::wstring(route.begin(),route.end());
}
bool Available(const std::wstring& directory) {
    const DWORD attributes=GetFileAttributesW(Index(directory).c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}
bool Open(HWND owner,const std::wstring& directory,const std::wstring& locale,const Topic& topic) {
    if (!Available(directory)) return false;
    const auto url=Url(directory,locale,topic);
    if (url.empty()) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32;
}
bool OpenForExecutable(HWND owner,const Topic& topic) {
    wchar_t path[32768]{};
    const DWORD length=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));
    if (!length || length>=std::size(path)) return false;
    std::wstring directory(path);directory.resize(directory.find_last_of(L"\\/"));
    return Open(owner,directory,i18n::CurrentCode(),topic);
}
}
