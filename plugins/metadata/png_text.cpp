#include "png_text.h"
#include <windows.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
constexpr size_t limit=4*1024*1024;
struct Text {
    std::string keyword,language;
    std::wstring text,translated;
    bool utf8=false,compressed=false,removed=false;
    uint32_t occurrence=0;
};
struct Chunk {uint64_t offset=0,size=0;int text=-1;bool end=false;};
struct Png {std::vector<Text> text;std::vector<Chunk> chunks;uint64_t size=0;};
const unsigned char signature[]={137,80,78,71,13,10,26,10};
uint32_t Be(const unsigned char* p){return(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
void Append32(std::vector<unsigned char>& v,uint32_t n){for(int shift=24;shift>=0;shift-=8)v.push_back(uint8_t(n>>shift));}
std::wstring Latin(const std::string& s){std::wstring r;r.reserve(s.size());for(unsigned char c:s)r+=wchar_t(c);return r;}
std::string Latin(const std::wstring& s){std::string r;r.reserve(s.size());for(auto c:s){if(c>255||c==0)throw std::runtime_error("PNG Latin-1 text cannot contain this character; use PNG-UTF8");r+=char(c);}return r;}
std::wstring Wide(const std::string& s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);if(!n)throw std::runtime_error("Invalid UTF-8 PNG text");std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),r.data(),n);return r;}
std::string Utf8(const std::wstring& s){if(s.empty())return{};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);if(!n)throw std::runtime_error("Invalid Unicode PNG text");std::string r(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),r.data(),n,nullptr,nullptr);return r;}
std::string Inflate(const std::string& s){std::string out(limit+1,0);uLongf size=uLongf(out.size());if(uncompress(reinterpret_cast<Bytef*>(out.data()),&size,reinterpret_cast<const Bytef*>(s.data()),uLong(s.size()))!=Z_OK||size>limit)throw std::runtime_error("Invalid or oversized compressed PNG metadata");out.resize(size);return out;}
std::string Deflate(const std::string& s){uLongf size=compressBound(uLong(s.size()));std::string out(size,0);if(compress2(reinterpret_cast<Bytef*>(out.data()),&size,reinterpret_cast<const Bytef*>(s.data()),uLong(s.size()),Z_DEFAULT_COMPRESSION)!=Z_OK)throw std::runtime_error("Cannot compress PNG metadata");out.resize(size);return out;}
std::string Part(const std::string& s,size_t& pos){auto end=s.find('\0',pos);if(end==std::string::npos)throw std::runtime_error("Invalid PNG text chunk");auto out=s.substr(pos,end-pos);pos=end+1;return out;}
bool Keyword(const std::string& key){if(key.empty()||key.size()>79||key.front()==' '||key.back()==' '||key.find("  ")!=std::string::npos)return false;for(unsigned char c:key)if(c<32||(c>126&&c<161))return false;return true;}
std::wstring Key(const std::string& keyword){std::wstring out=L"Png.Text.";static const wchar_t* h=L"0123456789ABCDEF";for(unsigned char c:keyword){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')out+=wchar_t(c);else{out+='%';out+=h[c>>4];out+=h[c&15];}}return out;}
std::pair<std::string,std::wstring> ParseKey(const wchar_t* raw){std::wstring s(raw);auto slash=s.find_last_of('/');if(slash==std::wstring::npos||slash<9)throw std::runtime_error("Use Png.Text.keyword/text");std::string key;for(size_t i=9;i<slash;++i){wchar_t c=s[i];if(c=='%'){if(i+2>=slash)throw std::runtime_error("Invalid escaped PNG keyword");auto hex=[](wchar_t v){if(v>='0'&&v<='9')return int(v-'0');if(v>='A'&&v<='F')return int(v-'A'+10);if(v>='a'&&v<='f')return int(v-'a'+10);return -1;};int a=hex(s[++i]),b=hex(s[++i]);if(a<0||b<0)throw std::runtime_error("Invalid PNG keyword escape");key+=char(a*16+b);}else{if(c>255)throw std::runtime_error("PNG keywords are Latin-1");key+=char(c);}}if(!Keyword(key))throw std::runtime_error("PNG keywords must be 1–79 Latin-1 characters without repeated or surrounding spaces");return{key,s.substr(slash+1)};}
Png Load(const wchar_t* path) {
    std::ifstream in{std::filesystem::path(path),std::ios::binary};if(!in)throw std::runtime_error("Cannot open PNG metadata source");
    in.seekg(0,std::ios::end);auto length=in.tellg();if(length<8)return{};in.seekg(0);
    std::array<unsigned char,8> sig{};in.read(reinterpret_cast<char*>(sig.data()),8);if(memcmp(sig.data(),signature,8)!=0)return{};
    Png png;png.size=uint64_t(length);uint64_t offset=8;size_t budget=0;std::map<std::string,uint32_t> counts;bool ended=false;
    while(offset+12<=png.size) {
        in.seekg(std::streamoff(offset));unsigned char header[8]{};in.read(reinterpret_cast<char*>(header),8);uint32_t size=Be(header);
        if(uint64_t(size)+12>png.size-offset)throw std::runtime_error("Invalid PNG chunk size");
        std::string kind(reinterpret_cast<char*>(header+4),4);Chunk chunk{offset,uint64_t(size)+12,-1,kind=="IEND"};
        if(kind=="tEXt"||kind=="zTXt"||kind=="iTXt") {
            if(size>limit||budget+size>32*limit)throw std::runtime_error("PNG text metadata exceeds the resource limit");
            std::string data(size,0);in.read(data.data(),size);unsigned char checksum[4]{};in.read(reinterpret_cast<char*>(checksum),4);
            uLong crc=crc32(0,header+4,4);crc=crc32(crc,reinterpret_cast<const Bytef*>(data.data()),size);
            if(!in||uint32_t(crc)!=Be(checksum))throw std::runtime_error("Invalid PNG text checksum");
            Text text;size_t pos=0;text.keyword=Part(data,pos);text.utf8=kind=="iTXt";text.compressed=kind=="zTXt";text.occurrence=counts[text.keyword]++;
            if(kind=="zTXt"){if(pos>=data.size()||data[pos++]!=0)throw std::runtime_error("Unsupported PNG metadata compression");}
            if(text.utf8){if(pos+2>data.size()||(data[pos]!=0&&data[pos]!=1)||data[pos+1]!=0)throw std::runtime_error("Invalid iTXt compression flags");text.compressed=data[pos]!=0;pos+=2;text.language=Part(data,pos);text.translated=Wide(Part(data,pos));}
            auto raw=data.substr(pos);if(text.compressed)raw=Inflate(raw);
            if(raw.find('\0')!=std::string::npos)throw std::runtime_error("PNG text contains an invalid NUL character");
            text.text=text.utf8?Wide(raw):Latin(raw);budget+=raw.size()+data.size();if(budget>32*limit)throw std::runtime_error("PNG text metadata exceeds the resource limit");
            chunk.text=int(png.text.size());png.text.push_back(std::move(text));
        }
        png.chunks.push_back(chunk);offset+=chunk.size;
        if(chunk.end){if(size!=0)throw std::runtime_error("Invalid PNG end chunk");ended=true;break;}
    }
    if(!ended)throw std::runtime_error("PNG end chunk missing");
    if(offset<png.size)png.chunks.push_back({offset,png.size-offset,-1,false});return png;
}
std::vector<unsigned char> Encode(const Text& t) {
    if(!Keyword(t.keyword))throw std::runtime_error("Invalid PNG keyword");
    std::string payload=t.keyword;payload+='\0';std::string type=t.utf8?"iTXt":t.compressed?"zTXt":"tEXt";
    if(t.utf8){payload+=char(t.compressed);payload+='\0';payload+=t.language;payload+='\0';payload+=Utf8(t.translated);payload+='\0';}
    else if(t.compressed)payload+='\0';
    auto text=t.utf8?Utf8(t.text):Latin(t.text);if(text.size()>limit)throw std::runtime_error("PNG metadata text too large");payload+=t.compressed?Deflate(text):text;
    std::vector<unsigned char> out;Append32(out,uint32_t(payload.size()));out.insert(out.end(),type.begin(),type.end());out.insert(out.end(),payload.begin(),payload.end());
    auto crc=crc32(0,out.data()+4,uInt(out.size()-4));Append32(out,uint32_t(crc));return out;
}
bool Same(const Text& a,const Text& b){return a.keyword==b.keyword&&a.text==b.text&&a.language==b.language&&a.translated==b.translated&&a.utf8==b.utf8&&a.compressed==b.compressed;}
}
bool ReadPngText(const CyImgMetadataHost* h,void* ctx,const wchar_t* path) {
    auto png=Load(path);if(!png.size)return true;h->writable_group(ctx,L"PNG");
    for(auto& t:png.text) {
        const auto base=Key(t.keyword);auto label=Latin(t.keyword);
        auto emit=[&](const wchar_t* suffix,const wchar_t* type,const std::wstring& value) {
            auto key=base+suffix;auto name=label+(wcscmp(suffix,L"/text")==0?L"":std::wstring(suffix));
            CyImgMetadataField f{sizeof(f),key.c_str(),name.c_str(),L"PNG",type,value.c_str(),L"PNG tEXt / zTXt / iTXt",t.occurrence,CYMETA_EDITABLE};return h->field(ctx,&f)!=0;
        };
        if(h->cancelled(ctx)||!emit(L"/text",t.utf8?L"PNG-UTF8":L"PNG-Latin1",t.text))return false;
        if(t.utf8&&(!emit(L"/language",L"PNG-Language",Latin(t.language))||!emit(L"/translated",L"PNG-UTF8",t.translated)))return false;
        if(!emit(L"/compression",L"PNG-Compression",t.compressed?L"1":L"0"))return false;
    }return true;
}
void WritePngText(const CyImgMetadataHost* h,void* ctx,const wchar_t* path,const std::vector<CyImgMetadataEdit>& edits) {
    auto png=Load(path);if(!png.size)throw std::runtime_error("PNG text requires a PNG file");auto original=png.text;
    for(auto& edit:edits) {
        if(h->cancelled(ctx))throw std::runtime_error("Cancelled");auto [keyword,part]=ParseKey(edit.key);
        auto found=std::find_if(png.text.begin(),png.text.end(),[&](auto& t){return t.keyword==keyword&&t.occurrence==edit.occurrence;});
        if(edit.operation==CYMETA_ADD) {
            if(part!=L"text"||found!=png.text.end())throw std::runtime_error("Add a new PNG text occurrence through its /text field");
            Text t;t.keyword=keyword;t.occurrence=edit.occurrence;t.utf8=true;png.text.push_back(std::move(t));found=png.text.end()-1;
        }
        if(found==png.text.end())throw std::runtime_error("PNG text occurrence no longer exists");
        if(edit.operation==CYMETA_DELETE){if(part==L"text")found->removed=true;else if(part==L"language")found->language.clear();else if(part==L"translated")found->translated.clear();else if(part==L"compression")found->compressed=false;else throw std::runtime_error("Unknown PNG metadata field");continue;}
        if(part==L"text"){if(wcscmp(edit.type,L"PNG-UTF8")!=0&&wcscmp(edit.type,L"PNG-Latin1")!=0)throw std::runtime_error("Use PNG-UTF8 or PNG-Latin1 for PNG text");found->utf8=wcscmp(edit.type,L"PNG-UTF8")==0;found->text=edit.value;if(!found->utf8){Latin(found->text);found->language.clear();found->translated.clear();}}
        else if(part==L"language"){auto language=Latin(std::wstring(edit.value));if(language.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")!=std::string::npos)throw std::runtime_error("Invalid PNG language tag");found->utf8=true;found->language=language;}
        else if(part==L"translated"){found->utf8=true;found->translated=edit.value;}
        else if(part==L"compression"){if(wcscmp(edit.value,L"0")&&wcscmp(edit.value,L"1"))throw std::runtime_error("PNG compression must be 0 or 1");found->compressed=wcscmp(edit.value,L"1")==0;}
        else throw std::runtime_error("Unknown PNG text field");
    }
    std::wstring temporary=std::wstring(path)+L".png-text";
    try {
        std::ifstream in{std::filesystem::path(path),std::ios::binary};
        HANDLE out=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(out==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create PNG metadata output");
        try {
            auto write=[&](const unsigned char* data,size_t count){while(count){DWORD n=0;if(!WriteFile(out,data,DWORD(std::min<size_t>(count,1<<20)),&n,nullptr)||!n)throw std::runtime_error("Cannot write PNG metadata");data+=n;count-=n;}};
            write(signature,8);std::array<unsigned char,65536> buffer{};
            for(auto& chunk:png.chunks) {
                if(h->cancelled(ctx))throw std::runtime_error("Cancelled");
                if(chunk.end)for(size_t i=original.size();i<png.text.size();++i)if(!png.text[i].removed){auto bytes=Encode(png.text[i]);write(bytes.data(),bytes.size());}
                if(chunk.text>=0){auto& t=png.text[chunk.text];if(t.removed)continue;if(!Same(t,original[chunk.text])){auto bytes=Encode(t);write(bytes.data(),bytes.size());continue;}}
                in.seekg(std::streamoff(chunk.offset));auto count=chunk.size;
                while(count){size_t n=std::min<uint64_t>(count,buffer.size());in.read(reinterpret_cast<char*>(buffer.data()),std::streamsize(n));if(!in)throw std::runtime_error("Cannot copy PNG image data");write(buffer.data(),n);count-=n;}
            }
            if(!FlushFileBuffers(out))throw std::runtime_error("Cannot flush PNG metadata");CloseHandle(out);out=INVALID_HANDLE_VALUE;
        }catch(...){if(out!=INVALID_HANDLE_VALUE)CloseHandle(out);throw;}
        in.close();auto check=Load(temporary.c_str());size_t n=0;for(auto& t:png.text)if(!t.removed){if(n>=check.text.size()||!Same(t,check.text[n++]))throw std::runtime_error("PNG metadata verification failed");}
        if(n!=check.text.size())throw std::runtime_error("PNG text count changed unexpectedly");
        if(!MoveFileExW(temporary.c_str(),path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit PNG working copy");
    }catch(...){DeleteFileW(temporary.c_str());throw;}
}
