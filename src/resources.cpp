#include "resources.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace replay {
namespace {
uint64_t ticks(const FILETIME& time) { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }

struct Process { DWORD id{}, parent{}; std::wstring name; unsigned threads{}; int group{}; bool read{}, ended{}; uint64_t created{}, cpu{}, memory{}; };
struct Group { double cpu{}; uint64_t memory{}, gpu_memory{}; unsigned processes{}, threads{}; std::map<std::wstring, double> engines; };
struct Previous { uint64_t created{}, cpu{}; };

// GPU counters come from Windows performance data (the same source as Task Manager), one instance per process and engine.
struct GpuCounters {
    PDH_HQUERY query{};
    PDH_HCOUNTER engine{}, memory{};
    bool ready{};
    GpuCounters() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
        const bool engines = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine) == ERROR_SUCCESS;
        const bool memories = PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &memory) == ERROR_SUCCESS;
        ready = engines && memories;
        if (ready) PdhCollectQueryData(query);
    }
    ~GpuCounters() { if (query) PdhCloseQuery(query); }
    template <class F> void each(PDH_HCOUNTER counter, F&& visit) {
        DWORD size{}, count{};
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA) || !size) return;
        std::vector<unsigned char> buffer(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
        for (DWORD i = 0; i != count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            // Instance names look like "pid_1234_luid_0x..._phys_0_eng_3_engtype_VideoEncode".
            const std::wstring_view name = items[i].szName;
            if (!name.starts_with(L"pid_")) continue;
            const DWORD id = std::wcstoul(items[i].szName + 4, nullptr, 10);
            const auto type = name.find(L"engtype_");
            visit(id, type == std::wstring_view::npos ? std::wstring{} : std::wstring(name.substr(type + 8)), items[i].FmtValue.doubleValue);
        }
    }
};

Json group_json(const Group& group, bool gpu, bool sampled) {
    double busiest{}, encode{};
    for (const auto& [type, share] : group.engines) { busiest = std::max(busiest, share); if (type == L"VideoEncode") encode = share; }
    Json result{{"processes", group.processes}, {"threads", group.threads}, {"cpu", sampled ? std::min(100.0, group.cpu) : 0.0}, {"memory", group.memory}};
    result["gpu"] = gpu && sampled ? Json(std::min(100.0, busiest)) : Json();
    result["gpu_encode"] = gpu && sampled ? Json(std::min(100.0, encode)) : Json();
    result["gpu_memory"] = gpu ? Json(group.gpu_memory) : Json();
    return result;
}
}

