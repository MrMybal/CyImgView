// Minimal independent C ABI provider. Built only into testplugins, never packaged.
#include "cyimg_metadata_plugin.h"
#include <windows.h>
#include <string>
#include <fstream>
#include <filesystem>
namespace {
int Read(const CyImgMetadataHost* h,void* ctx,const wchar_t* path) {
    if(std::filesystem::path(path).extension()!=L".cymeta")return CYMETA_UNSUPPORTED;
    std::wifstream stream{std::filesystem::path(path)};std::wstring value;std::getline(stream,value);
    h->writable_group(ctx,L"Example");
    CyImgMetadataField field{sizeof(field),L"Example.Author",L"Independent provider field",L"Example",L"String",value.c_str(),L"Plugin test",0,CYMETA_EDITABLE};
    return h->field(ctx,&field)?CYMETA_OK:CYMETA_CANCELLED;
}
int Write(const CyImgMetadataHost* h,void* ctx,const wchar_t* path,const CyImgMetadataEdit* edits,size_t count) {
    for(size_t i=0;i<count;++i) {
        std::wofstream stream{std::filesystem::path(path)};stream<<(edits[i].operation==CYMETA_DELETE?L"":edits[i].value);stream.close();
        if(wcscmp(edits[i].value,L"FAIL")==0){h->error(ctx,L"Intentional writer failure after modifying the private copy");return CYMETA_FAILED;}
        if(wcscmp(edits[i].value,L"CRASH")==0)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    }return CYMETA_OK;
}
const CyImgMetadataPlugin plugin{1,sizeof(plugin),L"example",L"Independent test provider",L"1",Read,Write};
}
extern "C" __declspec(dllexport) const CyImgMetadataPlugin* CyImg_GetMetadataPlugin(){return &plugin;}
