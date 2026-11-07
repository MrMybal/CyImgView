#pragma once
#include <exiv2/exiv2.hpp>
#include <windows.h>
#include <algorithm>
#include <string>

// Exiv2's narrow FileIo uses the Windows ANSI code page. This adapter keeps
// paths UTF-16, uses bounded native I/O and maps large files without copying pixels.
class MetadataWindowsIo final : public Exiv2::BasicIo {
    std::wstring filename;std::string diagnostic;bool writable,failed=false,atEnd=false;
    HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;bool mapWritable=false;
public:
    MetadataWindowsIo(const wchar_t* path,bool write):filename(path),writable(write) {
        int n=WideCharToMultiByte(CP_UTF8,0,path,-1,nullptr,0,nullptr,nullptr);
        diagnostic.resize(n);WideCharToMultiByte(CP_UTF8,0,path,-1,diagnostic.data(),n,nullptr,nullptr);diagnostic.pop_back();
        if(filename.size()>240&&filename.rfind(L"\\\\?\\",0)!=0)filename=filename.rfind(L"\\\\",0)==0?L"\\\\?\\UNC\\"+filename.substr(2):L"\\\\?\\"+filename;
    }
    ~MetadataWindowsIo() override {close();}
    int open() override {
        close();failed=atEnd=false;
        file=CreateFileW(filename.c_str(),GENERIC_READ|(writable?GENERIC_WRITE:0),FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        failed=file==INVALID_HANDLE_VALUE;return failed?1:0;
    }
    int close() override {munmap();if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}return 0;}
    size_t write(const Exiv2::byte* data,size_t count) override {
        if(!writable||!isopen()){failed=true;return 0;}munmap();size_t done=0;
        while(done<count){DWORD n=0;if(!WriteFile(file,data+done,DWORD(std::min<size_t>(count-done,1<<20)),&n,nullptr)||!n){failed=true;break;}done+=n;}return done;
    }
    size_t write(BasicIo& src) override {size_t total=0;Exiv2::byte buffer[65536];while(!src.eof()){auto n=src.read(buffer,sizeof(buffer));if(!n)break;auto put=write(buffer,n);total+=put;if(put!=n)break;}return total;}
    int putb(Exiv2::byte b) override {return write(&b,1)==1?b:EOF;}
    Exiv2::DataBuf read(size_t count) override {count=std::min(count,size()-std::min(size(),tell()));Exiv2::DataBuf out(count);auto n=read(out.data(),count);out.resize(n);return out;}
    size_t read(Exiv2::byte* data,size_t count) override {
        size_t done=0;if(!isopen()){failed=true;return 0;}
        while(done<count){DWORD n=0;if(!ReadFile(file,data+done,DWORD(std::min<size_t>(count-done,1<<20)),&n,nullptr)){failed=true;break;}if(!n){atEnd=true;break;}done+=n;}return done;
    }
    int getb() override {Exiv2::byte b=0;return read(&b,1)==1?b:EOF;}
    void transfer(BasicIo& src) override {
        if(!writable)throw std::runtime_error("Metadata source is read-only");
        if(!isopen()&&open())throw std::runtime_error("Cannot reopen metadata working copy");
        munmap();if(seek(0,beg)||!SetEndOfFile(file))throw std::runtime_error("Cannot truncate metadata working copy");
        if(src.seek(0,beg))throw std::runtime_error("Cannot rewind metadata output");
        auto n=write(src);if(n!=src.size()||src.error()||failed)throw std::runtime_error("Incomplete metadata transfer");
        if(!FlushFileBuffers(file))throw std::runtime_error("Cannot flush metadata output");
    }
    int seek(int64_t offset,Position origin) override {
        LARGE_INTEGER move;move.QuadPart=offset;atEnd=false;
        if(!SetFilePointerEx(file,move,nullptr,origin==beg?FILE_BEGIN:origin==cur?FILE_CURRENT:FILE_END)){failed=true;return 1;}return 0;
    }
    Exiv2::byte* mmap(bool write=false) override {
        if(bigBlock_&&(!write||mapWritable))return bigBlock_;munmap();
        if(write&&!writable)throw std::runtime_error("Cannot map source for writing");
        mapping=CreateFileMappingW(file,nullptr,write?PAGE_READWRITE:PAGE_READONLY,0,0,nullptr);
        if(mapping)bigBlock_=static_cast<Exiv2::byte*>(MapViewOfFile(mapping,write?FILE_MAP_WRITE:FILE_MAP_READ,0,0,0));
        if(!bigBlock_)throw std::runtime_error("Cannot map metadata file");mapWritable=write;return bigBlock_;
    }
    int munmap() override {
        bool ok=true;if(bigBlock_){if(mapWritable)ok=FlushViewOfFile(bigBlock_,0)!=FALSE;UnmapViewOfFile(bigBlock_);bigBlock_=nullptr;}
        if(mapping){CloseHandle(mapping);mapping=nullptr;}mapWritable=false;return ok?0:1;
    }
    size_t tell() const override {LARGE_INTEGER zero{},pos{};return SetFilePointerEx(file,zero,&pos,FILE_CURRENT)?size_t(pos.QuadPart):0;}
    size_t size() const override {LARGE_INTEGER n{};if(file!=INVALID_HANDLE_VALUE&&GetFileSizeEx(file,&n))return size_t(n.QuadPart);WIN32_FILE_ATTRIBUTE_DATA a{};return GetFileAttributesExW(filename.c_str(),GetFileExInfoStandard,&a)?size_t((uint64_t(a.nFileSizeHigh)<<32)|a.nFileSizeLow):0;}
    bool isopen() const override{return file!=INVALID_HANDLE_VALUE;}
    int error() const override{return failed?1:0;}
    bool eof() const override{return atEnd;}
    const std::string& path() const noexcept override{return diagnostic;}
    void populateFakeData() override{}
};
