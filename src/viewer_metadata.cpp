#include "viewer.h"
#include "metadata_dialog.h"
#include "i18n.h"

void Viewer::ShowMetadata() {
    const auto* file=Current();
    if(!file || (m_edit.active&&(m_edit.untitled||!m_projectPath.empty()))) {ShowToast(T("metadata.need_file"));return;}
    const auto path=file->path;
    m_showSettings=m_showChannels=false;
    std::vector<std::pair<std::wstring,std::wstring>> decoderInfo;
    if(m_cur) {
        decoderInfo=m_cur->info;
        decoderInfo.emplace_back(T("viewer.info.format"),m_cur->format);
        decoderInfo.emplace_back(T("viewer.info.dimensions"),std::to_wstring(m_cur->fullWidth?m_cur->fullWidth:m_cur->width)+L" × "+std::to_wstring(m_cur->fullHeight?m_cur->fullHeight:m_cur->height));
    }
    if(!metadata::ShowDialog(m_hwnd,m_exeDir,path,decoderInfo))return;
    // Discard in-flight results before reopening: orientation and thumbnails may change.
    m_loader.reset();
    MSG pending{};
    while(PeekMessageW(&pending,m_hwnd,WM_APP+1,WM_APP+1,PM_REMOVE))delete reinterpret_cast<LoadResult*>(pending.lParam);
    m_fullCache.clear();m_thumbCache.clear();m_failed.clear();m_cur.reset();m_placeholder.reset();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&fa)&&m_index>=0) {
        auto& entry=m_files[m_index];entry.size=(uint64_t(fa.nFileSizeHigh)<<32)|fa.nFileSizeLow;entry.modified=fa.ftLastWriteTime;
    }
    // WM_APP + 1 is the viewer's image-loaded message.
    m_loader=std::make_unique<Loader>(m_decoder,m_hwnd,WM_APP+1);
    m_loading=true;m_error.clear();m_loadStart=NowSeconds();
    RequestLoads();Invalidate();
}
