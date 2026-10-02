#pragma once

#include "common.h"
#include "decoder.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Full : image à afficher (un aperçu est accepté) ; Refine : version complète d'un aperçu ;
// Thumb : vignette.
enum class JobKind : uint8_t { Full, Refine, Thumb };

struct LoadResult {
    std::wstring path;
    JobKind kind = JobKind::Full;
    ImagePtr image;
    std::wstring error;
    bool cancelled = false;
};

// Pool de threads de décodage. Les résultats sont postés à la fenêtre (lParam = LoadResult*,
// à libérer par le destinataire). Priorités : Full, puis Refine, puis Thumb. Le thread 0 ne prend
// jamais de vignette, pour que l'image courante ne fasse pas la queue derrière.
class Loader {
public:
    Loader(const Decoder& decoder, HWND hwnd, UINT message);
    ~Loader();

    void SetThumbSize(uint32_t size) { m_thumbSize = size; }
    // Remplace la file d'attente (ordre = priorité). Les travaux en cours qui ne sont plus
    // demandés reçoivent un signal d'annulation.
    void SetFullQueue(const std::vector<std::wstring>& paths);
    void SetThumbQueue(const std::vector<std::wstring>& paths);
    void SetRefineQueue(const std::vector<std::wstring>& paths);

private:
    struct InFlight {
        std::wstring path;
        JobKind kind;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    void Worker(int id);
    void SetQueue(JobKind kind, const std::vector<std::wstring>& paths);
    bool IsInFlight(const std::wstring& path, JobKind kind) const;

    const Decoder& m_decoder;
    HWND m_hwnd;
    UINT m_message;
    std::atomic<uint32_t> m_thumbSize{160};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::wstring> m_full, m_refine, m_thumbs;
    std::vector<InFlight> m_inflight;
    bool m_stop = false;
    std::vector<std::thread> m_threads;
};
