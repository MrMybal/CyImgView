#include "metadata.h"
#include "i18n.h"
#include <bcrypt.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace metadata {
namespace {
constexpr size_t maxText=4*1024*1024, maxTotal=32*1024*1024, maxFields=50000;
struct Handle { HANDLE h=INVALID_HANDLE_VALUE; ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);} };
bool Cancelled(const std::atomic<bool>* p){return p&&p->load();}
std::wstring Text(const wchar_t* p) {
    if(!p)return {};size_t n=wcsnlen_s(p,maxText+1);
    if(n>maxText)throw std::runtime_error("Metadata value exceeds the 4 Mi-character limit");
    return {p,n};
}
struct Context {
    Document* doc=nullptr; Provider* provider=nullptr; const std::atomic<bool>* cancel=nullptr;
    std::wstring error;size_t total=0;
};
int AddField(void* p,const CyImgMetadataField* f) {
    auto& c=*static_cast<Context*>(p);
    if(!c.doc||Cancelled(c.cancel))return 0;
    try {
        if(!f||f->struct_size<sizeof(*f)||!f->key||!f->value)throw std::runtime_error("Invalid provider field");
        Field v{c.provider->id,Text(f->key),Text(f->label),Text(f->group),Text(f->type),Text(f->value),Text(f->description),f->occurrence,f->flags};
        c.total+=v.key.size()+v.label.size()+v.group.size()+v.type.size()+v.value.size()+v.description.size();
        if(c.doc->fields.size()>=maxFields||c.total>maxTotal)throw std::runtime_error("Metadata resource limit reached; the result is incomplete");
        if(v.key.empty())throw std::runtime_error("Empty metadata key");
        c.doc->fields.push_back(std::move(v));return 1;
    } catch(const std::exception& e){c.error=Wide(e.what());return 0;}
}
void AddGroup(void* p,const wchar_t* group) {
    auto& c=*static_cast<Context*>(p);
    if(c.provider->writableGroups.size()<64)c.provider->writableGroups.push_back(Text(group));
}
void SetError(void* p,const wchar_t* error){static_cast<Context*>(p)->error=Text(error);}
int IsCancelled(void* p){auto& c=*static_cast<Context*>(p);return Cancelled(c.cancel)||!c.error.empty();}
const CyImgMetadataHost host{1,sizeof(host),AddField,AddGroup,SetError,IsCancelled};
int InvokeRead(const CyImgMetadataPlugin* api,Context* c,const wchar_t* path,bool* crashed) {
    __try {return api->read(&host,c,path);} __except(EXCEPTION_EXECUTE_HANDLER){*crashed=true;return CYMETA_FAILED;}
}
int InvokeWrite(const CyImgMetadataPlugin* api,Context* c,const wchar_t* path,const CyImgMetadataEdit* edits,size_t count,bool* crashed) {
    __try {return api->write(&host,c,path,edits,count);} __except(EXCEPTION_EXECUTE_HANDLER){*crashed=true;return CYMETA_FAILED;}
}
bool Descriptor(HMODULE module,CyImgMetadataPlugin* out) {
    __try {
        auto get=reinterpret_cast<CyImgGetMetadataPluginFn>(GetProcAddress(module,CYIMG_METADATA_ENTRY));
        if(!get)return false;auto p=get();
        if(!p||p->api_version!=1||p->struct_size<sizeof(*out)||!p->read||!p->id||!p->name)return false;
        *out=*p;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool FillPluginInfo(const CyImgMetadataPlugin* api,Provider* out) {
    out->id=Text(api->id);out->name=Text(api->name);out->version=Text(api->version);return !out->id.empty();
}
bool CopyPluginInfo(const CyImgMetadataPlugin* api,Provider* out) {
    __try {return FillPluginInfo(api,out);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Fingerprint(const std::wstring& path,std::array<uint8_t,32>& digest,const std::atomic<bool>* cancel) {
    Handle file;file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.h==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::vector<uint8_t> buffer(1<<20);DWORD got=0;
    while(ok) {
        if(Cancelled(cancel)){ok=false;break;}
        if(!ReadFile(file.h,buffer.data(),DWORD(buffer.size()),&got,nullptr)){ok=false;break;}
        if(!got)break;
        ok=BCryptHashData(hash,buffer.data(),got,0)>=0;
    }
    if(ok)ok=BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);return ok;
}
std::wstring PropText(const PROPVARIANT& v) {
    if(v.vt==(VT_VECTOR|VT_UI1)||v.vt==VT_BLOB) {
        const auto* data=v.vt==VT_BLOB?v.blob.pBlobData:v.caub.pElems;
        size_t count=v.vt==VT_BLOB?v.blob.cbSize:v.caub.cElems;
        if(count>maxText/3)throw std::runtime_error("WIC binary metadata exceeds the display limit");
        std::wstring text;static const wchar_t* digits=L"0123456789abcdef";text.reserve(count*3);
        for(size_t i=0;i<count;++i){if(i)text+=' ';text+=digits[data[i]>>4];text+=digits[data[i]&15];}return text;
    }
    PWSTR text=nullptr;std::wstring out;
    if(SUCCEEDED(PropVariantToStringAlloc(v,&text))&&text){out=text;CoTaskMemFree(text);}
    return out;
}
std::wstring IsoTime(const FILETIME& time) {
    SYSTEMTIME t{};if(!FileTimeToSystemTime(&time,&t))return {};
    wchar_t text[40]{};swprintf_s(text,L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);return text;
}
void WicFields(IWICMetadataQueryReader* qr,const std::wstring& prefix,Document& doc,int depth) {
    if(depth>16||doc.fields.size()>=maxFields)return;
    ComPtr<IEnumString> items;if(FAILED(qr->GetEnumerator(&items)))return;
    ULONG count=0;PWSTR name=nullptr;
    while(items->Next(1,&name,&count)==S_OK) {
        std::wstring key=name;CoTaskMemFree(name);name=nullptr;
        PROPVARIANT value;PropVariantInit(&value);
        if(SUCCEEDED(qr->GetMetadataByName(key.c_str(),&value))) {
            if(value.vt==VT_UNKNOWN&&value.punkVal) {
                ComPtr<IWICMetadataQueryReader> child;
                if(SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&child))))WicFields(child.Get(),prefix+key,doc,depth+1);
            } else {
                auto text=PropText(value);
                doc.fields.push_back({L"wic",prefix+key,prefix+key,L"WIC",L"PROPVARIANT "+std::to_wstring(value.vt),std::move(text),L"",0,0});
            }
        }
        PropVariantClear(&value);if(doc.fields.size()>=maxFields)break;
    }
}
void WicFallback(Document& doc) {
    ComPtr<IWICImagingFactory> f;ComPtr<IWICBitmapDecoder> decoder;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)))||
       FAILED(f->CreateDecoderFromFilename(doc.path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder)))return;
    ComPtr<IWICMetadataQueryReader> qr;
    if(SUCCEEDED(decoder->GetMetadataQueryReader(&qr)))WicFields(qr.Get(),L"Container",doc,0);
    UINT frames=0;decoder->GetFrameCount(&frames);
    for(UINT i=0;i<std::min(frames,1024u);++i) {
        ComPtr<IWICBitmapFrameDecode> frame;qr.Reset();
        if(SUCCEEDED(decoder->GetFrame(i,&frame))&&SUCCEEDED(frame->GetMetadataQueryReader(&qr)))
            WicFields(qr.Get(),L"Frame["+std::to_wstring(i)+L"]",doc,0);
    }
    doc.providers.push_back({L"wic",L"Windows Imaging Component",L"",L"",L"",{}});
}
std::string Json(const std::wstring& value) {
    const auto s=Utf8(value);std::string out="\"";static const char* hex="0123456789abcdef";
    for(unsigned char c:s) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);
    } return out+'"';
}
} // namespace
std::string Utf8(const std::wstring& s) {
    if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    if(!n)throw std::runtime_error("Invalid Unicode");std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr);return r;
}
std::wstring Wide(const std::string& s) {
    if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);
    std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),r.data(),n);return r;
}
Engine::~Engine(){for(auto& p:m_plugins)FreeLibrary(p.module);}
void Engine::Init(const std::wstring& exeDir) {
    std::lock_guard lock(m_mutex);if(!m_plugins.empty())return;
    WIN32_FIND_DATAW fd{};auto folder=exeDir+L"\\plugins\\";
    HANDLE find=FindFirstFileW((folder+L"*.dll").c_str(),&fd);if(find==INVALID_HANDLE_VALUE)return;
    do {
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        auto path=folder+fd.cFileName;
        HMODULE mod=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!mod)continue;
        Plugin p{};p.module=mod;
        bool ok=false;
        try {ok=Descriptor(mod,&p.api)&&CopyPluginInfo(&p.api,&p.info);}catch(...){ok=false;}
        if(ok) {
            ok=std::none_of(m_plugins.begin(),m_plugins.end(),[&](const Plugin& q){return q.info.id==p.info.id;});
        }
        if(!ok){FreeLibrary(mod);continue;}
        p.info.file=path;m_info.push_back(p.info);m_plugins.push_back(std::move(p));
    }while(FindNextFileW(find,&fd));FindClose(find);
}
Document Engine::Read(const std::wstring& path,const std::atomic<bool>* cancel) {
    std::lock_guard lock(m_mutex);Document doc;doc.path=path;
    try {
        if(!Fingerprint(path,doc.fingerprint,cancel)){doc.error=T("metadata.read_failed");return doc;}
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&fa)) {
            doc.fields.push_back({L"file",L"File.Name",T("viewer.info.file"),L"File",L"String",FileNameOf(path),L"",0,0});
            doc.fields.push_back({L"file",L"File.Path",T("viewer.info.folder"),L"File",L"String",ParentOf(path),L"",0,0});
            doc.fields.push_back({L"file",L"File.Size",T("viewer.info.size"),L"File",L"UInt64",std::to_wstring((uint64_t(fa.nFileSizeHigh)<<32)|fa.nFileSizeLow),L"bytes",0,0});
            doc.fields.push_back({L"file",L"File.Modified",T("viewer.info.modified"),L"File",L"UTC",IsoTime(fa.ftLastWriteTime),L"",0,0});
            doc.fields.push_back({L"file",L"File.Created",T("metadata.created"),L"File",L"UTC",IsoTime(fa.ftCreationTime),L"",0,0});
        }
        for(auto& p:m_plugins) {
            Provider info=p.info;Context c{&doc,&info,cancel};bool crashed=false;int status=CYMETA_FAILED;
            size_t begin=doc.fields.size();
            if(p.disabled)c.error=T("metadata.plugin_disabled");
            else try {status=InvokeRead(&p.api,&c,path.c_str(),&crashed);}catch(const std::exception& e){c.error=Wide(e.what());}
            if(crashed){p.disabled=true;c.error=T("metadata.plugin_crash");}
            if(status==CYMETA_UNSUPPORTED){doc.fields.resize(begin);continue;}
            if(!p.api.write) {info.writableGroups.clear();for(size_t i=begin;i<doc.fields.size();++i)doc.fields[i].flags&=~CYMETA_EDITABLE;}
            if(status!=CYMETA_OK||!c.error.empty()) {
                // Partial metadata is visible, but never treated as an editable snapshot.
                info.error=c.error.empty()?T("metadata.read_failed"):c.error;info.writableGroups.clear();
                for(size_t i=begin;i<doc.fields.size();++i)doc.fields[i].flags&=~CYMETA_EDITABLE;
            }
            doc.providers.push_back(std::move(info));
        }
        // WIC may expose container-specific fields absent from a standard provider.
        // Keep its query paths in a separate read-only namespace rather than losing them.
        if(!Cancelled(cancel))WicFallback(doc);
        std::array<uint8_t,32> after{};
        if(!Fingerprint(path,after,cancel)||after!=doc.fingerprint){doc.error=T("metadata.changed");return doc;}
        doc.valid=!Cancelled(cancel);
    }catch(const std::exception& e){doc.error=Wide(e.what());}
    return doc;
}
bool Engine::Save(const Document& doc,const std::vector<Edit>& edits,const std::wstring& destination,
                  std::wstring& backup,std::wstring& error,const std::atomic<bool>* cancel) {
    std::lock_guard lock(m_mutex);backup.clear();error.clear();
    std::wstring temporary;
    try {
        if(!doc.valid||edits.empty()){error=T("metadata.no_changes");return false;}
        const auto target=destination.empty()?doc.path:destination;
        bool original=destination.empty();
        std::error_code ec;
        if(!original&&(_wcsicmp(target.c_str(),doc.path.c_str())==0||std::filesystem::equivalent(target,doc.path,ec))) {
            error=T("metadata.copy_distinct");return false;
        }
        if(ExtensionOf(target)!=ExtensionOf(doc.path)){error=T("metadata.same_format");return false;}
        DWORD attrs=GetFileAttributesW(target.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_DIRECTORY))){error=T("metadata.read_only");return false;}
        std::array<uint8_t,32> current{};
        if(!Fingerprint(doc.path,current,cancel)||current!=doc.fingerprint){error=T("metadata.changed");return false;}
        // New file created exclusively alongside the destination: same-volume atomic replacement.
        for(unsigned i=0;i<100;++i) {
            temporary=(std::filesystem::path(target).parent_path()/(L".cyimg-meta-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(i)+L"."+ExtensionOf(doc.path))).wstring();
            if(CopyFileW(doc.path.c_str(),temporary.c_str(),TRUE))break;
            temporary.clear();if(GetLastError()!=ERROR_FILE_EXISTS)break;
        }
        if(temporary.empty())throw std::runtime_error("Cannot create metadata working copy");
        SetFileAttributesW(temporary.c_str(),FILE_ATTRIBUTE_NORMAL);
        std::map<std::wstring,std::vector<CyImgMetadataEdit>> batches;
        std::set<std::wstring> identities;
        for(auto& e:edits) {
            if(e.operation>CYMETA_ADD||e.key.empty())throw std::runtime_error("Invalid metadata edit");
            const auto id=e.provider+L"\n"+e.key+L"\n"+std::to_wstring(e.occurrence);
            if(e.operation!=CYMETA_ADD&&!identities.insert(id).second)throw std::runtime_error("Conflicting metadata edits");
            auto provider=std::find_if(doc.providers.begin(),doc.providers.end(),[&](auto& p){return p.id==e.provider;});
            if(provider==doc.providers.end()||!provider->error.empty()||provider->writableGroups.empty())throw std::runtime_error("Metadata provider is read-only or unavailable");
            if(e.operation!=CYMETA_ADD) {
                auto field=std::find_if(doc.fields.begin(),doc.fields.end(),[&](auto& f){return f.provider==e.provider&&f.key==e.key&&f.occurrence==e.occurrence;});
                if(field==doc.fields.end()||!field->Editable())throw std::runtime_error("Metadata field is read-only");
            }
            batches[e.provider].push_back({sizeof(CyImgMetadataEdit),e.key.c_str(),e.type.c_str(),e.value.c_str(),e.occurrence,e.operation});
        }
        for(auto& [id,batch]:batches) {
            auto p=std::find_if(m_plugins.begin(),m_plugins.end(),[&](auto& v){return v.info.id==id;});
            if(p==m_plugins.end()||p->disabled||!p->api.write)throw std::runtime_error("Metadata writer unavailable");
            Provider info=p->info;Context c{nullptr,&info,cancel};bool crashed=false;
            int result=InvokeWrite(&p->api,&c,temporary.c_str(),batch.data(),batch.size(),&crashed);
            if(crashed){p->disabled=true;c.error=T("metadata.plugin_crash");}
            if(result!=CYMETA_OK||!c.error.empty())throw std::runtime_error(Utf8(c.error.empty()?T("metadata.write_failed"):c.error));
        }
        if(Cancelled(cancel))throw std::runtime_error("Cancelled");
        // Detect outside changes again immediately before commit, while denying writes.
        Handle source;source.h=CreateFileW(doc.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(source.h==INVALID_HANDLE_VALUE||!Fingerprint(doc.path,current,cancel)||current!=doc.fingerprint)throw std::runtime_error(Utf8(T("metadata.changed")));
        Handle flush;flush.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(flush.h==INVALID_HANDLE_VALUE||!FlushFileBuffers(flush.h))throw std::runtime_error("Cannot flush metadata working copy");
        CloseHandle(flush.h);flush.h=INVALID_HANDLE_VALUE;
        bool ok=false;
        if(original) {
            backup=doc.path+L".metadata-backup";
            for(unsigned i=1;GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES&&i<10000;++i)backup=doc.path+L".metadata-backup-"+std::to_wstring(i);
            if(GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot reserve backup filename");
            ok=ReplaceFileW(doc.path.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr)!=FALSE;
        } else ok=MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
        if(!ok)throw std::runtime_error("Cannot commit metadata file (Windows error "+std::to_string(GetLastError())+")");
        temporary.clear();return true;
    }catch(const std::exception& e){error=Wide(e.what());}
    if(!temporary.empty())DeleteFileW(temporary.c_str());
    if(!backup.empty()&&GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES) {
        if(GetFileAttributesW(doc.path.c_str())==INVALID_FILE_ATTRIBUTES&&MoveFileExW(backup.c_str(),doc.path.c_str(),MOVEFILE_WRITE_THROUGH))backup.clear();
        else error+=L"\n"+std::wstring(T("metadata.recovery"))+L" "+backup;
    } else backup.clear();
    return false;
}
bool ExportJson(const Document& doc,const std::wstring& path,std::wstring& error) {
    std::wstring temporary;
    try {
        std::error_code ec;
        if(_wcsicmp(path.c_str(),doc.path.c_str())==0||std::filesystem::equivalent(path,doc.path,ec))throw std::runtime_error("Choose a separate file for the JSON export");
        std::string out="{\n  \"schema\": \"cyimg-metadata-1\",\n  \"source\": "+Json(doc.path)+",\n  \"fields\": [\n";
        for(size_t i=0;i<doc.fields.size();++i) {
            const auto& f=doc.fields[i];if(i)out+=",\n";
            out+="    {\"provider\":"+Json(f.provider)+",\"key\":"+Json(f.key)+",\"label\":"+Json(f.label)+",\"group\":"+Json(f.group)+",\"type\":"+Json(f.type)+",\"occurrence\":"+std::to_string(f.occurrence)+",\"editable\":"+(f.Editable()?"true":"false")+",\"value\":"+Json(f.value)+"}";
        }
        out+="\n  ]\n}\n";
        temporary=path+L".cyimg-json-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        Handle file;file.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        DWORD done=0;if(file.h==INVALID_HANDLE_VALUE||!WriteFile(file.h,out.data(),DWORD(out.size()),&done,nullptr)||done!=out.size()||!FlushFileBuffers(file.h))throw std::runtime_error("Cannot export metadata JSON");
        CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit metadata JSON export");
        temporary.clear();
        return true;
    }catch(const std::exception& e){error=Wide(e.what());if(!temporary.empty())DeleteFileW(temporary.c_str());return false;}
}
} // namespace metadata
