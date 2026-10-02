#include "loader.h"

#include <algorithm>

namespace {
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}
}  // namespace

Loader::Loader(const Decoder& decoder, HWND hwnd, UINT message)
    : m_decoder(decoder), m_hwnd(hwnd), m_message(message) {
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    int n = int(std::clamp(hw / 2, 3u, 6u));
    for (int i = 0; i < n; ++i) m_threads.emplace_back(&Loader::Worker, this, i);
}

Loader::~Loader() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        for (auto& f : m_inflight) f.cancel->store(true);
    }
    m_cv.notify_all();
    for (auto& t : m_threads) t.join();
}

bool Loader::IsInFlight(const std::wstring& path, JobKind kind) const {
    for (auto& f : m_inflight)
        if (f.kind == kind && SamePath(f.path, path)) return true;
    return false;
}

void Loader::SetQueue(JobKind kind, const std::vector<std::wstring>& paths) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& q = kind == JobKind::Full ? m_full : kind == JobKind::Refine ? m_refine : m_thumbs;
        q.clear();
        for (auto& p : paths)
            if (!IsInFlight(p, kind)) q.push_back(p);
        for (auto& f : m_inflight) {
            if (f.kind != kind) continue;
            bool wanted = std::any_of(paths.begin(), paths.end(), [&](const std::wstring& p) { return SamePath(p, f.path); });
            if (!wanted) f.cancel->store(true);
        }
    }
    m_cv.notify_all();
}

void Loader::SetFullQueue(const std::vector<std::wstring>& paths) { SetQueue(JobKind::Full, paths); }
void Loader::SetThumbQueue(const std::vector<std::wstring>& paths) { SetQueue(JobKind::Thumb, paths); }
void Loader::SetRefineQueue(const std::vector<std::wstring>& paths) { SetQueue(JobKind::Refine, paths); }

void Loader::Worker(int id) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::wstring path;
        JobKind kind;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] {
                return m_stop || !m_full.empty() || !m_refine.empty() || (id != 0 && !m_thumbs.empty());
            });
            if (m_stop) break;
            if (!m_full.empty()) {
                path = std::move(m_full.front());
                m_full.pop_front();
                kind = JobKind::Full;
            } else if (!m_refine.empty()) {
                path = std::move(m_refine.front());
                m_refine.pop_front();
                kind = JobKind::Refine;
            } else {
                path = std::move(m_thumbs.front());
                m_thumbs.pop_front();
                kind = JobKind::Thumb;
            }
            m_inflight.push_back({path, kind, cancel});
        }

        SetThreadPriority(GetCurrentThread(), kind == JobKind::Thumb ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
        auto* r = new LoadResult;
        r->path = path;
        r->kind = kind;
        r->image = m_decoder.Decode(path, kind == JobKind::Thumb ? m_thumbSize.load() : 0, kind == JobKind::Refine,
                                    cancel.get(), r->error);
        r->cancelled = cancel->load();

        bool stop;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = std::find_if(m_inflight.begin(), m_inflight.end(),
                                   [&](const InFlight& f) { return f.cancel == cancel; });
            if (it != m_inflight.end()) m_inflight.erase(it);
            stop = m_stop;
        }
        if (stop || !PostMessageW(m_hwnd, m_message, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }
    Decoder::ReleaseThreadResources();
    CoUninitialize();
}
