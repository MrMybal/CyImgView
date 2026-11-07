#include "cyimg_metadata_plugin.h"
#include <exiv2/exiv2.hpp>
#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <windows.h>
#include "windows_io.h"
#include "png_text.h"

namespace {
std::recursive_mutex operations;
std::wstring Hex(const Exiv2::DataBuf&);
Exiv2::DataBuf Unhex(const std::wstring&);
std::string U(const std::wstring& s) {
    if (s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    if (!n) throw std::runtime_error("Invalid UTF-16");
    std::string r(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr); return r;
}
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);
    std::wstring r(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),r.data(),n); return r;
}
std::string Escape(const std::string& s) {
    if(s.empty())return "\\e";
    std::string r; for(char c:s) { if(c=='\\')r+="\\\\"; else if(c=='\n')r+="\\n"; else if(c=='\r')r+="\\r"; else r+=c; } return r;
}
std::string Unescape(const std::string& s) {
    if(s=="\\e")return {};
    std::string r; for(size_t i=0;i<s.size();++i) {
        if(s[i]!='\\') { r+=s[i];continue; }
        if(++i==s.size())throw std::runtime_error("Incomplete escape");
        if(s[i]=='n')r+='\n';else if(s[i]=='r')r+='\r';else if(s[i]=='\\')r+='\\';else throw std::runtime_error("Use \\n, \\r or \\\\ escapes");
    } return r;
}
std::vector<std::string> Lines(std::string s) {
    s.erase(std::remove(s.begin(),s.end(),'\r'),s.end());
    std::vector<std::string> r; std::istringstream in(s); std::string line;
    while(std::getline(in,line))r.push_back(line); return r;
}
std::string Serialize(const Exiv2::Value& v) {
    std::string s;
    if(v.typeId()==Exiv2::langAlt) {
        for(auto& [lang,text]:static_cast<const Exiv2::LangAltValue&>(v).value_) {
            if(!s.empty())s+='\n';s+=lang+"="+Escape(text);
        }
    } else if(v.typeId()==Exiv2::xmpBag || v.typeId()==Exiv2::xmpSeq || v.typeId()==Exiv2::xmpAlt) {
        for(size_t i=0;i<v.count();++i) { if(i)s+='\n';s+=Escape(v.toString(i)); }
    } else s=v.toString();
    return s;
}
Exiv2::Value::UniquePtr Parse(const std::string& type,const std::string& text) {
    if(type.rfind("Hex:",0)==0) {
        auto id=Exiv2::TypeInfo::typeId(type.substr(4));
        if(id==Exiv2::invalidTypeId||id==Exiv2::tiffIfd||id==Exiv2::tiffIfd8||id==Exiv2::directory)throw std::runtime_error("Invalid raw value type");
        auto bytes=Unhex(W(text));auto value=Exiv2::Value::create(id);
        if(value->read(bytes.c_data(),bytes.size(),Exiv2::littleEndian))throw std::runtime_error("Invalid raw metadata value");return value;
    }
    auto id=Exiv2::TypeInfo::typeId(type);
    if(id==Exiv2::invalidTypeId || id==Exiv2::tiffIfd || id==Exiv2::tiffIfd8 || id==Exiv2::directory)
        throw std::runtime_error("Unsupported or structural value type");
    auto v=Exiv2::Value::create(id);
    if(id==Exiv2::langAlt) {
        auto& map=static_cast<Exiv2::LangAltValue&>(*v).value_;
        for(const auto& line:Lines(text)) {
            auto eq=line.find('='); if(eq==std::string::npos || !eq)throw std::runtime_error("Use language=value, one language per line");
            auto lang=line.substr(0,eq);
            if(lang.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")!=std::string::npos || map.count(lang))
                throw std::runtime_error("Invalid or duplicate language");
            map[lang]=Unescape(line.substr(eq+1));
        }
    } else if(id==Exiv2::xmpBag || id==Exiv2::xmpSeq || id==Exiv2::xmpAlt) {
        for(const auto& line:Lines(text))if(v->read(Unescape(line)))throw std::runtime_error("Invalid array item");
    } else if(v->read(text)) throw std::runtime_error("Invalid value for "+type);
    return v;
}
bool Structural(const std::string& key) {
    if(key.rfind("Exif.",0)!=0)return false;
    // These values address or describe the encoded pixel storage, not descriptive metadata.
    const std::string name=key.substr(key.find_last_of('.')+1);
    static const char* names[]={"ImageWidth","ImageLength","BitsPerSample","Compression","PhotometricInterpretation",
        "StripOffsets","SamplesPerPixel","RowsPerStrip","StripByteCounts","PlanarConfiguration","Predictor",
        "TileWidth","TileLength","TileOffsets","TileByteCounts","SubIFDs","ExifTag","GPSTag","InteroperabilityTag",
        "JPEGInterchangeFormat","JPEGInterchangeFormatLength","JPEGTables","PixelXDimension","PixelYDimension",
        "MakerNote","MakerNoteSafety","DNGPrivateData","CFAPattern","CFARepeatPatternDim","InterColorProfile","XMLPacket","IPTCNAA","ImageResources"};
    for(auto n:names)if(name==n)return true;
    return key.find("Thumbnail")!=std::string::npos || key.rfind("Exif.SubImage",0)==0 || key.rfind("Exif.SubThumb",0)==0;
}
std::string Canonical(const std::string& key) {
    if(key.rfind("Exif.",0)==0)return Exiv2::ExifKey(key).key();
    if(key.rfind("Iptc.",0)==0)return Exiv2::IptcKey(key).key();
    if(key.rfind("Xmp.",0)==0)return Exiv2::XmpKey(key).key();
    return key;
}
std::wstring Hex(const Exiv2::DataBuf& b) {
    static const wchar_t* digits=L"0123456789abcdef";std::wstring s;s.reserve(b.size()*3);
    for(size_t i=0;i<b.size();++i) { if(i)s+=' ';auto v=b.c_data()[i];s+=digits[v>>4];s+=digits[v&15]; } return s;
}
Exiv2::DataBuf Unhex(const std::wstring& text) {
    std::vector<uint8_t> bytes;int high=-1;
    for(auto c:text) {
        if(c==' ' || c=='\r' || c=='\n' || c=='\t')continue;
        int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if(n<0)throw std::runtime_error("Invalid hexadecimal byte");
        if(high<0)high=n;else {bytes.push_back(uint8_t(high*16+n));high=-1;}
    }
    if(high>=0)throw std::runtime_error("Incomplete hexadecimal byte");
    return Exiv2::DataBuf(bytes.data(),bytes.size());
}
bool Writable(Exiv2::Image& image,Exiv2::MetadataId kind) {
    // The upstream factory's legacy access-mode table has no ICC column.
    if(kind==Exiv2::mdIccProfile) {
        auto type=image.imageType();return type==Exiv2::ImageType::jpeg||type==Exiv2::ImageType::png||type==Exiv2::ImageType::tiff||type==Exiv2::ImageType::webp||type==Exiv2::ImageType::jp2;
    }
    return (image.checkMode(kind)&Exiv2::amWrite)!=0;
}
template<class Data> bool Emit(const CyImgMetadataHost* h,void* ctx,Data& data,const wchar_t* group,bool write) {
    std::map<std::string,uint32_t> occurrences;
    for(const auto& tag:data) {
        if(h->cancelled(ctx))return false;
        auto raw=Serialize(tag.value());
        auto key=W(tag.key()),label=W(tag.tagLabel()),type=W(tag.typeName()),value=W(raw);
        bool binary=raw.find('\0')!=std::string::npos||(!raw.empty()&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,raw.data(),int(raw.size()),nullptr,0));
        if(binary) {
            Exiv2::DataBuf bytes(tag.value().size());tag.value().copy(bytes.data(),Exiv2::littleEndian);
            value=Hex(bytes);type=L"Hex:"+type;
        }
        auto desc=W(tag.tagName()); bool structure=Structural(tag.key());
        auto tid=tag.typeId(); structure=structure || tid==Exiv2::directory || tid==Exiv2::tiffIfd || tid==Exiv2::tiffIfd8;
        uint32_t flags=(write&&!structure?CYMETA_EDITABLE:0u)|(structure?CYMETA_STRUCTURAL:0u)|(binary?CYMETA_BINARY:0u);
        CyImgMetadataField f{sizeof(f),key.c_str(),label.c_str(),group,type.c_str(),value.c_str(),desc.c_str(),occurrences[tag.key()]++,flags};
        if(!h->field(ctx,&f))return false;
    } return true;
}
int Read(const CyImgMetadataHost* h,void* ctx,const wchar_t* path) {
    std::lock_guard lock(operations);
    try {
        auto image=Exiv2::ImageFactory::open(std::make_unique<MetadataWindowsIo>(path,false));
        if(!image)return CYMETA_UNSUPPORTED;
        image->readMetadata();
        auto emitInfo=[&](const wchar_t* key,const wchar_t* label,const std::wstring& value) {
            CyImgMetadataField f{sizeof(f),key,label,L"Image",L"String",value.c_str(),L"",0,CYMETA_STRUCTURAL};return h->field(ctx,&f)!=0;
        };
        if(!emitInfo(L"Image.Width",L"Width",std::to_wstring(image->pixelWidth()))||
           !emitInfo(L"Image.Height",L"Height",std::to_wstring(image->pixelHeight()))||
           !emitInfo(L"Image.MimeType",L"MIME type",W(image->mimeType())))return CYMETA_CANCELLED;
        const struct {Exiv2::MetadataId id;const wchar_t* name;} groups[]={
            {Exiv2::mdExif,L"Exif"},{Exiv2::mdIptc,L"Iptc"},{Exiv2::mdXmp,L"Xmp"},{Exiv2::mdComment,L"Comment"},{Exiv2::mdIccProfile,L"ICC"}};
        for(auto& g:groups)if(Writable(*image,g.id))h->writable_group(ctx,g.name);
        if(!Emit(h,ctx,image->exifData(),L"Exif",Writable(*image,Exiv2::mdExif)) ||
           !Emit(h,ctx,image->iptcData(),L"Iptc",Writable(*image,Exiv2::mdIptc)) ||
           !Emit(h,ctx,image->xmpData(),L"Xmp",Writable(*image,Exiv2::mdXmp)))return CYMETA_CANCELLED;
        if(!image->comment().empty()) {
            auto value=W(image->comment());
            CyImgMetadataField f{sizeof(f),L"Comment",L"Image comment",L"Comment",L"String",value.c_str(),L"",0,Writable(*image,Exiv2::mdComment)?CYMETA_EDITABLE:0u};
            if(!h->field(ctx,&f))return CYMETA_CANCELLED;
        }
        if(image->iccProfileDefined()) {
            auto value=Hex(image->iccProfile());
            CyImgMetadataField f{sizeof(f),L"ICC",L"ICC profile",L"ICC",L"Hex",value.c_str(),L"Binary profile, hexadecimal bytes",0,CYMETA_BINARY|(Writable(*image,Exiv2::mdIccProfile)?CYMETA_EDITABLE:0u)};
            if(!h->field(ctx,&f))return CYMETA_CANCELLED;
        }
        if(!ReadPngText(h,ctx,path))return CYMETA_CANCELLED;
        return CYMETA_OK;
    } catch(const Exiv2::Error& e) {
        if(e.code()==Exiv2::ErrorCode::kerFileContainsUnknownImageType)return CYMETA_UNSUPPORTED;
        auto message=W(e.what());h->error(ctx,message.c_str());return CYMETA_FAILED;
    } catch(const std::exception& e) {auto message=W(e.what());h->error(ctx,message.c_str());return CYMETA_FAILED;}
}
template<class Data,class Key> void Apply(Data& data,const CyImgMetadataEdit& edit) {
    Key parsedKey(U(edit.key));const auto key=parsedKey.key();uint32_t n=0;
    auto it=data.begin();for(;it!=data.end();++it)if(it->key()==key && n++==edit.occurrence)break;
    if(edit.operation!=CYMETA_ADD && it==data.end())throw std::runtime_error("Field no longer exists: "+key);
    if(edit.operation==CYMETA_DELETE) {data.erase(it);return;}
    auto value=Parse(U(edit.type),U(edit.value));
    if(edit.operation==CYMETA_ADD) {
        if constexpr(std::is_void_v<decltype(data.add(parsedKey,value.get()))>)data.add(parsedKey,value.get());
        else if(data.add(parsedKey,value.get())!=0)throw std::runtime_error("Cannot add this field (it may be non-repeatable): "+key);
    }
    else it->setValue(value.get());
}
struct Capture {std::map<std::wstring,std::vector<std::pair<std::wstring,std::wstring>>> fields;};
int CaptureField(void* p,const CyImgMetadataField* f) {
    static_cast<Capture*>(p)->fields[f->key].emplace_back(f->type,f->value);return 1;
}
void IgnoreGroup(void*,const wchar_t*) {}
void CaptureError(void*,const wchar_t*) {}
int NeverCancel(void*) {return 0;}
int Write(const CyImgMetadataHost* h,void* ctx,const wchar_t* path,const CyImgMetadataEdit* input,size_t count) {
    std::lock_guard lock(operations);
    try {
        auto image=Exiv2::ImageFactory::open(std::make_unique<MetadataWindowsIo>(path,true));if(!image)return CYMETA_UNSUPPORTED;
        image->readMetadata();
        std::vector<CyImgMetadataEdit> edits(input,input+count);
        std::vector<CyImgMetadataEdit> pngEdits;
        // Erasing a duplicate must not renumber the following edits in the same batch.
        std::stable_sort(edits.begin(),edits.end(),[](auto& a,auto& b){int c=wcscmp(a.key,b.key);return c?c<0:a.operation==CYMETA_ADD?false:b.operation==CYMETA_ADD?true:a.occurrence>b.occurrence;});
        for(const auto& e:edits) {
            if(h->cancelled(ctx))return CYMETA_CANCELLED;
            if(e.struct_size<sizeof(e) || e.operation>CYMETA_ADD || !e.key || !e.type || !e.value)throw std::runtime_error("Invalid metadata edit");
            const auto key=Canonical(U(e.key));
            if(IsPngText(e.key)){pngEdits.push_back(e);continue;}
            if(Structural(key))throw std::runtime_error("Pixel storage fields are read-only: "+key);
            Exiv2::MetadataId kind;
            if(key.rfind("Exif.",0)==0)kind=Exiv2::mdExif;
            else if(key.rfind("Iptc.",0)==0)kind=Exiv2::mdIptc;
            else if(key.rfind("Xmp.",0)==0)kind=Exiv2::mdXmp;
            else if(key=="Comment")kind=Exiv2::mdComment;
            else if(key=="ICC")kind=Exiv2::mdIccProfile;
            else throw std::runtime_error("Use an Exif.*, Iptc.*, Xmp.*, Comment or ICC key");
            if(!Writable(*image,kind))throw std::runtime_error("This metadata namespace is read-only for the file format");
            switch(kind) {
            case Exiv2::mdExif:Apply<Exiv2::ExifData,Exiv2::ExifKey>(image->exifData(),e);break;
            case Exiv2::mdIptc:Apply<Exiv2::IptcData,Exiv2::IptcKey>(image->iptcData(),e);break;
            case Exiv2::mdXmp:Apply<Exiv2::XmpData,Exiv2::XmpKey>(image->xmpData(),e);break;
            case Exiv2::mdComment:if(e.operation==CYMETA_DELETE)image->clearComment();else image->setComment(U(e.value));break;
            case Exiv2::mdIccProfile:if(e.operation==CYMETA_DELETE)image->clearIccProfile();else image->setIccProfile(Unhex(e.value));break;
            default:break;
            }
        }
        // Compare the requested tag collections after reopening, including repeatable tags.
        Capture wanted,actual; CyImgMetadataHost capture{1,sizeof(capture),CaptureField,IgnoreGroup,CaptureError,NeverCancel};
        Emit(&capture,&wanted,image->exifData(),L"Exif",true);
        Emit(&capture,&wanted,image->iptcData(),L"Iptc",true);
        Emit(&capture,&wanted,image->xmpData(),L"Xmp",true);
        if(!image->comment().empty())wanted.fields[L"Comment"]={{L"String",W(image->comment())}};
        if(image->iccProfileDefined())wanted.fields[L"ICC"]={{L"Hex",Hex(image->iccProfile())}};
        if(pngEdits.size()!=edits.size())image->writeMetadata();image.reset();
        if(!pngEdits.empty())WritePngText(h,ctx,path,pngEdits);
        if(Read(&capture,&actual,path)!=CYMETA_OK)throw std::runtime_error("Cannot reopen written metadata");
        for(const auto& e:edits)if(!IsPngText(e.key)&&wanted.fields[W(Canonical(U(e.key)))]!=actual.fields[W(Canonical(U(e.key)))])
            throw std::runtime_error("The format did not preserve the requested value: "+U(e.key));
        return CYMETA_OK;
    } catch(const std::exception& e) {auto message=W(e.what());h->error(ctx,message.c_str());return CYMETA_FAILED;}
}
const CyImgMetadataPlugin plugin{1,sizeof(plugin),L"exiv2",L"Exiv2 — EXIF / IPTC / XMP / GPS / ICC",L"0.28.9",Read,Write};
} // namespace
extern "C" __declspec(dllexport) const CyImgMetadataPlugin* CyImg_GetMetadataPlugin() {
    struct Runtime {
        bool ready=Exiv2::XmpParser::initialize();
        ~Runtime(){if(ready)Exiv2::XmpParser::terminate();}
    };
    static Runtime runtime;
    return runtime.ready?&plugin:nullptr;
}
