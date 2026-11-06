#include "viewer.h"
#include "viewer_util.h"
#include "viewer_modes.h"
#include "edit_plugins.h"
#include "i18n.h"
#include "help.h"
#include <algorithm>

using namespace ui;
cyhelp::Topic Viewer::HelpTopic() const {
    if (!m_edit.active) {
        if (m_showSettings) return {"settings","settings"};
        if (m_slideshow) return {"viewer","animation"};
        if (m_showChannels) return {"viewer","channels"};
        if (m_showInfo) return {"viewer","information"};
        return {"viewer","navigation"};
    }
    if (m_edit.textActive) return {"edit","objects"};
    if (m_mode==1) return {"develop","adjust"};
    if (m_mode>=2) {
        const auto& modes=editplug::Get().modes;
        const auto index=static_cast<size_t>(m_mode-2);
        const bool blackWhite=index<modes.size() && modes[index].plugin && modes[index].plugin->api.id &&
            std::string(modes[index].plugin->api.id)=="bw_mode";
        return blackWhite ? cyhelp::Topic{"black-white","cancel"} : cyhelp::Topic{"plugins","modes"};
    }
    if (m_edit.menu==EditMenu::Effects) return {"effects","gallery"};
    if (m_edit.fx>=0) return {"effects","apply"};
    if (m_edit.editMask) return {"layers","masks"};
    if (const auto* button=HitEdit(m_mouse)) {
        if (button->cmd==EditCmd::LayerSelect || button->cmd==EditCmd::LayerEye || button->cmd==EditCmd::LayerNew ||
            button->cmd==EditCmd::LayerDup || button->cmd==EditCmd::LayerDel || button->cmd==EditCmd::BlendMenu) return {"layers","layers"};
    }
    switch (m_edit.tool) {
    case Tool::Text:case Tool::Move:case Tool::Line:case Tool::Arrow:case Tool::Rect:case Tool::Ellipse:return {"edit","objects"};
    case Tool::Brush:case Tool::Eraser:case Tool::Fill:return {"edit","paint"};
    case Tool::Gradient:return {"edit","gradient"};
    default:return {"edit","tools"};
    }
}
void Viewer::OpenHelp(const cyhelp::Topic& topic) {
    const auto target=topic.page.empty()?HelpTopic():topic;
    const auto url=cyhelp::Url(m_exeDir,i18n::CurrentCode(),target);
    if (m_helpLauncher) { m_helpLauncher(url); return; }
    if (!cyhelp::Open(m_hwnd,m_exeDir,i18n::CurrentCode(),target)) ShowToast(T("help.manual.unavailable"),6);
}
void Viewer::ShowInstalledPlugins() {
    EnsureEditPlugins();
    const auto& decoders=m_decoder.Plugins();
    std::wstring message=Tf("viewer.help.plugins",int(decoders.size()))+L"\n";
    for (const auto& plugin:decoders) {
        std::string key="pluginname.";
        const auto name=FileNameOf(plugin.file);
        for (wchar_t c:name.substr(0,name.find_last_of(L'.'))) key+=char(towlower(c));
        auto extensions=plugin.extensions;std::replace(extensions.begin(),extensions.end(),L';',L' ');
        message+=L"\n"+std::wstring(TrOr(key.c_str(),plugin.name.c_str()))+L" — "+extensions;
    }
    const auto& registry=editplug::Get();
    message+=L"\n\n"+Tf("editapi.help.title",int(registry.plugins.size()))+L"\n";
    for (const auto& plugin:registry.plugins) {
        message+=L"\n"+plugin->Name()+L" — ";
        auto append=[&](const auto& entries,const char* key) {
            int count=0;for(const auto& entry:entries)if(entry.plugin==plugin.get())++count;
            if(count) message+=Tf(key,count)+L"  ";
        };
        append(registry.effects,"editapi.help.effects");append(registry.tools,"editapi.help.tools");
        append(registry.modes,"editapi.help.modes");append(registry.actions,"editapi.help.actions");
    }
    MessageBoxW(m_hwnd,message.c_str(),T("help.plugins.title"),MB_OK|MB_ICONINFORMATION);
}
