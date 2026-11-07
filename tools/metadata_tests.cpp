#include "metadata.h"
#include "save.h"
#include "i18n.h"
#include <wincodec.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <algorithm>
namespace metadata_ui_tests {bool Run(const std::wstring&,const std::wstring&,const std::wstring&,bool);}

namespace fs=std::filesystem;
using namespace metadata;
namespace {
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);printf("PASS %s\n",text);}
std::vector<uint8_t> Bytes(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
const Field* Find(const Document& d,const std::wstring& key,uint32_t occurrence=0){for(auto& f:d.fields)if(f.key==key&&f.occurrence==occurrence)return &f;return nullptr;}
std::vector<uint8_t> Pixels(const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;ComPtr<IWICBitmapDecoder> dec;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> cv;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)))||
       FAILED(f->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&dec))||FAILED(dec->GetFrame(0,&frame)))throw std::runtime_error("Decode fixture");
    UINT w=0,h=0;frame->GetSize(&w,&h);f->CreateFormatConverter(&cv);cv->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    std::vector<uint8_t> out(size_t(w)*h*4);if(FAILED(cv->CopyPixels(nullptr,w*4,UINT(out.size()),out.data())))throw std::runtime_error("Fixture pixels");return out;
}
Edit Add(const wchar_t* key,const wchar_t* type,const wchar_t* value,uint32_t occurrence=0){return {L"exiv2",key,type,value,occurrence,CYMETA_ADD};}
Edit Set(const Field& f,const wchar_t* value){return {f.provider,f.key,f.type,value,f.occurrence,CYMETA_SET};}
void RoundTrip(Engine& e,const fs::path& root,const wchar_t* ext) {
    auto path=(root/(std::wstring(L"Photographie été 日本語.")+ext)).wstring();
    std::vector<uint8_t> pixels(64*48*4);
    for(size_t i=0;i<pixels.size();i+=4){pixels[i]=uint8_t(i%255);pixels[i+1]=uint8_t(i/37);pixels[i+2]=uint8_t(i/71);pixels[i+3]=255;}
    auto formats=EnumerateSaveFormats();auto fmt=std::find_if(formats.begin(),formats.end(),[&](auto& v){return std::find(v.exts.begin(),v.exts.end(),ext)!=v.exts.end();});
    Check(fmt!=formats.end(),"fixture encoder exists");std::wstring error,backup;
    Check(SaveImage({path,0,95},*fmt,pixels.data(),64,48,256,false,error),"write representative image");
    const auto before=Bytes(path),image=Pixels(path);auto doc=e.Read(path);
    Check(doc.valid,"metadata snapshot valid");
    for(auto& p:doc.providers)fwprintf(stderr,L"Provider %ls: %zu writable groups; %ls\n",p.id.c_str(),p.writableGroups.size(),p.error.c_str());
    Check(!e.Save(doc,{Add(L"Exif.Image.0x0100",L"Long",L"4096")},L"",backup,error)&&Bytes(path)==before,"numeric EXIF aliases cannot bypass pixel-layout protection");
    std::vector<Edit> edits={Add(L"Exif.Image.Artist",L"Ascii",L"CyImgView"),Add(L"Exif.Photo.ISOSpeedRatings",L"Short",L"400"),
        Add(L"Exif.GPSInfo.GPSLatitude",L"Rational",L"48/1 51/1 24/1"),Add(L"Exif.GPSInfo.GPSLatitudeRef",L"Ascii",L"N"),
        Add(L"Xmp.dc.title",L"LangAlt",L"x-default=Lake\nfr-FR=Lac été"),Add(L"Xmp.dc.subject",L"XmpBag",L"lake\nmountains, summer\n日本語"),
        Add(L"Iptc.Application2.Keywords",L"String",L"landscape"),Add(L"Iptc.Application2.Keywords",L"String",L"summer",1)};
    bool saved=e.Save(doc,edits,L"",backup,error);if(!saved)fwprintf(stderr,L"Native metadata error: %ls\n",error.c_str());
    Check(saved,"add EXIF GPS IPTC XMP metadata");Check(Bytes(backup)==before,"backup preserves original bytes");
    Check(Pixels(path)==image,"metadata-only save preserves decoded pixels exactly");doc=e.Read(path);
    for(auto& edit:edits)Check(Find(doc,edit.key,edit.occurrence)!=nullptr,"all requested fields persisted");
    Check(Find(doc,L"Iptc.Application2.Keywords",1)->value==L"summer","repeatable IPTC fields retain occurrences");
    Check(Find(doc,L"Xmp.dc.subject")->value==L"lake\nmountains, summer\n日本語","XMP array items preserve comma and Unicode");
    const auto current=Bytes(path);auto old=doc;
    std::vector<Edit> invalid={Set(*Find(doc,L"Exif.Photo.ISOSpeedRatings"),L"not a number")};
    Check(!e.Save(doc,invalid,L"",backup,error)&&Bytes(path)==current,"invalid typed value cannot alter source");
    auto title=Find(doc,L"Xmp.dc.title");Check(title&&title->value.find(L"fr-FR=Lac été")!=std::wstring::npos,"multilingual XMP preserved");
    auto artist=*Find(doc,L"Exif.Image.Artist");std::vector<Edit> change={Set(artist,L"New author")};
    auto copy=(root/(std::wstring(L"copy.")+ext)).wstring();Check(e.Save(doc,change,copy,backup,error),"save metadata copy");
    Check(Bytes(path)==current&&Find(e.Read(copy),L"Exif.Image.Artist")->value==L"New author","save copy leaves source byte-identical");
    auto keyword=*Find(doc,L"Iptc.Application2.Keywords",0);change={{keyword.provider,keyword.key,keyword.type,L"",0,CYMETA_DELETE},Set(*Find(doc,L"Iptc.Application2.Keywords",1),L"winter")};
    Check(e.Save(doc,change,L"",backup,error),"delete first repeatable field while editing second");doc=e.Read(path);
    Check(Find(doc,L"Iptc.Application2.Keywords")->value==L"winter"&&!Find(doc,L"Iptc.Application2.Keywords",1),"duplicate edit addresses original occurrence");
    Check(!e.Save(old,{Set(artist,L"stale")},L"",backup,error),"stale snapshot rejected");
    Check(ExportJson(doc,path+L".json",error),"export complete metadata as UTF-8 JSON");
    auto json=Bytes(path+L".json");Check(json.size()>100,"JSON contains metadata fields");
    auto width=Find(doc,L"Exif.Image.ImageWidth");if(width)Check(!width->Editable(),"pixel storage fields are read-only");
    auto encodedBefore=Bytes(path);
    const auto subject=*Find(doc,L"Xmp.dc.subject");
    const wchar_t* complex=L"one\\\\path\nline\\nwrap\n\\e\nlast";
    Check(e.Save(doc,{Set(subject,complex)},L"",backup,error),"edit XMP arrays with escaped backslashes newlines and empty items");
    doc=e.Read(path);if(Find(doc,L"Xmp.dc.subject")->value!=complex)fwprintf(stderr,L"Expected [%ls] Actual [%ls]\n",complex,Find(doc,L"Xmp.dc.subject")->value.c_str());Check(Find(doc,L"Xmp.dc.subject")->value==complex,"XMP escaped values round-trip exactly");
    if(std::wstring(ext)==L"jpg") {
        auto scan=[](const std::vector<uint8_t>& b){for(size_t i=0;i+1<b.size();++i)if(b[i]==0xff&&b[i+1]==0xda)return std::vector<uint8_t>(b.begin()+i,b.end());return std::vector<uint8_t>{};};
        Check(!scan(encodedBefore).empty()&&scan(encodedBefore)==scan(Bytes(path)),"JPEG compressed scan data retained byte-for-byte");
    }
    auto copyDoc=doc;auto bytesBefore=Bytes(path);
    Check(!e.Save(doc,{Set(*Find(doc,L"Exif.Image.Artist"),L"alias")},path,backup,error)&&Bytes(path)==bytesBefore,"save-copy rejects the original path");
    Check(!ExportJson(doc,path,error)&&Bytes(path)==bytesBefore,"JSON export cannot overwrite the source image");
    auto binary=Set(*Find(doc,L"Exif.Image.Artist"),L"52 e9 6e e9 00");binary.type=L"Hex:Ascii";
    Check(e.Save(doc,{binary},L"",backup,error),"edit a legacy non-UTF8 EXIF byte string without lossy conversion");
    doc=e.Read(path);auto raw=Find(doc,L"Exif.Image.Artist");
    Check(raw&&raw->type==L"Hex:Ascii"&&raw->value==L"52 e9 6e e9 00","legacy bytes exposed as exact editable hexadecimal");
    Check(e.Save(doc,{Set(*Find(doc,L"Xmp.dc.title"),L"x-default=Lake\nfr-FR=Lac été")},L"",backup,error),"unrelated metadata can be edited alongside legacy byte strings");
    doc=e.Read(path);raw=Find(doc,L"Exif.Image.Artist");
    auto restore=Set(*raw,L"CyImgView");restore.type=L"Ascii";Check(e.Save(doc,{restore},L"",backup,error),"replace raw EXIF with a standard text value");
    doc=e.Read(path);
    std::wstring profile;const wchar_t* hex=L"0123456789abcdef";
    std::vector<uint8_t> icc(132);icc[3]=132;memcpy(icc.data()+12,"mntrRGB XYZ ",12);memcpy(icc.data()+36,"acsp",4);
    for(size_t i=0;i<icc.size();++i){if(i)profile+=' ';profile+=hex[icc[i]>>4];profile+=hex[icc[i]&15];}
    Check(e.Save(doc,{Add(L"ICC",L"Hex",profile.c_str())},L"",backup,error),"add binary ICC profile");doc=e.Read(path);
    Check(Find(doc,L"ICC")&&Find(doc,L"ICC")->value==profile&&Find(doc,L"ICC")->Editable(),"ICC bytes and write capability preserved");
    auto iccField=*Find(doc,L"ICC");Check(e.Save(doc,{{iccField.provider,iccField.key,iccField.type,L"",0,CYMETA_DELETE}},L"",backup,error),"remove ICC profile");
    if(std::wstring(ext)==L"png") {
        doc=e.Read(path);const auto originalPixels=Pixels(path);const auto originalPng=Bytes(path);
        Check(e.Save(doc,{Add(L"Png.Text.parameters/text",L"PNG-UTF8",L"a landscape\nnegative: artifacts\n日本語"),Add(L"Png.Text.parameters/text",L"PNG-Latin1",L"second caption",1)},L"",backup,error),"add arbitrary PNG text and repeated keywords");
        doc=e.Read(path);auto png=*Find(doc,L"Png.Text.parameters/text");
        Check(png.value==L"a landscape\nnegative: artifacts\n日本語"&&Find(doc,png.key,1)->value==L"second caption","custom PNG Unicode text and duplicates retained");
        auto language=*Find(doc,L"Png.Text.parameters/language"),translated=*Find(doc,L"Png.Text.parameters/translated"),compression=*Find(doc,L"Png.Text.parameters/compression");
        Check(e.Save(doc,{Set(language,L"fr-FR"),Set(translated,L"Paramètres"),Set(compression,L"1"),Set(*Find(doc,png.key,1),L"caption changed")},L"",backup,error),"edit compressed iTXt language translated keyword and Latin-1 text");
        doc=e.Read(path);Check(Find(doc,language.key)->value==L"fr-FR"&&Find(doc,translated.key)->value==L"Paramètres"&&Find(doc,compression.key)->value==L"1","all iTXt attributes round-trip");
        Check(Pixels(path)==originalPixels,"PNG custom metadata does not change pixels");
        auto imageChunks=[](const std::vector<uint8_t>& bytes){std::vector<uint8_t> out;for(size_t i=8;i+12<=bytes.size();){size_t n=(size_t(bytes[i])<<24)|(size_t(bytes[i+1])<<16)|(size_t(bytes[i+2])<<8)|bytes[i+3];if(n>bytes.size()-i-12)break;if(!memcmp(bytes.data()+i+4,"IDAT",4)||!memcmp(bytes.data()+i+4,"fdAT",4))out.insert(out.end(),bytes.begin()+i,bytes.begin()+i+n+12);i+=n+12;}return out;};
        Check(!imageChunks(originalPng).empty()&&imageChunks(originalPng)==imageChunks(Bytes(path)),"PNG compressed IDAT/fdAT chunks retained byte-for-byte");
        auto first=*Find(doc,png.key);Check(e.Save(doc,{{first.provider,first.key,first.type,L"",0,CYMETA_DELETE},Set(*Find(doc,png.key,1),L"remaining")},L"",backup,error),"delete first PNG text occurrence while editing second");
        doc=e.Read(path);Check(Find(doc,png.key)->value==L"remaining"&&!Find(doc,png.key,1),"PNG repeated text uses original occurrence indices");
    }
    std::atomic<bool> cancel=true;Check(!e.Read(path,&cancel).valid,"cancelled reads do not become editable documents");
}
}
int wmain(int argc,wchar_t**) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);auto bin=fs::path(module).parent_path();
        auto root=bin/(L"metadata-fixtures-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if(!fs::create_directory(root)||fs::weakly_canonical(root).parent_path()!=fs::weakly_canonical(bin))throw std::runtime_error("Unsafe or existing metadata fixture directory");
        fs::create_directory(root/L"plugins");
        fs::copy_file(bin/L"plugins/metadata_exiv2.dll",root/L"plugins/metadata_exiv2.dll",fs::copy_options::overwrite_existing);
        fs::copy_file(bin/L"testplugins/metadata_example.dll",root/L"plugins/metadata_example.dll",fs::copy_options::overwrite_existing);
        i18n::Init(bin.wstring(),L"en");
        { // Unload provider DLLs before removing their isolated fixture directory.
        Engine engine;engine.Init(root.wstring());Check(engine.Providers().size()==2,"discover independent metadata-provider DLLs");
        RoundTrip(engine,root,L"jpg");RoundTrip(engine,root,L"png");RoundTrip(engine,root,L"tif");
        auto file=(root/L"provider.cymeta").wstring();{std::wofstream out{fs::path(file)};out<<L"Before";}
        auto doc=engine.Read(file);auto f=Find(doc,L"Example.Author");Check(f&&f->Editable(),"external plugin contributes typed editable metadata");
        auto before=Bytes(file);std::wstring backup,error;
        Check(!engine.Save(doc,{Set(*f,L"FAIL")},L"",backup,error)&&Bytes(file)==before,"plugin failure rolls back private-copy changes");
        Check(engine.Save(doc,{Set(*f,L"After")},L"",backup,error),"external plugin writes metadata through host transaction");
        doc=engine.Read(file);f=Find(doc,L"Example.Author");before=Bytes(file);
        Check(!engine.Save(doc,{Set(*f,L"CRASH")},L"",backup,error)&&Bytes(file)==before,"plugin crash isolated and original retained");
        auto damaged=engine.Read(file);Check(std::any_of(damaged.providers.begin(),damaged.providers.end(),[](auto& p){return p.id==L"example"&&!p.error.empty();}),"crashed provider disabled for session");
        }
        if(argc>1) {
            const auto image=(root/L"Photographie été 日本語.jpg").wstring();
            Check(metadata_ui_tests::Run(bin.wstring(),image,(root/L"metadata-en.png").wstring(),true),"English metadata editor interactions");
            i18n::SetLanguage(L"fr");
            Check(metadata_ui_tests::Run(bin.wstring(),image,(root/L"metadata-fr.png").wstring(),false),"French metadata reader and localization");
            printf("Fixtures retained: %ls\n",root.c_str());
        }else {
            if(fs::weakly_canonical(root).parent_path()!=fs::weakly_canonical(bin)||root.filename().wstring().rfind(L"metadata-fixtures-",0)!=0)throw std::runtime_error("Refusing unsafe fixture cleanup");
            fs::remove_all(root);
        }
        printf("All metadata integration tests passed.\n");CoUninitialize();return 0;
    } catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());CoUninitialize();return 1;}
}
