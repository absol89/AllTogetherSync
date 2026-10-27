#pragma once

// The part of LipSync Together 0.8 that listens to the microphone and carries voices (plugin/), as the Skyrim Together
// side (Host.cpp) and the settings window (Window.cpp) reach it: the same STR LipSync API descriptor the 0.8 add-on in
// the Skyrim Together exe used, now handed over directly instead of exported.

#include "STRLipSyncAPI.h"

#include <cstdint>

struct HINSTANCE__;

namespace LipSync
{
const STRLipSync::Descriptor* GetDescriptor() noexcept;
// Reads LipSyncTogether.ini next to the plugin. The microphone opens on the first GetFrame.
void Start(HINSTANCE__* aModule, uint32_t aRuntimeVersion) noexcept;
// LipSync Together's own log (Documents\My Games\Skyrim Special Edition\SKSE\LipSyncTogether.log), printf-style.
void Log(const char* acpFormat, ...);
} // namespace LipSync