Json resource_usage(const fs::path& clips_folder) {
    static std::mutex guard;
    static std::unordered_map<DWORD, Previous> previous;
    static uint64_t previous_wall{}, previous_idle{}, previous_busy{};
    static GpuCounters gpu;
    std::lock_guard lock(guard);

    std::vector<Process> processes;
    if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) do processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile, entry.cntThreads}); while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    const auto read = [](Process& process) {
        if (process.read) return;
        process.read = true;
        // Windows keeps listing a process that has ended while something still holds a handle to it. Those are not counted.
        if (!process.threads) { process.ended = true; return; }
        const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process.id);
        if (!handle) return;
        if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) { process.ended = true; CloseHandle(handle); return; }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(handle, &created, &exited, &kernel, &user)) { process.created = ticks(created); process.cpu = ticks(kernel) + ticks(user); }
        // Private working set: memory only this process holds, the figure Task Manager shows. The plain working set
        // also counts shared pages (the browser engine's code) again in every process, which more than doubles a sum.
        PROCESS_MEMORY_COUNTERS_EX2 counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) process.memory = counters.PrivateWorkingSetSize;
        else {
            // Windows before 11 22H2 has no private figure here; the working set is the closest it offers.
            PROCESS_MEMORY_COUNTERS basic{};
            basic.cb = sizeof(basic);
            if (GetProcessMemoryInfo(handle, &basic, sizeof(basic))) process.memory = basic.WorkingSetSize;
        }
        CloseHandle(handle);
    };
    // Groups: 1 = OBSReplay, 2 = OBS Studio. OBS is marked first: OBSReplay starts it, and it is still OBS.
    const DWORD self = GetCurrentProcessId();
    for (auto& process : processes) {
        if (_wcsicmp(process.name.c_str(), L"obs64.exe") == 0) process.group = 2;
        else if (process.id == self) process.group = 1;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& child : processes) {
            if (child.group || !child.id) continue;
            const auto parent = std::find_if(processes.begin(), processes.end(), [&](const Process& p) { return p.id == child.parent && p.group; });
            if (parent == processes.end()) continue;
            // A process id is reused after its process ends: a real child was created after its parent.
            read(child); read(*parent);
            if (!child.created || !parent->created || child.created < parent->created) continue;
            child.group = parent->group;
            changed = true;
        }
    }

    FILETIME now_time{}, idle{}, kernel{}, user{};
    GetSystemTimeAsFileTime(&now_time);
    GetSystemTimes(&idle, &kernel, &user);
    const uint64_t wall = ticks(now_time), idle_now = ticks(idle), busy_now = ticks(kernel) + ticks(user);
    const unsigned cores = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const uint64_t elapsed = previous_wall && wall > previous_wall ? wall - previous_wall : 0;
    const bool sampled = elapsed >= 2'000'000; // 200 ms: shorter gaps give noisy shares

    Group groups[3];
    std::unordered_map<DWORD, Previous> current;
    std::unordered_map<DWORD, int> group_of;
    for (auto& process : processes) {
        if (!process.group) continue;
        read(process);
        if (process.ended) continue;
        auto& group = groups[process.group];
        ++group.processes;
        group.threads += process.threads;
        group.memory += process.memory;
        group_of[process.id] = process.group;
        current[process.id] = {process.created, process.cpu};
        const auto before = previous.find(process.id);
        if (sampled && before != previous.end() && before->second.created == process.created && process.cpu >= before->second.cpu)
            group.cpu += 100.0 * static_cast<double>(process.cpu - before->second.cpu) / static_cast<double>(elapsed) / cores;
    }
    if (gpu.ready) {
        PdhCollectQueryData(gpu.query);
        gpu.each(gpu.engine, [&](DWORD id, const std::wstring& type, double value) { if (const auto found = group_of.find(id); found != group_of.end()) groups[found->second].engines[type] += value; });
        gpu.each(gpu.memory, [&](DWORD id, const std::wstring&, double value) { if (const auto found = group_of.find(id); found != group_of.end()) groups[found->second].gpu_memory += static_cast<uint64_t>(value); });
    }
    double system_cpu{};
    if (sampled && busy_now > previous_busy) {
        // Kernel time includes idle time.
        const double total = static_cast<double>(busy_now - previous_busy), idle_share = static_cast<double>(idle_now - previous_idle);
        system_cpu = std::clamp(100.0 * (total - idle_share) / total, 0.0, 100.0);
    }
    if (sampled || !previous_wall) { previous = std::move(current); previous_wall = wall; previous_idle = idle_now; previous_busy = busy_now; }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    Json disk;
    if (ULARGE_INTEGER free{}, total{}; !clips_folder.empty() && GetDiskFreeSpaceExW(clips_folder.root_path().c_str(), &free, &total, nullptr))
        disk = {{"free", free.QuadPart}, {"total", total.QuadPart}, {"drive", path_string(clips_folder.root_path())}};
    Json obs = group_json(groups[2], gpu.ready, sampled);
    obs["running"] = groups[2].processes != 0;
    return {{"sampled", sampled}, {"cores", cores}, {"gpu_available", gpu.ready}, {"app", group_json(groups[1], gpu.ready, sampled)}, {"obs", obs},
            {"system", {{"cpu", system_cpu}, {"memory_used", memory.ullTotalPhys - memory.ullAvailPhys}, {"memory_total", memory.ullTotalPhys}}}, {"disk", disk}};
}
}
