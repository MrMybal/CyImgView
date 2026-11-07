#pragma once
#include "common.h"
#include "cyimg_metadata_plugin.h"
#include <array>
#include <mutex>

namespace metadata {
struct Field {
    std::wstring provider, key, label, group, type, value, description;
    uint32_t occurrence = 0, flags = 0;
    bool Editable() const { return (flags & CYMETA_EDITABLE) != 0; }
};
struct Edit {
    std::wstring provider, key, type, value;
    uint32_t occurrence = 0, operation = CYMETA_SET;
};
struct Provider {
    std::wstring id, name, version, file, error;
    std::vector<std::wstring> writableGroups;
};
struct Document {
    std::wstring path, error;
    std::vector<Field> fields;
    std::vector<Provider> providers;
    std::array<uint8_t, 32> fingerprint{};
    bool valid = false;
};
class Engine {
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    void Init(const std::wstring& exeDir);
    Document Read(const std::wstring& path, const std::atomic<bool>* cancel = nullptr);
    // Empty destination means overwrite source atomically, retaining a backup.
    // A destination must have the same extension; this never re-encodes pixels.
    bool Save(const Document&, const std::vector<Edit>&, const std::wstring& destination,
              std::wstring& backup, std::wstring& error, const std::atomic<bool>* cancel = nullptr);
    const std::vector<Provider>& Providers() const { return m_info; }
private:
    struct Plugin { HMODULE module; CyImgMetadataPlugin api; Provider info; bool disabled = false; };
    std::vector<Plugin> m_plugins;
    std::vector<Provider> m_info;
    std::mutex m_mutex;
};
bool ExportJson(const Document&, const std::wstring& path, std::wstring& error);
// Strict UTF-8 conversions shared by export and the UI.
std::string Utf8(const std::wstring&);
std::wstring Wide(const std::string&);
} // namespace metadata
