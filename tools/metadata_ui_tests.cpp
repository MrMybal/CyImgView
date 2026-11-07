#include "metadata.h"
#include "metadata_dialog.h"
#include "i18n.h"
#include "save.h"
#include <commctrl.h>
#include <wincodec.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace metadata_ui_tests {
using namespace metadata::controls;
namespace {
struct Probe {std::wstring path,bin,screenshot;bool edit=false,done=false,ok=true;int phase=0,ticks=0;DWORD thread=GetCurrentThreadId();};
Probe* active=nullptr;
std::wstring Get(HWND d,int id){HWND c=GetDlgItem(d,id);int n=GetWindowTextLengthW(c);std::wstring s(size_t(n)+1,0);GetWindowTextW(c,s.data(),n+1);s.resize(n);return s;}
void Require(Probe& p,bool pass,const char* name){printf("%s UI %s\n",pass?"PASS":"FAIL",name);if(!pass)p.ok=false;}
bool Capture(HWND dlg,const std::wstring& path) {
    RedrawWindow(dlg,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);GdiFlush();
    RECT r{};GetWindowRect(dlg,&r);const int w=r.right-r.left,h=r.bottom-r.top;
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;void* bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);auto old=SelectObject(dc,bitmap);
    bool ok=PrintWindow(dlg,dc,2)!=FALSE;
    if(ok){auto* p=static_cast<uint8_t*>(bits);for(int i=0;i<w*h;++i)p[i*4+3]=255;auto formats=EnumerateSaveFormats();std::wstring error;
        auto fmt=std::find_if(formats.begin(),formats.end(),[](auto& f){return f.container==GUID_ContainerFormatPng;});
        ok=fmt!=formats.end()&&SaveImage({path,0,92},*fmt,p,w,h,w*4,false,error);
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return ok;
}
BOOL CALLBACK Find(HWND window,LPARAM out){if(GetDlgItem(window,Search)&&GetDlgItem(window,List)){*reinterpret_cast<HWND*>(out)=window;return FALSE;}return TRUE;}
void CALLBACK Tick(HWND,UINT,UINT_PTR,DWORD) {
    if(!active)return;auto& p=*active;HWND d=nullptr;EnumThreadWindows(p.thread,Find,reinterpret_cast<LPARAM>(&d));
    if(!d)return;
    if(++p.ticks>250){Require(p,false,"dialog finishes asynchronous work");SendMessageW(d,WM_COMMAND,IDCANCEL,0);return;}
    if(!IsWindowEnabled(GetDlgItem(d,Search))||ListView_GetItemCount(GetDlgItem(d,List))<1)return;
    if(!p.edit&&p.phase==0) {
        SetDlgItemTextW(d,Search,L"Xmp.dc.title");p.phase=20;return;
    }
    if(p.phase==20) {
        SetDlgItemTextW(d,Search,L"");
        p.phase=21;return;
    }
    if(p.phase==21) {
        Require(p,Get(d,Key)==L"Xmp.dc.title","French reader selects full XMP field");
        Require(p,Capture(d,p.screenshot),"capture localized metadata workspace");p.done=true;SendMessageW(d,WM_COMMAND,IDCANCEL,0);return;
    }
    if(p.phase==0) {
        SetDlgItemTextW(d,Search,L"Exif.Image.Artist");
        Require(p,ListView_GetItemCount(GetDlgItem(d,List))==1,"search filters actual tag keys");
        Require(p,Get(d,Key)==L"Exif.Image.Artist","selection populates full value editor");
        SendDlgItemMessageW(d,Value,EM_SETSEL,0,-1);
        SendDlgItemMessageW(d,Value,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"UI saved author"));
        SendMessageW(d,WM_COMMAND,Apply,0);Require(p,IsWindowEnabled(GetDlgItem(d,Save))!=FALSE,"applying field stages a change");
        metadata::Engine e;e.Init(p.bin);auto doc=e.Read(p.path);bool unchanged=true;for(auto& f:doc.fields)if(f.key==L"Exif.Image.Artist")unchanged=f.value!=L"UI saved author";
        Require(p,unchanged,"staged field does not write source");p.phase=1;
        COMBOBOXINFO combo{sizeof(combo)};GetComboBoxInfo(GetDlgItem(d,Type),&combo);SetFocus(combo.hwndItem);
        BYTE oldKeys[256]{},keys[256]{};GetKeyboardState(oldKeys);memcpy(keys,oldKeys,sizeof(keys));keys[VK_CONTROL]=0x80;keys[VK_SHIFT]=0;SetKeyboardState(keys);
        SendMessageW(combo.hwndItem,WM_KEYDOWN,'S',0);SetKeyboardState(oldKeys);
        Require(p,IsWindowEnabled(GetDlgItem(d,Search))==FALSE,"Ctrl+S works inside the editable type combo");return;
    }
    if(p.phase==1) {
        Require(p,Get(d,Value)==L"UI saved author","save refreshes the persisted field");
        SendMessageW(d,WM_COMMAND,Remove,0);Require(p,IsWindowEnabled(GetDlgItem(d,Revert))!=FALSE,"deletion is reversible before save");
        SendMessageW(d,WM_COMMAND,Revert,0);Require(p,Get(d,Value)==L"UI saved author","revert restores selected metadata");
        SetDlgItemTextW(d,Search,L"");SendMessageW(d,WM_COMMAND,Add,0);
        int preset=int(SendDlgItemMessageW(d,Preset,CB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(L"Exif.GPSInfo.GPSAltitude")));
        SendDlgItemMessageW(d,Preset,CB_SETCURSEL,preset,0);SendMessageW(d,WM_COMMAND,MAKEWPARAM(Preset,CBN_SELCHANGE),0);
        Require(p,Get(d,Type)==L"Rational"&&Get(d,Value)==L"0/1","GPS altitude preset uses a single rational");
        preset=int(SendDlgItemMessageW(d,Preset,CB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(L"Exif.GPSInfo.GPSAltitudeRef")));
        SendDlgItemMessageW(d,Preset,CB_SETCURSEL,preset,0);SendMessageW(d,WM_COMMAND,MAKEWPARAM(Preset,CBN_SELCHANGE),0);
        Require(p,Get(d,Type)==L"Byte"&&Get(d,Value)==L"0","GPS altitude reference preset uses a byte");
        SetDlgItemTextW(d,Key,L"Xmp.dc.description");SetDlgItemTextW(d,Type,L"LangAlt");SetDlgItemTextW(d,Value,L"x-default=UI metadata test\nfr-FR=Test français");
        SendMessageW(d,WM_COMMAND,Apply,0);Require(p,Get(d,Key)==L"Xmp.dc.description","add a standard typed field");
        SendMessageW(d,WM_COMMAND,Reset,0);Require(p,IsWindowEnabled(GetDlgItem(d,Save))==FALSE,"reset clears pending changes");
        SendMessageW(d,WM_COMMAND,Add,0);ListView_SetItemState(GetDlgItem(d,List),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        Require(p,IsWindowEnabled(GetDlgItem(d,Save))==FALSE,"dismissing an untouched new field does not stage an empty tag");
        SetDlgItemTextW(d,Search,L"Xmp.dc.title");p.phase=2;return;
    }
    if(p.phase==2) {SetDlgItemTextW(d,Search,L"");p.phase=3;return;}
    if(p.phase==3) {Require(p,Capture(d,p.screenshot),"capture English metadata workspace");p.done=true;SendMessageW(d,WM_COMMAND,IDCANCEL,0);}
}
}
bool Run(const std::wstring& bin,const std::wstring& path,const std::wstring& output,bool edit) {
    Probe probe;probe.bin=bin;probe.path=path;probe.screenshot=output;probe.edit=edit;active=&probe;
    auto timer=SetTimer(nullptr,0,100,Tick);metadata::ShowDialog(nullptr,bin,path);KillTimer(nullptr,timer);active=nullptr;
    return probe.ok&&probe.done;
}
}
