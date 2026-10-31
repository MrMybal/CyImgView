#pragma once
#include <windows.h>
#include <cstdint>

namespace newdoc {
enum Control { Width=101, Height, Swap, Category=110, Preset=120, Background=130, Preview=140, Error, Summary };
struct Options {
    uint32_t width=1920, height=1080;
    int background=0; // transparent, white, black
    uint32_t maxDimension=16384;
};
bool Show(HWND owner, Options& options);
}
