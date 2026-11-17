// (PS5) host/ps5/ps5_code_idle.cpp: the lowered code of programs not drawn for a while, let go
#pragma once
#include <cstdint>
#include "../../gpu/render/rx_nvn.h"

extern "C" void hos_ps5_code_idle_start(void (*release)(const nvn_prog*));   // (vk_replay.cpp, the renderer's start)
extern "C" void hos_ps5_code_idle_touch(uint64_t serial);                    // (vk_draw.cpp, each draw's and dispatch's program)
