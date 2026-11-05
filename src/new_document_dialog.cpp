#include "new_document_dialog.h"
#include "i18n.h"
#include "help.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace newdoc {
namespace {
constexpr COLORREF bg=RGB(24,25,29), panel=RGB(30,32,37), card=RGB(37,40,46);
constexpr COLORREF ink=RGB(238,241,246), muted=RGB(156,166,180), accent=RGB(77,161,235);
struct Template { const char* key; unsigned w,h; };
constexpr Template templates[3][6]={
    {{"newdoc.hd",1920,1080},{"newdoc.4k",3840,2160},{"newdoc.square",1080,1080},
     {"newdoc.portrait",1080,1350},{"newdoc.story",1080,1920},{"newdoc.banner",2560,1440}},
    {{"newdoc.landscape",3000,2000},{"newdoc.portrait",2000,3000},{"newdoc.standard",2400,1800},
     {"newdoc.square",2400,2400},{"newdoc.panorama",3600,1200},{"newdoc.wallpaper",2560,1600}},
    {{"newdoc.a4",2480,3508},{"newdoc.a4wide",3508,2480},{"newdoc.a5",1748,2480},
     {"newdoc.a3",3508,4961},{"newdoc.letter",2550,3300},{"newdoc.postcard",1181,1748}}
};
struct State {
    Options options;
    int category=0, selected=0;
    bool updating=false, valid=true;
    HBRUSH brush=CreateSolidBrush(bg), editBrush=CreateSolidBrush(card), panelBrush=CreateSolidBrush(panel);
    HFONT font=nullptr, smallFont=nullptr, title=nullptr, strong=nullptr;
    ~State() { DeleteObject(brush); DeleteObject(editBrush); DeleteObject(panelBrush); DeleteObject(font); DeleteObject(smallFont); DeleteObject(title); DeleteObject(strong); }
};
RECT Units(HWND dlg,int x,int y,int w,int h) { RECT r{x,y,x+w,y+h}; MapDialogRect(dlg,&r); return r; }
void Fill(HDC dc,RECT r,COLORREF color) { HBRUSH b=CreateSolidBrush(color); FillRect(dc,&r,b); DeleteObject(b); }
void Round(HDC dc,RECT r,COLORREF color,COLORREF border,int radius) {
    HBRUSH b=CreateSolidBrush(color); HPEN p=CreatePen(PS_SOLID,1,border);
    auto oldB=SelectObject(dc,b); auto oldP=SelectObject(dc,p);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);
    SelectObject(dc,oldP); SelectObject(dc,oldB); DeleteObject(p); DeleteObject(b);
}
void Text(HDC dc,RECT r,const wchar_t* text,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
    auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
    DrawTextW(dc,text,-1,&r,flags|DT_NOPREFIX|DT_END_ELLIPSIS); SelectObject(dc,old);
}
std::wstring Dimensions(unsigned w,unsigned h) { return std::to_wstring(w)+L" × "+std::to_wstring(h)+L" px"; }
void Refresh(HWND dlg,State& s) {
    if(s.updating) return;
    BOOL okW=FALSE,okH=FALSE;
    unsigned w=GetDlgItemInt(dlg,Width,&okW,FALSE),h=GetDlgItemInt(dlg,Height,&okH,FALSE);
    s.valid=okW && okH && w && h && w<=s.options.maxDimension && h<=s.options.maxDimension && uint64_t(w)*h<=64000000;
    s.options.width=w; s.options.height=h;
    s.selected=-1;
    for(int i=0;i<6;++i) if(templates[s.category][i].w==w && templates[s.category][i].h==h) s.selected=i;
    const auto error=s.valid ? std::wstring() : Tf("newdoc.invalid",s.options.maxDimension);
    SetDlgItemTextW(dlg,Error,error.c_str());
    EnableWindow(GetDlgItem(dlg,IDOK),s.valid);
    const auto description=s.valid ? Dimensions(w,h) : std::wstring(L"—");
    SetDlgItemTextW(dlg,Summary,description.c_str());
    for(int i=0;i<6;++i) InvalidateRect(GetDlgItem(dlg,Preset+i),nullptr,FALSE);
    InvalidateRect(GetDlgItem(dlg,Preview),nullptr,FALSE);
}
void SetSize(HWND dlg,State& s,unsigned w,unsigned h) {
    s.updating=true; SetDlgItemInt(dlg,Width,w,FALSE); SetDlgItemInt(dlg,Height,h,FALSE); s.updating=false; Refresh(dlg,s);
}
void UpdateTemplates(HWND dlg,State& s) {
    for(int i=0;i<6;++i) {
        const auto& t=templates[s.category][i];
        auto name=std::wstring(T(t.key))+L" — "+Dimensions(t.w,t.h);
        SetDlgItemTextW(dlg,Preset+i,name.c_str());
    }
    for(int i=0;i<3;++i) InvalidateRect(GetDlgItem(dlg,Category+i),nullptr,FALSE);
    Refresh(dlg,s);
}
void DrawControl(HWND dlg,State& s,const DRAWITEMSTRUCT& d) {
    const auto id=d.CtlID; const RECT r=d.rcItem;
    const float k=Units(dlg,0,0,558,370).right/976.5f;
    const int scale=std::max(1,int(k));
    Fill(d.hDC,r,((id>=Background && id<Background+3) || id==Swap) ? panel : bg);
    const int pad=int(12*k);
    const bool focus=(d.itemState&ODS_FOCUS)!=0, pressed=(d.itemState&ODS_SELECTED)!=0;
    if(id==Preview) {
        Fill(d.hDC,r,panel);
        if(!s.valid) return;
        const float fit=std::min(float(r.right-2*pad)/s.options.width,float(r.bottom-2*pad)/s.options.height);
        const int w=std::max(2,int(s.options.width*fit)),h=std::max(2,int(s.options.height*fit));
        RECT paper{(r.right-w)/2,(r.bottom-h)/2,(r.right+w)/2,(r.bottom+h)/2};
        RECT shadow=paper; OffsetRect(&shadow,3*scale,3*scale); Fill(d.hDC,shadow,RGB(16,17,20));
        Fill(d.hDC,paper,s.options.background==2 ? RGB(12,12,14) : RGB(250,250,250));
        if(s.options.background==0) {
            const int cell=int(8*k);
            for(int y=paper.top;y<paper.bottom;y+=cell) for(int x=paper.left;x<paper.right;x+=cell)
                if(((x-paper.left)/cell+(y-paper.top)/cell)%2) Fill(d.hDC,{x,y,std::min<LONG>(x+cell,paper.right),std::min<LONG>(y+cell,paper.bottom)},RGB(197,202,210));
        }
        return;
    }
    bool selected=(id>=Category && id<Category+3 && int(id-Category)==s.category) ||
        (id>=Preset && id<Preset+6 && int(id-Preset)==s.selected) ||
        (id>=Background && id<Background+3 && int(id-Background)==s.options.background);
    const bool primary=id==IDOK, disabled=(d.itemState&ODS_DISABLED)!=0;
    COLORREF color=primary&&!disabled ? accent : selected ? RGB(38,61,83) : card;
    if(pressed) color=RGB(49,87,120);
    Round(d.hDC,r,color,focus||selected ? accent : color,int(9*k));
    if(id>=Preset && id<Preset+6) {
        const auto& t=templates[s.category][id-Preset];
        const int iw=int(34*k),ih=int(29*k); const float f=std::min(float(iw)/t.w,float(ih)/t.h);
        RECT icon{pad,pad,pad+std::max(3,int(t.w*f)),pad+std::max(3,int(t.h*f))};
        Round(d.hDC,icon,selected?RGB(50,80,108):RGB(49,53,62),selected?accent:muted,int(2*k));
        Text(d.hDC,{pad,r.bottom-int(50*k),r.right-pad,r.bottom-int(27*k)},T(t.key),s.strong,ink);
        Text(d.hDC,{pad,r.bottom-int(27*k),r.right-pad,r.bottom-int(7*k)},Dimensions(t.w,t.h).c_str(),s.smallFont,muted);
        if(selected) Text(d.hDC,{r.right-int(30*k),pad,r.right-pad,pad+int(20*k)},L"✓",s.strong,accent,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    } else {
        wchar_t label[128]{}; GetWindowTextW(d.hwndItem,label,128);
        Text(d.hDC,r,label,s.font,disabled?muted:ink,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    if(focus) { RECT f=r; InflateRect(&f,-4*scale,-4*scale); DrawFocusRect(d.hDC,&f); }
}
INT_PTR CALLBACK Proc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(dlg,DWLP_USER));
    switch(msg) {
    case WM_INITDIALOG: {
        s=reinterpret_cast<State*>(lp); SetWindowLongPtrW(dlg,DWLP_USER,lp);
        SetWindowTextW(dlg,T("newdoc.title"));
        BOOL dark=TRUE; DwmSetWindowAttribute(dlg,20,&dark,sizeof(dark));
        DwmSetWindowAttribute(dlg,35,&bg,sizeof(bg));
        const int dpi=int(std::lround(96*Units(dlg,0,0,558,370).right/976.5f));
        auto font=[&](int size,int weight) { return CreateFontW(-MulDiv(size,dpi,72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI"); };
        s->font=font(10,FW_NORMAL); s->smallFont=font(9,FW_NORMAL); s->title=font(20,FW_SEMIBOLD); s->strong=font(10,FW_SEMIBOLD);
        auto add=[&](const wchar_t* cls,const wchar_t* label,int id,int x,int y,int w,int h,DWORD style) {
            RECT r=Units(dlg,x,y,w,h);
            HWND c=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,r.left,r.top,r.right-r.left,r.bottom-r.top,dlg,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),FALSE);
            if(wcscmp(cls,L"EDIT")==0) { SetWindowTheme(c,L"DarkMode_CFD",nullptr); SendMessageW(c,EM_SETLIMITTEXT,5,0); SendMessageW(c,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(8,8)); }
            return c;
        };
        const char* categories[]={"newdoc.screen","newdoc.photo","newdoc.print"};
        for(int i=0;i<3;++i) add(L"BUTTON",T(categories[i]),Category+i,18+i*105,55,99,24,BS_OWNERDRAW|WS_TABSTOP);
        for(int i=0;i<6;++i) add(L"BUTTON",L"",Preset+i,18+(i%2)*158,90+(i/2)*68,150,60,BS_OWNERDRAW|WS_TABSTOP);
        add(L"STATIC",T("newdoc.width"),150,360,89,75,15,0);
        add(L"STATIC",T("newdoc.height"),151,463,89,75,15,0);
        add(L"EDIT",L"",Width,360,109,76,22,ES_NUMBER|ES_AUTOHSCROLL|WS_TABSTOP);
        add(L"BUTTON",L"⇄",Swap,440,108,20,24,BS_OWNERDRAW|WS_TABSTOP);
        add(L"EDIT",L"",Height,464,109,76,22,ES_NUMBER|ES_AUTOHSCROLL|WS_TABSTOP);
        add(L"STATIC",T("newdoc.background"),152,360,145,180,16,0);
        const char* backgrounds[]={"newdoc.transparent","newdoc.white","newdoc.black"};
        for(int i=0;i<3;++i) add(L"BUTTON",T(backgrounds[i]),Background+i,360+i*61,168,58,24,BS_OWNERDRAW|WS_TABSTOP);
        add(L"STATIC",L"",Preview,360,204,180,75,SS_OWNERDRAW);
        add(L"STATIC",L"",Summary,360,282,180,18,SS_CENTER);
        add(L"STATIC",L"",Error,18,308,522,16,0);
        add(L"BUTTON",T("openwith.dlg.cancel"),IDCANCEL,360,338,84,27,BS_OWNERDRAW|WS_TABSTOP);
        add(L"BUTTON",T("newdoc.create"),IDOK,452,338,88,27,BS_OWNERDRAW|WS_TABSTOP);
        SetSize(dlg,*s,s->options.width,s->options.height); UpdateTemplates(dlg,*s);
        SendMessageW(dlg,DM_SETDEFID,IDOK,0);
        // Center on the owner and keep the dialog on its monitor's work area.
        RECT owner{},rect{}; GetWindowRect(GetParent(dlg),&owner); GetWindowRect(dlg,&rect);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(dlg,MONITOR_DEFAULTTONEAREST),&monitor);
        const int width=rect.right-rect.left,height=rect.bottom-rect.top;
        int x=owner.left+(owner.right-owner.left-width)/2,y=owner.top+(owner.bottom-owner.top-height)/2;
        x=std::max<LONG>(monitor.rcWork.left,std::min<LONG>(x,monitor.rcWork.right-width)); y=std::max<LONG>(monitor.rcWork.top,std::min<LONG>(y,monitor.rcWork.bottom-height));
        SetWindowPos(dlg,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER);
        SetFocus(GetDlgItem(dlg,Preset)); return FALSE;
    }
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(dlg,&ps); RECT client; GetClientRect(dlg,&client); Fill(dc,client,bg);
        Fill(dc,Units(dlg,346,48,208,258),panel);
        Text(dc,Units(dlg,18,9,400,27),T("newdoc.title"),s->title,ink);
        Text(dc,Units(dlg,18,35,510,15),T("newdoc.subtitle"),s->smallFont,muted);
        Text(dc,Units(dlg,360,60,180,22),T("newdoc.details"),s->strong,ink);
        Round(dc,Units(dlg,358,106,80,28),card,card,8); Round(dc,Units(dlg,462,106,80,28),card,card,8);
        Fill(dc,Units(dlg,18,328,522,1),RGB(48,51,58));
        EndPaint(dlg,&ps); return TRUE;
    }
    case WM_DRAWITEM: if(s) { DrawControl(dlg,*s,*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; } break;
    case WM_CTLCOLORDLG: return s ? reinterpret_cast<INT_PTR>(s->brush) : FALSE;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: {
        if(!s) break; HDC dc=reinterpret_cast<HDC>(wp); SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,GetDlgCtrlID(reinterpret_cast<HWND>(lp))==Error ? RGB(255,159,143) : muted);
        if(msg==WM_CTLCOLOREDIT) { SetTextColor(dc,ink); SetBkColor(dc,card); return reinterpret_cast<INT_PTR>(s->editBrush); }
        const int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetBkColor(dc,id==Error?bg:panel);
        return reinterpret_cast<INT_PTR>(id==Error?s->brush:s->panelBrush);
    }
    case WM_COMMAND: {
        if(!s) break; int id=LOWORD(wp);
        if(id==IDCANCEL) { EndDialog(dlg,IDCANCEL); return TRUE; }
        if(id==IDOK) { Refresh(dlg,*s); if(s->valid) EndDialog(dlg,IDOK); return TRUE; }
        if((id==Width || id==Height) && HIWORD(wp)==EN_CHANGE) { Refresh(dlg,*s); return TRUE; }
        if(HIWORD(wp)!=BN_CLICKED) break;
        if(id>=Category && id<Category+3) { s->category=id-Category; UpdateTemplates(dlg,*s); return TRUE; }
        if(id>=Preset && id<Preset+6) { const auto& t=templates[s->category][id-Preset]; SetSize(dlg,*s,t.w,t.h); return TRUE; }
        if(id==Swap) { SetSize(dlg,*s,s->options.height,s->options.width); return TRUE; }
        if(id>=Background && id<Background+3) {
            s->options.background=id-Background;
            for(int i=0;i<3;++i) InvalidateRect(GetDlgItem(dlg,Background+i),nullptr,FALSE);
            InvalidateRect(GetDlgItem(dlg,Preview),nullptr,FALSE); return TRUE;
        }
        break;
    }
    case WM_HELP:
        if (!cyhelp::OpenForExecutable(dlg,{"new-document","templates","new-document-dialog"}))
            MessageBoxW(dlg,T("help.manual.unavailable"),T("help.manual.action"),MB_OK|MB_ICONINFORMATION);
        return TRUE;
    case WM_CLOSE: EndDialog(dlg,IDCANCEL); return TRUE;
    }
    return FALSE;
}
}
bool Show(HWND owner,Options& options) {
    std::vector<WORD> t;
    auto word=[&](WORD v){t.push_back(v);}; auto dword=[&](DWORD v){word(LOWORD(v));word(HIWORD(v));};
    auto str=[&](const wchar_t* s){while(*s) word(WORD(*s++));word(0);};
    dword(DS_SETFONT|DS_MODALFRAME|WS_POPUP|WS_CAPTION|WS_SYSMENU); dword(0);
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor);
    const float dpi=GetDpiForWindow(owner)/96.0f;
    const float available=std::min((monitor.rcWork.right-monitor.rcWork.left-32)/(980.0f*dpi),
        (monitor.rcWork.bottom-monitor.rcWork.top-32)/(760.0f*dpi));
    const WORD pointSize=WORD(std::clamp(int(std::floor(9*available)),6,9));
    word(0);word(0);word(0);word(558);word(378);word(0);word(0);str(L"");word(pointSize);str(L"Segoe UI");
    State state;state.options=options;
    if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE*>(t.data()),owner,Proc,reinterpret_cast<LPARAM>(&state))!=IDOK) return false;
    options=state.options; return true;
}
}
