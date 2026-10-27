#pragma once

// LipSync Together's settings, drawn by SKSE Menu Framework: a window that the open key (Shift+V by default) toggles,
// and the same page in the framework's Mod Control Panel. Built from the settings list the plugin publishes, so every
// INI setting appears without this file knowing them; changes take effect at once, Save writes the INI.

#include "STRLipSyncAPI.h"

namespace Window
{
// False when SKSE Menu Framework is not installed (the INI still works).
bool Register(const STRLipSync::Descriptor* apDescriptor) noexcept;
void Toggle() noexcept;
// While the window waits for a key to assign, that key must not also toggle the window.
bool IsCapturingKey() noexcept;
} // namespace Window
