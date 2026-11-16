#pragma once
#include "platform.hpp"

namespace replay {
// What OBSReplay and OBS Studio are using right now, for the Debug tab and exported logs.
// Each group is a process and everything it started: OBSReplay with its WebView2 and FFmpeg processes,
// OBS Studio with its helpers. CPU is a share of the whole processor (all cores = 100) since the previous call,
// so the first call reports "sampled": false and zero CPU. GPU figures are null where Windows has no counters.
// `clips_folder` picks the drive whose free space is reported.
Json resource_usage(const fs::path& clips_folder);
}
