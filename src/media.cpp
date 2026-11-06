#include "media.hpp"
#include <wrl.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace replay {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto origin = "https://replay.obsreplay.invalid/";

class FileRange final : public IStream {
public:
    FileRange(HANDLE file, fs::path path, ULONGLONG first, ULONGLONG size)
        : file_(file), path_(std::move(path)), first_(first), size_(size) {}
    ~FileRange() { CloseHandle(file_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id == IID_IUnknown || id == IID_IStream || id == IID_ISequentialStream) {
            *value = static_cast<IStream*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Read(void* target, ULONG requested, ULONG* count) override {
        if (count) *count = 0;
        if (!target && requested) return STG_E_INVALIDPOINTER;
        std::lock_guard lock(mutex_);
        const auto bytes = static_cast<DWORD>(std::min<ULONGLONG>(requested, size_ - position_));
        if (!bytes) return requested ? S_FALSE : S_OK;
        LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(first_ + position_);
        if (!SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN)) return HRESULT_FROM_WIN32(GetLastError());
        DWORD read = 0;
        if (!ReadFile(file_, target, bytes, &read, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        position_ += read;
        if (count) *count = read;
        return read == requested ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER delta, DWORD seek_origin, ULARGE_INTEGER* result) override {
        std::lock_guard lock(mutex_);
        const auto base = seek_origin == STREAM_SEEK_SET ? 0 : seek_origin == STREAM_SEEK_CUR ? position_ : size_;
        if (seek_origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        if (delta.QuadPart < 0) {
            const auto magnitude = static_cast<ULONGLONG>(-(delta.QuadPart + 1)) + 1;
            if (magnitude > base) return STG_E_INVALIDFUNCTION;
            position_ = base - magnitude;
        } else {
            if (static_cast<ULONGLONG>(delta.QuadPart) > size_ - base) return STG_E_INVALIDFUNCTION;
            position_ = base + static_cast<ULONGLONG>(delta.QuadPart);
        }
        if (result) result->QuadPart = position_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* value, DWORD) override {
        if (!value) return E_POINTER;
        *value = {}; value->type = STGTY_STREAM; value->cbSize.QuadPart = size_; value->grfMode = STGM_READ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        const auto file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        auto* clone = new FileRange(file, path_, first_, size_);
        { std::lock_guard lock(mutex_); clone->position_ = position_; }
        *result = clone; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
private:
    std::atomic<ULONG> references_{1};
    HANDLE file_;
    fs::path path_;
    ULONGLONG first_, size_, position_{};
    std::mutex mutex_;
};

bool range_bounds(const std::wstring& header, ULONGLONG total, ULONGLONG& first, ULONGLONG& last) {
    if (!header.starts_with(L"bytes=") || header.find(L',') != std::wstring::npos) return false;
    const auto range = header.substr(6);
    const auto dash = range.find(L'-');
    if (dash == std::wstring::npos) return false;
    const auto parse = [](const std::wstring& number) {
        if (number.empty() || number.find_first_not_of(L"0123456789") != std::wstring::npos) throw std::invalid_argument("range");
        return std::stoull(number);
    };
    try {
        if (!dash) {
            const auto suffix = parse(range.substr(1));
            if (!suffix) return false;
            first = total - std::min<ULONGLONG>(total, suffix); last = total - 1;
        } else {
            first = parse(range.substr(0, dash));
            last = dash + 1 == range.size() ? total - 1 : std::min<ULONGLONG>(parse(range.substr(dash + 1)), total - 1);
        }
        return first <= last && first < total;
    } catch (...) { return false; }
}
}

ClipMedia::ClipMedia(ICoreWebView2* view, Controller& controller) : view_(view), controller_(controller) {
    ComPtr<ICoreWebView2_2> newer;
    if (FAILED(view_->QueryInterface(IID_PPV_ARGS(&newer))) || FAILED(newer->get_Environment(&environment_)))
        throw std::runtime_error("The replay player could not access WebView2.");
    if (FAILED(view_->AddWebResourceRequestedFilter(L"https://replay.obsreplay.invalid/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL)) ||
        FAILED(view_->add_WebResourceRequested(Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) { return respond(args); }).Get(), &handler_)))
        throw std::runtime_error("The replay player could not register its media source.");
}
ClipMedia::~ClipMedia() {
    view_->remove_WebResourceRequested(handler_);
    view_->RemoveWebResourceRequestedFilter(L"https://replay.obsreplay.invalid/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
}
std::expected<std::string, std::string> ClipMedia::url(const std::string& path) {
    if (!controller_.knows_clip(path)) return std::unexpected("This replay is no longer in your recent clips.");
    for (auto it = sources_.begin(); it != sources_.end();) {
        if (!controller_.knows_clip(path_string(it->second))) it = sources_.erase(it);
        else { if (path_string(it->second) == path) return it->first; ++it; }
    }
    auto url = std::string(origin) + random_secret();
    sources_.emplace(url, from_string(path));
    return url;
}
HRESULT ClipMedia::respond(ICoreWebView2WebResourceRequestedEventArgs* args) {
    const auto reply = [&](int status, const wchar_t* reason, const std::wstring& extra, IStream* stream = nullptr) {
        ComPtr<ICoreWebView2WebResourceResponse> response;
        const auto headers = L"Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nAccess-Control-Allow-Headers: Range\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n" + extra;
        const auto result = environment_->CreateWebResourceResponse(stream, status, reason, headers.c_str(), &response);
        return FAILED(result) ? result : args->put_Response(response.Get());
    };
    try {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        if (FAILED(args->get_Request(&request))) return E_FAIL;
        LPWSTR raw = nullptr;
        request->get_Uri(&raw); const auto url = raw ? utf8(raw) : ""; CoTaskMemFree(raw);
        raw = nullptr;
        if (!url.starts_with(origin)) return S_OK;
        const auto source = sources_.find(url);
        if (source == sources_.end() || !controller_.knows_clip(path_string(source->second))) return reply(404, L"Not Found", L"");
        request->get_Method(&raw); const std::wstring method = raw ? raw : L""; CoTaskMemFree(raw);
        raw = nullptr;
        if (method == L"OPTIONS") return reply(204, L"No Content", L"");
        if (method != L"GET" && method != L"HEAD") return reply(405, L"Method Not Allowed", L"");
        const auto file = CreateFileW(source->second.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return reply(404, L"Not Found", L"");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) { CloseHandle(file); return reply(404, L"Not Found", L""); }
        const auto total = static_cast<ULONGLONG>(size.QuadPart);
        ULONGLONG first = 0, last = total - 1;
        ComPtr<ICoreWebView2HttpRequestHeaders> headers;
        request->get_Headers(&headers);
        BOOL has_range = FALSE; if (headers) headers->Contains(L"Range", &has_range);
        if (has_range) {
            headers->GetHeader(L"Range", &raw); const std::wstring range = raw ? raw : L""; CoTaskMemFree(raw);
            if (!range_bounds(range, total, first, last)) { CloseHandle(file); return reply(416, L"Range Not Satisfiable", L"Content-Range: bytes */" + std::to_wstring(total) + L"\r\n"); }
        }
        auto extension = source->second.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const auto mime = extension == L".mp4" || extension == L".mov" ? L"video/mp4" : L"video/x-matroska";
        std::wstring extra = std::wstring(L"Content-Type: ") + mime + L"\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_wstring(last - first + 1) + L"\r\n";
        if (has_range) extra += L"Content-Range: bytes " + std::to_wstring(first) + L"-" + std::to_wstring(last) + L"/" + std::to_wstring(total) + L"\r\n";
        ComPtr<IStream> stream;
        if (method == L"HEAD") CloseHandle(file);
        else stream.Attach(new FileRange(file, source->second, first, last - first + 1));
        return reply(has_range ? 206 : 200, has_range ? L"Partial Content" : L"OK", extra, stream.Get());
    } catch (...) { return reply(500, L"Media Error", L""); }
}
}
