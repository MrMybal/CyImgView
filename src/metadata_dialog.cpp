#include "metadata_dialog.h"
#include "metadata.h"
#include "i18n.h"
#include "help.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <set>

namespace metadata {
namespace {
constexpr COLORREF bg=RGB(22,24,28),card=RGB(31,34,39),ink=RGB(233,239,241),muted=RGB(155,171,178),accent=RGB(42,178,169);
using namespace controls;
struct SaveResult {bool ok=false;std::wstring backup,error,destination;};
struct State {
    Engine engine;Document doc;std::vector<Edit> edits;std::vector<Field> rows;
    std::vector<std::pair<std::wstring,std::wstring>> decoderInfo;
    std::vector<size_t> visible;std::vector<size_t> writers;
    std::wstring exeDir,path,notice;std::atomic<bool> cancel=false;
    std::future<Document> reading;std::future<SaveResult> saving;
    bool busy=true,updating=false,adding=false,formDirty=false,changed=false,wantClose=false;
    int selected=-1;float scale=1;HFONT font=nullptr,titleFont=nullptr,mono=nullptr;
    HBRUSH background=CreateSolidBrush(bg),editBrush=CreateSolidBrush(card);
    ~State(){cancel=true;if(reading.valid())reading.wait();if(saving.valid())saving.wait();DeleteObject(font);DeleteObject(titleFont);DeleteObject(mono);DeleteObject(background);DeleteObject(editBrush);}
};
HWND C(HWND d,int id){return GetDlgItem(d,id);}
std::wstring Get(HWND d,int id){HWND c=C(d,id);int n=GetWindowTextLengthW(c);std::wstring s(size_t(n)+1,0);GetWindowTextW(c,s.data(),n+1);s.resize(n);return s;}
void Set(HWND d,int id,const std::wstring& s){
    std::wstring text;for(size_t i=0;i<s.size();++i){if(s[i]=='\n'&&(i==0||s[i-1]!='\r'))text+='\r';text+=s[i];}
    SetWindowTextW(C(d,id),text.c_str());
}
void Enable(HWND d,int id,bool yes){EnableWindow(C(d,id),yes);}
void Combo(HWND d,int id,const std::wstring& s){SendMessageW(C(d,id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s.c_str()));}
int Index(HWND d,int id){return int(SendMessageW(C(d,id),CB_GETCURSEL,0,0));}
bool Same(const Field& f,const Edit& e){return f.provider==e.provider&&f.key==e.key&&f.occurrence==e.occurrence;}
auto Pending(State& s,const Field& f){return std::find_if(s.edits.begin(),s.edits.end(),[&](const Edit& e){return Same(f,e);});}
void Error(HWND d,const std::wstring& text){MessageBoxW(d,text.c_str(),T("metadata.title"),MB_OK|MB_ICONERROR);}
void Layout(HWND d,State& s) {
    RECT rc{};GetClientRect(d,&rc);const int w=int(rc.right/s.scale),h=int(rc.bottom/s.scale),left=w*53/100,gap=18,right=left+gap,rw=w-right-22;
    auto place=[&](int id,int x,int y,int a,int b){MoveWindow(C(d,id),int(x*s.scale),int(y*s.scale),int(a*s.scale),int(b*s.scale),TRUE);};
    place(Title,22,16,w-300,34);place(Plugins,w-252,21,230,28);place(Subtitle,22,53,w-44,24);
    place(Search,22,92,left-208,30);place(Groups,left-174,92,174,260);
    place(List,22,133,left-22,h-267);place(Count,22,h-127,left-22,23);
    place(Add,22,h-97,125,30);place(Copy,155,h-97,126,30);place(Export,289,h-97,140,30);
    place(ProviderLabel,right,93,88,24);place(ProviderBox,right+92,90,rw-92,240);
    place(Preset,right,126,rw,260);place(KeyLabel,right,164,rw,19);place(Key,right,187,rw,28);
    place(TypeLabel,right,224,rw,19);place(Type,right,247,rw,240);place(ValueLabel,right,286,rw,19);
    place(Value,right,310,rw,std::max(76,h-496));place(Description,right,h-178,rw,42);
    place(Apply,right,h-126,128,30);place(Remove,right+136,h-126,106,30);place(Revert,right+250,h-126,rw-250,30);
    place(Status,22,h-55,std::max(200,w-660),40);place(Help,w-620,h-48,38,30);place(Reset,w-573,h-48,124,30);
    place(SaveCopy,w-440,h-48,168,30);place(Save,w-263,h-48,137,30);place(IDCANCEL,w-117,h-48,95,30);
    const int lw=int((left-22)*s.scale);ListView_SetColumnWidth(C(d,List),0,lw*48/100);ListView_SetColumnWidth(C(d,List),1,lw*29/100);ListView_SetColumnWidth(C(d,List),2,lw*20/100);
}
void StatusText(HWND d,State& s) {
    Set(d,Status,s.busy?T("metadata.working"):s.notice.empty()?Tf("metadata.pending",int(s.edits.size())):s.notice);
    for(int id:{Save,SaveCopy,Reset})Enable(d,id,!s.busy&&(!s.edits.empty()||s.formDirty));
    Enable(d,Add,!s.busy&&!s.writers.empty());
    Enable(d,Copy,!s.busy&&!s.doc.fields.empty());Enable(d,Export,!s.busy&&!s.doc.fields.empty());
}
void BuildRows(State& s) {
    s.rows=s.doc.fields;
    for(auto& e:s.edits)if(e.operation==CYMETA_ADD)s.rows.push_back({e.provider,e.key,e.key,e.key.substr(0,e.key.find('.')),e.type,e.value,L"",e.occurrence,CYMETA_EDITABLE});
}
bool Queue(HWND d,State& s) {
    if(s.updating||s.busy||!s.formDirty)return true;
    auto value=Get(d,Value),type=Get(d,Type);
    if(type==L"LangAlt"||type==L"XmpBag"||type==L"XmpSeq"||type==L"XmpAlt")
        value.erase(std::remove(value.begin(),value.end(),'\r'),value.end());
    if(s.adding) {
        int writer=Index(d,ProviderBox);auto key=Get(d,Key);
        if(writer<0||size_t(writer)>=s.writers.size()||key.empty()||type.empty()){Error(d,T("metadata.required"));return false;}
        const auto& provider=s.doc.providers[s.writers[writer]];
        uint32_t occurrence=0;for(auto& f:s.rows)if(f.key==key&&f.provider==provider.id)occurrence=std::max(occurrence,f.occurrence+1);
        s.edits.push_back({provider.id,key,type,value,occurrence,CYMETA_ADD});s.adding=false;
    } else if(s.selected>=0&&size_t(s.selected)<s.rows.size()) {
        auto f=s.rows[s.selected];if(!f.Editable())return true;
        auto pending=Pending(s,f);
        if(pending!=s.edits.end()&&pending->operation==CYMETA_DELETE)return true;
        if(pending!=s.edits.end()&&pending->operation==CYMETA_ADD){pending->value=value;pending->type=type;}
        else {
            if(pending!=s.edits.end())s.edits.erase(pending);
            if(type!=f.type||value!=f.value)s.edits.push_back({f.provider,f.key,type,value,f.occurrence,CYMETA_SET});
        }
    }
    s.formDirty=false;s.notice.clear();StatusText(d,s);return true;
}
void FillForm(HWND d,State& s,int row) {
    s.updating=true;s.selected=row;s.adding=false;s.formDirty=false;
    const bool has=row>=0&&size_t(row)<s.rows.size();
    SendMessageW(C(d,Preset),CB_SETCURSEL,0,0);
    Enable(d,ProviderBox,false);Enable(d,Preset,false);SendMessageW(C(d,Key),EM_SETREADONLY,TRUE,0);
    bool writable=false,removed=false;
    if(has) {
        auto& f=s.rows[row];auto e=Pending(s,f);removed=e!=s.edits.end()&&e->operation==CYMETA_DELETE;
        writable=s.doc.valid&&f.Editable()&&!removed;
        Set(d,Key,f.key);Set(d,Type,e!=s.edits.end()?e->type:f.type);Set(d,Value,e!=s.edits.end()?e->value:f.value);
        Set(d,ProviderBox,f.provider);
        std::wstring details=f.group+L" · "+(removed?T("metadata.deleted"):f.Editable()?T("metadata.editable"):T("metadata.read_only"));
        if(f.occurrence)details+=L" · #"+std::to_wstring(f.occurrence+1);
        if(!f.description.empty())details+=L"\n"+f.description;
        if(f.flags&CYMETA_STRUCTURAL)details+=L"\n"+std::wstring(T("metadata.structural"));
        Set(d,Description,details);
    } else {
        for(int id:{Key,Type,Value,ProviderBox})Set(d,id,L"");Set(d,Description,T("metadata.select"));
    }
    SendMessageW(C(d,Value),EM_SETREADONLY,!writable,0);
    Enable(d,Type,writable);Enable(d,Apply,writable);Enable(d,Remove,has&&s.rows[row].Editable()&&!removed);
    Enable(d,Revert,has&&Pending(s,s.rows[row])!=s.edits.end());
    s.updating=false;StatusText(d,s);
}
void Filter(HWND d,State& s,const std::wstring& selectKey=L"",const std::wstring& selectProvider=L"",uint32_t occurrence=0) {
    auto wantedKey=selectKey,wantedProvider=selectProvider;auto wantedOccurrence=occurrence;
    if(wantedKey.empty()&&s.selected>=0&&size_t(s.selected)<s.rows.size()) {
        const auto& f=s.rows[s.selected];wantedKey=f.key;wantedProvider=f.provider;wantedOccurrence=f.occurrence;
    }
    s.updating=true;BuildRows(s);s.visible.clear();ListView_DeleteAllItems(C(d,List));
    auto query=ToLower(Get(d,Search));auto group=Get(d,Groups);int selected=-1;
    for(size_t i=0;i<s.rows.size();++i) {
        auto& f=s.rows[i];auto edit=Pending(s,f);auto value=edit==s.edits.end()?f.value:edit->value;
        if(Index(d,Groups)>0&&group!=f.group)continue;
        if(!query.empty()&&ToLower(f.key+L" "+f.label+L" "+f.group+L" "+value).find(query)==std::wstring::npos)continue;
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(s.visible.size());
        std::wstring label=f.label.empty()?f.key:f.label;item.pszText=label.data();ListView_InsertItem(C(d,List),&item);
        ListView_SetItemText(C(d,List),item.iItem,1,f.group.data());
        auto status=edit!=s.edits.end()?T(edit->operation==CYMETA_DELETE?"metadata.deleted":edit->operation==CYMETA_ADD?"metadata.added":"metadata.modified"):T(f.Editable()?"metadata.editable":"metadata.read_only");
        ListView_SetItemText(C(d,List),item.iItem,2,const_cast<wchar_t*>(status));
        if(f.key==wantedKey&&f.provider==wantedProvider&&f.occurrence==wantedOccurrence)selected=item.iItem;
        s.visible.push_back(i);
    }
    Set(d,Count,Tf("metadata.count",int(s.visible.size()),int(s.rows.size()),int(s.edits.size())));
    if(selected<0&&!s.visible.empty())selected=0;
    if(selected>=0){ListView_SetItemState(C(d,List),selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);ListView_EnsureVisible(C(d,List),selected,FALSE);}
    s.updating=false;FillForm(d,s,selected>=0?int(s.visible[selected]):-1);
}
void Refresh(HWND d,State& s) {
    if(!s.busy)for(int id:{Search,Groups,List,Key,Value})Enable(d,id,true);
    s.updating=true;SendMessageW(C(d,Groups),CB_RESETCONTENT,0,0);Combo(d,Groups,T("metadata.all_groups"));
    std::set<std::wstring> groups;for(auto& f:s.doc.fields)groups.insert(f.group);
    for(auto& group:groups)Combo(d,Groups,group);SendMessageW(C(d,Groups),CB_SETCURSEL,0,0);
    s.writers.clear();SendMessageW(C(d,ProviderBox),CB_RESETCONTENT,0,0);
    for(size_t i=0;i<s.doc.providers.size();++i)if(s.doc.valid&&!s.doc.providers[i].writableGroups.empty()) {
        s.writers.push_back(i);Combo(d,ProviderBox,s.doc.providers[i].name);
    }
    if(s.writers.empty()&&s.notice.empty())s.notice=T("metadata.no_reader");
    s.updating=false;Filter(d,s);StatusText(d,s);
}
void ReadAsync(HWND d,State& s,bool initialize) {
    s.busy=true;s.cancel=false;s.formDirty=false;
    for(int id:{Search,Groups,List,Key,Type,Value,Preset,ProviderBox,Apply,Remove,Revert})Enable(d,id,false);
    s.reading=std::async(std::launch::async,[&s,initialize] {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(initialize)s.engine.Init(s.exeDir);
        auto doc=s.engine.Read(s.path,&s.cancel);
        for(size_t i=0;i<s.decoderInfo.size();++i)doc.fields.push_back({L"decoder",L"Decoder.Info."+std::to_wstring(i),s.decoderInfo[i].first,L"Decoder",L"String",s.decoderInfo[i].second,T("metadata.decoder_hint"),0,0});
        CoUninitialize();return doc;
    });StatusText(d,s);
}
bool ChoosePath(HWND d,const wchar_t* filter,std::wstring& path,const wchar_t* extension) {
    wchar_t file[32768]{};wcsncpy_s(file,path.c_str(),_TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};ofn.hwndOwner=d;ofn.lpstrFilter=filter;ofn.lpstrFile=file;ofn.nMaxFile=32768;
    ofn.lpstrDefExt=extension;ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&ofn))return false;path=file;return true;
}
void WriteAsync(HWND d,State& s,bool copy) {
    if(!Queue(d,s)||s.edits.empty())return;
    std::wstring destination;
    if(copy) {
        auto dot=s.path.find_last_of('.');destination=s.path.substr(0,dot)+L"-metadata."+ExtensionOf(s.path);
        std::wstring filter=T("metadata.same_format");filter.push_back(0);filter+=L"*."+ExtensionOf(s.path);filter.push_back(0);filter.push_back(0);
        if(!ChoosePath(d,filter.c_str(),destination,ExtensionOf(s.path).c_str()))return;
        if(_wcsicmp(destination.c_str(),s.path.c_str())==0){Error(d,T("metadata.copy_distinct"));return;}
    }
    s.busy=true;s.cancel=false;
    for(int id:{Search,Groups,List,Key,Type,Value,Preset,ProviderBox,Apply,Remove,Revert})Enable(d,id,false);
    s.saving=std::async(std::launch::async,[&s,destination] {
        SaveResult result;result.destination=destination;
        result.ok=s.engine.Save(s.doc,s.edits,destination,result.backup,result.error,&s.cancel);return result;
    });StatusText(d,s);
}
void NewField(HWND d,State& s) {
    if(!Queue(d,s))return;s.updating=true;s.selected=-1;s.adding=true;s.formDirty=false;
    Set(d,Search,L"");SendMessageW(C(d,Groups),CB_SETCURSEL,0,0);
    ListView_SetItemState(C(d,List),-1,0,LVIS_SELECTED);
    Enable(d,ProviderBox,true);SendMessageW(C(d,ProviderBox),CB_SETCURSEL,0,0);Enable(d,Preset,true);
    SendMessageW(C(d,Key),EM_SETREADONLY,FALSE,0);Set(d,Key,L"Xmp.dc.title");Set(d,Type,L"LangAlt");Set(d,Value,L"x-default=");
    Set(d,Description,T("metadata.new_hint"));SendMessageW(C(d,Value),EM_SETREADONLY,FALSE,0);
    Enable(d,Type,true);Enable(d,Apply,true);Enable(d,Remove,false);Enable(d,Revert,false);
    s.updating=false;s.formDirty=false;SetFocus(C(d,Value));StatusText(d,s);
}
void CopyText(HWND d,const std::wstring& text) {
    if(!OpenClipboard(d))return;
    HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
    if(mem){void* p=GlobalLock(mem);if(p){memcpy(p,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(mem);EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,mem))GlobalFree(mem);}else GlobalFree(mem);}
    CloseClipboard();
}
Document Effective(State& s) {
    BuildRows(s);Document doc=s.doc;doc.fields.clear();
    for(auto f:s.rows){auto e=Pending(s,f);if(e!=s.edits.end()){if(e->operation==CYMETA_DELETE)continue;f.type=e->type;f.value=e->value;}doc.fields.push_back(std::move(f));}return doc;
}
void Providers(HWND d,State& s) {
    std::wstring text=T("metadata.plugin_hint");text+=L"\n"+s.exeDir+L"\\plugins\n";
    for(auto& p:s.engine.Providers())text+=L"\n"+p.name+L"  "+p.version+L"\n"+p.file+L"\n";
    for(auto& p:s.doc.providers)if(!p.error.empty())text+=L"\n"+p.name+L": "+p.error+L"\n";
    MessageBoxW(d,text.c_str(),T("metadata.plugins"),MB_OK|MB_ICONINFORMATION);
}
bool Close(HWND d,State& s) {
    if(s.busy){s.cancel=true;s.wantClose=true;Set(d,Status,T("metadata.cancelling"));return false;}
    if((!s.edits.empty()||s.formDirty)&&MessageBoxW(d,T("metadata.discard"),T("metadata.title"),MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return false;
    EndDialog(d,IDOK);return true;
}
LRESULT CALLBACK Shortcuts(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    HWND d=GetAncestor(window,GA_ROOT);
    if(msg==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0) {
        if(wp=='S'){SendMessageW(d,WM_COMMAND,GetKeyState(VK_SHIFT)<0?SaveCopy:Save,0);return 0;}
        if(wp=='F'){SetFocus(C(d,Search));SendMessageW(C(d,Search),EM_SETSEL,0,-1);return 0;}
    }
    if(msg==WM_KEYDOWN&&wp==VK_F1){SendMessageW(d,WM_COMMAND,Help,0);return 0;}
    return DefSubclassProc(window,msg,wp,lp);
}
void Fill(HDC dc,const RECT& r,COLORREF color){HBRUSH brush=CreateSolidBrush(color);FillRect(dc,&r,brush);DeleteObject(brush);}
LRESULT CALLBACK HeaderPaint(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT) {
        auto& s=*reinterpret_cast<State*>(data);PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);RECT all{};GetClientRect(window,&all);Fill(dc,all,RGB(39,44,49));
        auto old=SelectObject(dc,s.font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,muted);
        for(int i=0;i<Header_GetItemCount(window);++i) {
            RECT r{};Header_GetItemRect(window,i,&r);r.left+=10;wchar_t text[128]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=text;item.cchTextMax=128;Header_GetItem(window,i,&item);
            DrawTextW(dc,text,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        SelectObject(dc,old);EndPaint(window,&ps);return 0;
    }
    return DefSubclassProc(window,msg,wp,lp);
}
void DrawButton(State& s,const DRAWITEMSTRUCT& item) {
    Fill(item.hDC,item.rcItem,bg);
    bool disabled=(item.itemState&ODS_DISABLED)!=0,selected=(item.itemState&ODS_SELECTED)!=0,primary=item.CtlID==Save;
    COLORREF color=disabled?RGB(32,36,41):selected?RGB(35,92,89):primary?RGB(20,125,119):RGB(43,50,56);
    HBRUSH brush=CreateSolidBrush(color);HPEN pen=CreatePen(PS_SOLID,1,primary&&!disabled?accent:RGB(60,72,78));
    auto oldB=SelectObject(item.hDC,brush),oldP=SelectObject(item.hDC,pen);const auto& r=item.rcItem;
    RoundRect(item.hDC,r.left,r.top,r.right,r.bottom,int(6*s.scale),int(6*s.scale));
    SelectObject(item.hDC,oldB);SelectObject(item.hDC,oldP);DeleteObject(brush);DeleteObject(pen);
    auto font=SelectObject(item.hDC,s.font);SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,disabled?RGB(108,122,131):ink);
    wchar_t text[256]{};GetWindowTextW(item.hwndItem,text,256);RECT label=r;InflateRect(&label,-7,0);
    DrawTextW(item.hDC,text,-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(item.hDC,font);if(item.itemState&ODS_FOCUS){RECT focus=r;InflateRect(&focus,-3,-3);DrawFocusRect(item.hDC,&focus);}
}
void Poll(HWND d,State& s) {
    using namespace std::chrono_literals;
    if(s.saving.valid()&&s.saving.wait_for(0ms)==std::future_status::ready) {
        auto result=s.saving.get();s.busy=false;
        if(result.ok) {
            if(result.destination.empty()) {
                s.changed=true;s.edits.clear();s.notice=std::wstring(T("metadata.saved"))+L"\n"+result.backup;ReadAsync(d,s,false);
            } else {s.notice=std::wstring(T("metadata.copy_saved"))+L"\n"+result.destination;Refresh(d,s);}
        } else {s.notice=result.error;Refresh(d,s);if(!s.wantClose)Error(d,result.error);}
    }
    if(s.reading.valid()&&s.reading.wait_for(0ms)==std::future_status::ready) {
        s.doc=s.reading.get();s.busy=false;
        for(int id:{Search,Groups,List,Key,Value})Enable(d,id,true);
        if(!s.doc.error.empty()){s.notice=s.doc.error;if(!s.wantClose)Error(d,s.doc.error);}
        Refresh(d,s);
    }
    if(s.wantClose&&!s.busy){s.wantClose=false;Close(d,s);}
}
INT_PTR CALLBACK Proc(HWND d,UINT msg,WPARAM wp,LPARAM lp) {
    auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(d,DWLP_USER));
    switch(msg) {
    case WM_INITDIALOG: {
        s=reinterpret_cast<State*>(lp);SetWindowLongPtrW(d,DWLP_USER,lp);SetWindowTextW(d,T("metadata.title"));
        s->scale=GetDpiForWindow(d)/96.0f;BOOL dark=TRUE;DwmSetWindowAttribute(d,20,&dark,sizeof(dark));DwmSetWindowAttribute(d,35,&bg,sizeof(bg));
        s->font=CreateFontW(-int(14*s->scale),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        s->titleFont=CreateFontW(-int(25*s->scale),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        s->mono=CreateFontW(-int(13*s->scale),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        auto add=[&](const wchar_t* cls,const wchar_t* label,int id,DWORD style,DWORD ex=0) {
            if(wcscmp(cls,L"EDIT")==0||wcscmp(cls,WC_LISTVIEWW)==0)ex=0;
            HWND c=CreateWindowExW(ex,cls,label,WS_CHILD|WS_VISIBLE|style,0,0,10,10,d,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),FALSE);SetWindowTheme(c,L"DarkMode_CFD",nullptr);
            if(style&WS_TABSTOP)SetWindowSubclass(c,Shortcuts,1,0);
            if(wcscmp(cls,L"COMBOBOX")==0&&(style&3)==CBS_DROPDOWN) {
                COMBOBOXINFO info{sizeof(info)};if(GetComboBoxInfo(c,&info)&&info.hwndItem)SetWindowSubclass(info.hwndItem,Shortcuts,1,0);
            }
            return c;
        };
        add(L"STATIC",T("metadata.title"),Title,0);SendMessageW(C(d,Title),WM_SETFONT,reinterpret_cast<WPARAM>(s->titleFont),FALSE);
        add(L"STATIC",FileNameOf(s->path).c_str(),Subtitle,SS_PATHELLIPSIS);
        add(L"EDIT",L"",Search,ES_AUTOHSCROLL|WS_TABSTOP,WS_EX_CLIENTEDGE);SendMessageW(C(d,Search),EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(T("metadata.search")));
        add(L"COMBOBOX",L"",Groups,CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL);
        add(WC_LISTVIEWW,L"",List,LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(C(d,List),LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
        ListView_SetBkColor(C(d,List),card);ListView_SetTextBkColor(C(d,List),card);ListView_SetTextColor(C(d,List),ink);
        auto header=ListView_GetHeader(C(d,List));SetWindowTheme(header,L"DarkMode_ItemsView",nullptr);
        SetWindowSubclass(header,HeaderPaint,1,reinterpret_cast<DWORD_PTR>(s));
        for(int i=0;i<3;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=120;col.pszText=const_cast<wchar_t*>(T(i==0?"metadata.field":i==1?"metadata.group":"metadata.state"));ListView_InsertColumn(C(d,List),i,&col);}
        add(L"EDIT",L"",Key,ES_AUTOHSCROLL|ES_READONLY|WS_TABSTOP,WS_EX_CLIENTEDGE);
        add(L"COMBOBOX",L"",Type,CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP);
        const wchar_t* types[]={L"Ascii",L"Byte",L"Short",L"Long",L"Rational",L"SByte",L"SShort",L"SLong",L"SRational",L"Undefined",L"Float",L"Double",L"String",L"Date",L"Time",L"Comment",L"XmpText",L"XmpBag",L"XmpSeq",L"XmpAlt",L"LangAlt",L"Hex",L"PNG-UTF8",L"PNG-Latin1",L"PNG-Language",L"PNG-Compression"};
        for(auto type:types)Combo(d,Type,type);
        add(L"EDIT",L"",Value,ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL|WS_TABSTOP,WS_EX_CLIENTEDGE);
        SendMessageW(C(d,Value),EM_SETLIMITTEXT,4*1024*1024,0);SendMessageW(C(d,Value),WM_SETFONT,reinterpret_cast<WPARAM>(s->mono),FALSE);
        add(L"COMBOBOX",L"",ProviderBox,CBS_DROPDOWN|WS_VSCROLL|WS_TABSTOP);
        add(L"COMBOBOX",L"",Preset,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
        Combo(d,Preset,T("metadata.presets"));const wchar_t* presets[]={L"Xmp.dc.title",L"Xmp.dc.description",L"Xmp.dc.creator",L"Xmp.dc.subject",L"Xmp.dc.rights",L"Exif.Image.Artist",L"Exif.Image.Copyright",L"Exif.Photo.DateTimeOriginal",L"Exif.GPSInfo.GPSLatitude",L"Exif.GPSInfo.GPSLatitudeRef",L"Exif.GPSInfo.GPSLongitude",L"Exif.GPSInfo.GPSLongitudeRef",L"Exif.GPSInfo.GPSAltitude",L"Exif.GPSInfo.GPSAltitudeRef",L"Iptc.Application2.Keywords",L"Iptc.Application2.Caption",L"Png.Text.parameters/text",L"Png.Text.Description/text",L"Comment"};
        for(auto preset:presets)Combo(d,Preset,preset);SendMessageW(C(d,Preset),CB_SETCURSEL,0,0);
        for(auto [id,key]:{std::pair{KeyLabel,"metadata.key"},{TypeLabel,"metadata.type"},{ValueLabel,"metadata.value"},{ProviderLabel,"metadata.provider"}})add(L"STATIC",T(key),id,0);
        for(int id:{Count,Description,Status})add(L"STATIC",L"",id,0);
        for(auto [id,key]:std::initializer_list<std::pair<int,const char*>>{{Add,"metadata.add"},{Apply,"metadata.apply"},{Remove,"metadata.delete"},{Revert,"metadata.revert"},{Reset,"metadata.reset"},{Save,"metadata.save"},{SaveCopy,"metadata.save_copy"},{Export,"metadata.export"},{Copy,"metadata.copy"},{Plugins,"metadata.plugins"},{Help,"metadata.help"},{IDCANCEL,"metadata.close"}})
            add(L"BUTTON",T(key),id,BS_OWNERDRAW|WS_TABSTOP);
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(d,MONITOR_DEFAULTTONEAREST),&mi);
        const int w=std::min(int(1120*s->scale),int(mi.rcWork.right-mi.rcWork.left-32)),h=std::min(int(760*s->scale),int(mi.rcWork.bottom-mi.rcWork.top-32));
        SetWindowPos(d,nullptr,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-w)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-h)/2,w,h,SWP_NOZORDER);
        Layout(d,*s);SetTimer(d,1,80,nullptr);ReadAsync(d,*s,true);return TRUE;
    }
    case WM_SIZE:if(s)Layout(d,*s);return TRUE;
    case WM_GETMINMAXINFO:if(s){auto* m=reinterpret_cast<MINMAXINFO*>(lp);m->ptMinTrackSize={int(900*s->scale),int(650*s->scale)};}return TRUE;
    case WM_TIMER:if(s)Poll(d,*s);return TRUE;
    case WM_DRAWITEM:if(s){DrawButton(*s,*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;}break;
    case WM_PAINT:if(s) {
        PAINTSTRUCT ps{};HDC dc=BeginPaint(d,&ps);Fill(dc,ps.rcPaint,bg);
        HPEN pen=CreatePen(PS_SOLID,1,RGB(56,65,73));auto old=SelectObject(dc,pen);auto brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        for(int id:{Search,List,Key,Value}){RECT r{};GetWindowRect(C(d,id),&r);MapWindowPoints(nullptr,d,reinterpret_cast<POINT*>(&r),2);InflateRect(&r,1,1);RoundRect(dc,r.left,r.top,r.right,r.bottom,4,4);}
        SelectObject(dc,old);SelectObject(dc,brush);DeleteObject(pen);EndPaint(d,&ps);return TRUE;
    }break;
    case WM_CTLCOLORDLG:return reinterpret_cast<INT_PTR>(s->background);
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT: {
        HDC dc=reinterpret_cast<HDC>(wp);bool edit=msg==WM_CTLCOLOREDIT||reinterpret_cast<HWND>(lp)==C(d,Value)||reinterpret_cast<HWND>(lp)==C(d,Key);
        SetTextColor(dc,ink);SetBkColor(dc,edit?card:bg);return reinterpret_cast<INT_PTR>(edit?s->editBrush:s->background);
    }
    case WM_NOTIFY: {
        auto* n=reinterpret_cast<NMHDR*>(lp);
        if(n->idFrom==List&&n->code==NM_CUSTOMDRAW) {
            auto* cd=reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
            if(cd->nmcd.dwDrawStage==CDDS_PREPAINT){SetWindowLongPtrW(d,DWLP_MSGRESULT,CDRF_NOTIFYITEMDRAW);return TRUE;}
            if(cd->nmcd.dwDrawStage==CDDS_ITEMPREPAINT) {
                int row=int(cd->nmcd.dwItemSpec);bool selected=(ListView_GetItemState(n->hwndFrom,row,LVIS_SELECTED)&LVIS_SELECTED)!=0;
                RECT rowRect{};ListView_GetItemRect(n->hwndFrom,row,&rowRect,LVIR_BOUNDS);
                Fill(cd->nmcd.hdc,rowRect,selected?RGB(27,76,74):card);auto old=SelectObject(cd->nmcd.hdc,s->font);SetBkMode(cd->nmcd.hdc,TRANSPARENT);
                for(int col=0;col<3;++col){RECT r{};ListView_GetSubItemRect(n->hwndFrom,row,col,LVIR_BOUNDS,&r);if(col==0)r.right=r.left+ListView_GetColumnWidth(n->hwndFrom,0);r.left+=9;
                    wchar_t text[512]{};ListView_GetItemText(n->hwndFrom,row,col,text,512);SetTextColor(cd->nmcd.hdc,col==0?ink:selected?ink:muted);
                    DrawTextW(cd->nmcd.hdc,text,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
                SelectObject(cd->nmcd.hdc,old);SetWindowLongPtrW(d,DWLP_MSGRESULT,CDRF_SKIPDEFAULT);return TRUE;
            }
        }
        if(n->idFrom==List&&n->code==LVN_ITEMCHANGED&&!s->updating&&!s->busy) {
            auto* item=reinterpret_cast<NMLISTVIEW*>(lp);
            if((item->uNewState&LVIS_SELECTED)&&!(item->uOldState&LVIS_SELECTED)&&size_t(item->iItem)<s->visible.size()) {
                auto target=s->rows[s->visible[item->iItem]];
                if(s->formDirty) {if(Queue(d,*s))Filter(d,*s,target.key,target.provider,target.occurrence);}
                else FillForm(d,*s,int(s->visible[item->iItem]));
            }
        }
        return FALSE;
    }
    case WM_COMMAND: {
        int id=LOWORD(wp),code=HIWORD(wp);if(!s)return FALSE;
        if(id==IDCANCEL){Close(d,*s);return TRUE;}
        if(id==Help){cyhelp::Open(d,s->exeDir,i18n::CurrentCode(),{"metadata","metadata"});return TRUE;}
        if(s->busy)return TRUE;
        if((id==Value||id==Key)&&code==EN_CHANGE&&!s->updating){s->formDirty=true;s->notice.clear();StatusText(d,*s);return TRUE;}
        if(id==Type&&(code==CBN_EDITCHANGE||code==CBN_SELCHANGE)&&!s->updating){s->formDirty=true;StatusText(d,*s);return TRUE;}
        if((id==Search&&code==EN_CHANGE)||(id==Groups&&code==CBN_SELCHANGE)) {if(!s->updating&&Queue(d,*s))Filter(d,*s);return TRUE;}
        if(id==Preset&&code==CBN_SELCHANGE&&s->adding&&Index(d,Preset)>0) {
            auto key=Get(d,Preset);s->updating=true;Set(d,Key,key);
            auto type=key.rfind(L"Xmp.dc.",0)==0?(key==L"Xmp.dc.creator"?L"XmpSeq":key==L"Xmp.dc.subject"?L"XmpBag":L"LangAlt"):
                key==L"Exif.GPSInfo.GPSAltitudeRef"?L"Byte":key.find(L"GPS")!=std::wstring::npos?(key.find(L"Ref")!=std::wstring::npos?L"Ascii":L"Rational"):
                key.rfind(L"Png.Text.",0)==0?L"PNG-UTF8":key.rfind(L"Iptc.",0)==0||key==L"Comment"?L"String":L"Ascii";
            Set(d,Type,type);Set(d,Value,wcscmp(type,L"LangAlt")==0?L"x-default=":key==L"Exif.GPSInfo.GPSAltitude"?L"0/1":key==L"Exif.GPSInfo.GPSAltitudeRef"?L"0":key==L"Exif.GPSInfo.GPSLatitudeRef"?L"N":key==L"Exif.GPSInfo.GPSLongitudeRef"?L"E":key.find(L"GPS")!=std::wstring::npos&&wcscmp(type,L"Rational")==0?L"0/1 0/1 0/1":L"");s->updating=false;s->formDirty=true;StatusText(d,*s);return TRUE;
        }
        switch(id) {
        case IDOK:if(s->formDirty){SendMessageW(d,WM_COMMAND,Apply,0);}else if(!s->edits.empty())WriteAsync(d,*s,false);break;
        case Add:NewField(d,*s);break;
        case Apply: {
            auto key=Get(d,Key);std::wstring provider;uint32_t occurrence=0;bool adding=s->adding;
            if(adding)s->formDirty=true; // Explicitly applying may create an empty value.
            if(s->adding){int w=Index(d,ProviderBox);if(w>=0&&size_t(w)<s->writers.size())provider=s->doc.providers[s->writers[w]].id;}
            else if(s->selected>=0){provider=s->rows[s->selected].provider;occurrence=s->rows[s->selected].occurrence;}
            if(Queue(d,*s)){if(adding&&!s->edits.empty())occurrence=s->edits.back().occurrence;Filter(d,*s,key,provider,occurrence);}break;
        }
        case Remove:case Revert: {
            if(s->selected<0)break;auto f=s->rows[s->selected];auto edit=Pending(*s,f);bool added=edit!=s->edits.end()&&edit->operation==CYMETA_ADD;
            if(edit!=s->edits.end())s->edits.erase(edit);
            if(id==Remove&&!added)s->edits.push_back({f.provider,f.key,f.type,f.value,f.occurrence,CYMETA_DELETE});
            s->formDirty=false;Filter(d,*s,f.key,f.provider,f.occurrence);break;
        }
        case Reset:s->edits.clear();s->formDirty=false;s->notice.clear();Filter(d,*s);break;
        case Save:WriteAsync(d,*s,false);break;case SaveCopy:WriteAsync(d,*s,true);break;
        case Export: {
            if(!Queue(d,*s))break;std::wstring path=s->path+L".metadata.json",error;
            if(ChoosePath(d,L"JSON\0*.json\0\0",path,L"json")&&!ExportJson(Effective(*s),path,error))Error(d,error);break;
        }
        case Copy: {
            if(!Queue(d,*s))break;std::wstring text;for(auto& f:Effective(*s).fields)text+=f.key+L"\t"+f.value+L"\r\n";CopyText(d,text);break;
        }
        case Plugins:Providers(d,*s);break;
        default:break;
        }return TRUE;
    }
    case WM_CLOSE:if(s)Close(d,*s);return TRUE;
    }
    return FALSE;
}
} // namespace
bool ShowDialog(HWND owner,const std::wstring& exeDir,const std::wstring& path,const std::vector<std::pair<std::wstring,std::wstring>>& decoderInfo) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    std::vector<WORD> t;
    auto word=[&](WORD v){t.push_back(v);};auto dword=[&](DWORD v){word(LOWORD(v));word(HIWORD(v));};
    auto str=[&](const wchar_t* v){while(*v)word(WORD(*v++));word(0);};
    dword(DS_SETFONT|DS_MODALFRAME|WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MAXIMIZEBOX|WS_CLIPCHILDREN);dword(WS_EX_CONTROLPARENT);
    word(0);word(0);word(0);word(640);word(430);word(0);word(0);str(L"");word(9);str(L"Segoe UI");
    State state;state.exeDir=exeDir;state.path=path;state.decoderInfo=decoderInfo;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<DLGTEMPLATE*>(t.data()),owner,Proc,reinterpret_cast<LPARAM>(&state));
    return state.changed;
}
} // namespace metadata
