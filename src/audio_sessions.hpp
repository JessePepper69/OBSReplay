#pragma once
#include <memory>

namespace replay {
// WebView2 plays sound from its own msedgewebview2.exe child processes, so Windows' volume mixer would list
// the app as "Microsoft Edge WebView2" with the Edge icon. This names those audio sessions after OBSReplay.
class AudioSessionLabel {
public:
    AudioSessionLabel();
    ~AudioSessionLabel();
    AudioSessionLabel(const AudioSessionLabel&) = delete;
    AudioSessionLabel& operator=(const AudioSessionLabel&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
