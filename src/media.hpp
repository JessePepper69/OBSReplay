#pragma once
#include "controller.hpp"
#include <windows.h>
#include <objidl.h>
#include <WebView2.h>
#include <wrl/client.h>
#include <map>

namespace replay {
class ClipMedia {
public:
    ClipMedia(ICoreWebView2* view, Controller& controller);
    ~ClipMedia();
    std::expected<std::string, std::string> url(const std::string& path);
private:
    HRESULT respond(ICoreWebView2WebResourceRequestedEventArgs* args);
    Microsoft::WRL::ComPtr<ICoreWebView2> view_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Controller& controller_;
    EventRegistrationToken handler_{};
    std::map<std::string, fs::path> sources_;
};
}
