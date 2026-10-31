// Native editing document. Explicit fields and bounded lengths; no serialized C++ pointers/padding.
#include "viewer.h"
#include "viewer_modes.h"
#include "i18n.h"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <stdexcept>

namespace {
struct ProjectIO {
    std::istream* input = nullptr;
    std::ostream* output = nullptr;
    void Data(void* ptr, size_t bytes) {
        if (output) { output->write(static_cast<const char*>(ptr), std::streamsize(bytes)); if (!*output) throw std::runtime_error("write"); }
        else { input->read(static_cast<char*>(ptr), std::streamsize(bytes)); if (!*input) throw std::runtime_error("read"); }
    }
    template<class T> void Value(T& value) { Data(&value, sizeof(value)); }
    void Text(std::wstring& text) {
        uint32_t length = uint32_t(text.size()); Value(length);
        if (length > 65536) throw std::runtime_error("text");
        if (input) text.resize(length);
        Data(text.data(), size_t(length) * sizeof(wchar_t));
    }
};
}

bool Viewer::WriteProject(const std::wstring& path, std::wstring& error) {
    if (m_edit.layers.empty() || m_edit.layers.size() > 256 || uint64_t(m_edit.w) * m_edit.h * m_edit.layers.size() > 120000000) {
        error = T("edit.err.layer_budget"); return false;
    }
    const std::wstring temporary = path + L".cyimg-tmp";
    try {
        std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        ProjectIO io{nullptr, &file};
        uint32_t magic = 0x31455943, w = m_edit.w, h = m_edit.h, count = uint32_t(m_edit.layers.size()), active = uint32_t(m_edit.layer);
        io.Value(magic); io.Value(w); io.Value(h); io.Value(count); io.Value(active);
        for (const Layer& source : m_edit.layers) {
            Layer l = source;
            io.Text(l.name);
            uint32_t flags = (l.visible ? 1u : 0u) | (l.object ? 2u : 0u) | (l.mask ? 4u : 0u) | (l.maskEnabled ? 8u : 0u);
            uint32_t tool = uint32_t(l.objectTool), fill = l.objectFill;
            io.Value(flags); io.Value(l.opacity); io.Value(l.blend); io.Value(tool);
            io.Value(l.a.x); io.Value(l.a.y); io.Value(l.b.x); io.Value(l.b.y);
            io.Value(l.objectColor); io.Value(l.objectSize); io.Value(l.objectOpacity); io.Value(fill);
            io.Text(l.objectText);
            io.Value(l.objectTransform._11); io.Value(l.objectTransform._12); io.Value(l.objectTransform._21);
            io.Value(l.objectTransform._22); io.Value(l.objectTransform._31); io.Value(l.objectTransform._32);
            PixelBuffer pixels; uint32_t stride;
            if (!ReadBitmap(l.bmp.Get(), pixels, stride)) throw std::runtime_error("pixels");
            io.Data(pixels.Data(), size_t(w) * h * 4);
            if (l.mask) io.Data(l.mask->data(), size_t(w) * h);
        }
        file.flush(); if (!file) throw std::runtime_error("flush"); file.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("replace");
        return true;
    } catch (...) {
        DeleteFileW(temporary.c_str()); error = T("save.err.cannot_write"); return false;
    }
}

bool Viewer::OpenProject(const std::wstring& path) {
    try {
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        ProjectIO io{&file, nullptr};
        uint32_t magic = 0, w = 0, h = 0, count = 0, active = 0;
        io.Value(magic); io.Value(w); io.Value(h); io.Value(count); io.Value(active);
        if (magic != 0x31455943 || !w || !h || w > 16384 || h > 16384 || !count || count > 256 || active >= count ||
            uint64_t(w) * h * count > 120000000 || !CreateDeviceResources() || std::max(w, h) > m_dc->GetMaximumBitmapSize())
            throw std::runtime_error("header");
        EditState fresh; fresh.active = fresh.untitled = true; fresh.hasAlpha = true; fresh.w = w; fresh.h = h; fresh.layer = int(active);
        for (uint32_t i = 0; i < count; ++i) {
            Layer l; uint32_t flags = 0, tool = 0, fill = 0;
            io.Text(l.name); io.Value(flags); io.Value(l.opacity); io.Value(l.blend); io.Value(tool);
            io.Value(l.a.x); io.Value(l.a.y); io.Value(l.b.x); io.Value(l.b.y);
            io.Value(l.objectColor); io.Value(l.objectSize); io.Value(l.objectOpacity); io.Value(fill);
            io.Text(l.objectText);
            io.Value(l.objectTransform._11); io.Value(l.objectTransform._12); io.Value(l.objectTransform._21);
            io.Value(l.objectTransform._22); io.Value(l.objectTransform._31); io.Value(l.objectTransform._32);
            for (float f : {l.opacity, l.a.x, l.a.y, l.b.x, l.b.y, l.objectSize, l.objectOpacity,
                l.objectTransform._11, l.objectTransform._12, l.objectTransform._21, l.objectTransform._22,
                l.objectTransform._31, l.objectTransform._32})
                if (!std::isfinite(f) || std::fabs(f) > 1000000) throw std::runtime_error("float");
            if (l.opacity < 0 || l.opacity > 1 || l.objectOpacity < 0 || l.objectOpacity > 1 || l.objectSize <= 0 ||
                l.blend < 0 || l.blend > 7 || tool < uint32_t(Tool::Line) || tool > uint32_t(Tool::Text) || flags > 15 || fill > 1)
                throw std::runtime_error("layer");
            l.visible = flags & 1; l.object = flags & 2; l.maskEnabled = flags & 8;
            l.objectTool = Tool(tool); l.objectFill = fill != 0;
            PixelBuffer pixels;
            if (!pixels.Alloc(size_t(w) * h * 4)) throw std::bad_alloc();
            io.Data(pixels.Data(), size_t(w) * h * 4);
            l.bmp = NewCanvas(w, h);
            if (!l.bmp || FAILED(l.bmp->CopyFromMemory(nullptr, pixels.Data(), w * 4))) throw std::bad_alloc();
            if (flags & 4) {
                l.mask = std::make_shared<std::vector<uint8_t>>(size_t(w) * h);
                io.Data(l.mask->data(), size_t(w) * h);
            }
            l.num = int(i); l.ver = ++fresh.verSeq;
            fresh.layers.push_back(std::move(l));
        }
        if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("trailing");
        if (!EndEdit(true)) return false;
        EnsureEditPlugins(); InitEditModes();
        m_edit = std::move(fresh); m_edit.layerSeq = int(count); m_mode = 0;
        m_projectPath = path; m_userRot = 0;
        ApplyFit(false); Invalidate(); return true;
    } catch (...) {
        ShowToast(T("document.project.error"), 5); return false;
    }
}
