#include "audio_sessions.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace replay {
namespace {
using Microsoft::WRL::ComPtr;

// This process and every process it started, directly or through WebView2's browser process.
std::unordered_set<DWORD> own_processes() {
    std::unordered_set<DWORD> result{GetCurrentProcessId()};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    std::unordered_map<DWORD, DWORD> parents;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do parents[entry.th32ProcessID] = entry.th32ParentProcessID; while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    // Parent IDs can be reused after a parent exits, so the walk is bounded and only follows live entries.
    for (const auto& [id, parent] : parents) {
        DWORD current = parent;
        for (int depth = 0; depth < 8 && current && parents.contains(current); ++depth) {
            if (result.contains(current)) { result.insert(id); break; }
            current = parents[current];
        }
    }
    return result;
}

bool equal(LPWSTR value, const std::wstring& expected) {
    const bool same = value && expected == value;
    if (value) CoTaskMemFree(value);
    return same;
}
}

struct AudioSessionLabel::Impl {
    std::wstring name = L"OBSReplay";
    std::wstring icon;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;

    Impl() {
        std::wstring exe(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        exe.resize(length < exe.size() ? length : 0);
        // Icon resource 1 (resources/app.rc.in), addressed by ID as the volume mixer expects.
        if (!exe.empty()) icon = exe + L",-1";
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void label(IMMDeviceEnumerator* devices) {
        ComPtr<IMMDeviceCollection> endpoints;
        if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints))) return;
        UINT count = 0;
        endpoints->GetCount(&count);
        std::unordered_set<DWORD> owned;
        bool listed = false;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            ComPtr<IAudioSessionManager2> manager;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(endpoints->Item(i, &device)) ||
                FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) ||
                FAILED(manager->GetSessionEnumerator(&sessions))) continue;
            int total = 0;
            sessions->GetCount(&total);
            for (int s = 0; s < total; ++s) {
                ComPtr<IAudioSessionControl> control;
                ComPtr<IAudioSessionControl2> details;
                DWORD process = 0;
                if (FAILED(sessions->GetSession(s, &control)) || FAILED(control.As(&details)) || details->GetProcessId(&process) != S_OK || !process) continue;
                if (!listed) { owned = own_processes(); listed = true; }
                if (process == GetCurrentProcessId() || !owned.contains(process)) continue;
                LPWSTR current = nullptr;
                if (SUCCEEDED(control->GetDisplayName(&current)) && !equal(current, name)) control->SetDisplayName(name.c_str(), nullptr);
                current = nullptr;
                if (!icon.empty() && SUCCEEDED(control->GetIconPath(&current)) && !equal(current, icon)) control->SetIconPath(icon.c_str(), nullptr);
            }
        }
    }

    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IMMDeviceEnumerator> devices;
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
            std::unique_lock lock(mutex);
            while (!stopping) {
                lock.unlock();
                // A session appears when the page first plays sound and again after an output device change,
                // so the endpoints are checked every second; already-named sessions are left untouched.
                if (devices) try { label(devices.Get()); } catch (...) {}
                lock.lock();
                wake.wait_for(lock, std::chrono::seconds(1), [&] { return stopping; });
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

AudioSessionLabel::AudioSessionLabel() : impl_(std::make_unique<Impl>()) {}
AudioSessionLabel::~AudioSessionLabel() = default;
}
