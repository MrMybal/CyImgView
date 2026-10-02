#pragma once

#include "common.h"
#include "cyimg_plugin.h"

#include <unordered_set>

struct PluginInfo {
    std::wstring name, version, file, extensions;
};

// Décodage d'images : plugins d'abord (selon l'extension), puis WIC, puis sondage des plugins.
// Decode() est thread-safe ; le thread appelant doit avoir initialisé COM.
class Decoder {
public:
    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    ~Decoder();

    void Init(const std::wstring& exeDir);

    bool IsSupportedExt(const std::wstring& extLower) const;
    // Extension reconnue ET accepté par le filtre du plugin (ex. .uasset qui est bien une texture)
    bool AcceptsFile(const std::wstring& path) const;
    std::wstring DialogPattern() const;  // "*.jpg;*.png;..."
    const std::vector<PluginInfo>& Plugins() const { return m_pluginInfo; }

    // maxSize = 0 : pleine résolution ; sinon vignette dont le plus grand côté ≤ maxSize.
    // best : version complète d'une image d'abord rendue comme aperçu (DecodedImage::refinable).
    ImagePtr Decode(const std::wstring& path, uint32_t maxSize, bool best, const std::atomic<bool>* cancel,
                    std::wstring& error) const;

    // À appeler par un thread de travail avant CoUninitialize.
    static void ReleaseThreadResources();

private:
    struct Plugin {
        HMODULE module = nullptr;
        const CyImgPlugin* api = nullptr;
        CyImgAcceptFn accept = nullptr;  // export optionnel CyImg_Accept
        std::vector<std::wstring> exts;
    };

    bool TryPlugin(const Plugin& p, const std::wstring& path, uint32_t maxSize, bool best,
                   const std::atomic<bool>* cancel, ImagePtr& out, std::wstring& error) const;

    std::vector<Plugin> m_plugins;
    std::vector<PluginInfo> m_pluginInfo;
    std::unordered_set<std::wstring> m_wicExts;
    std::unordered_set<std::wstring> m_allExts;
};
