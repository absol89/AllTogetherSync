// The settings window of LipSync Together 0.8, which the add-on drew with Skyrim Together's own ImGui, on SKSE Menu
// Framework instead: same pages, settings, meter, voice status, key capture and Save/Revert; the framework's look.

#include "Window.h"

// The framework's header uses CommonLib's types (RE::InputEvent), so CommonLib comes first.
#include "Common/Together.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "third_party/SKSEMenuFramework.h"

namespace
{
using namespace ImGuiMCP;

constexpr const char* kPages[] = {"General", "Voice chat", "Hotkeys", "Advanced"};
constexpr const char* kWindowsDefault = "Windows default";
constexpr float kGameUnitsPerMetre = 70.f;

const ImVec4 kTextDim{0.62f, 0.62f, 0.62f, 1.f};
const ImVec4 kGreen{0.25f, 0.75f, 0.45f, 1.f};
const ImVec4 kAmber{0.90f, 0.62f, 0.20f, 1.f};
const ImVec4 kRed{0.90f, 0.30f, 0.35f, 1.f};
const ImVec4 kPink{0.94f, 0.38f, 0.57f, 1.f};
const ImVec4 kPurple{0.68f, 0.40f, 0.86f, 1.f};

std::atomic<const STRLipSync::Descriptor*> g_pDescriptor{nullptr};
SKSEMenuFramework::Model::WindowInterface* g_pWindow = nullptr;
std::atomic<bool> g_capturingKey{false};

// Values being dragged; applied when the slider is released, so a drag does not restart the relay at every step.
std::unordered_map<uint32_t, float> g_dragging;
int g_captureIndex = -1;
bool g_heldAtCapture[256]{};
std::vector<std::string> g_inputDevices;
std::vector<std::string> g_outputDevices;
bool g_devicesRead = false;

std::string KeyName(uint32_t aKey)
{
    if (aKey == 0)
        return "Not set";
    char name[64]{};
    const UINT cScan = MapVirtualKeyA(aKey, MAPVK_VK_TO_VSC);
    if (cScan && GetKeyNameTextA(static_cast<LONG>(cScan << 16), name, sizeof(name)) > 0)
        return name;
    snprintf(name, sizeof(name), "Key 0x%02X", aKey);
    return name;
}

std::vector<std::string> Split(const char* acpText, char aSeparator)
{
    std::vector<std::string> parts;
    std::string current;
    for (const char* p = acpText; p && *p; ++p)
    {
        if (*p == aSeparator)
        {
            if (!current.empty())
                parts.push_back(current);
            current.clear();
        }
        else if (*p != '\r')
            current.push_back(*p);
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

// Some drivers put "()" in front of their device names; drop it: "()Name (Driver)" -> "Name (Driver)".
std::string TidyDeviceName(const std::string& acName)
{
    return acName.rfind("()", 0) == 0 ? acName.substr(2) : acName;
}

std::string ChoiceLabel(const std::string& acChoice)
{
    if (_stricmp(acChoice.c_str(), "Shapes") == 0)
        return "Jaw and lips";
    if (_stricmp(acChoice.c_str(), "Jaw") == 0)
        return "Jaw only";
    return acChoice;
}

void ReadDevices(const STRLipSync::Descriptor& acDescriptor)
{
    static char s_buffer[8192];
    g_inputDevices.clear();
    g_outputDevices.clear();
    if (acDescriptor.ListDevices(0, s_buffer, sizeof(s_buffer)))
        g_inputDevices = Split(s_buffer, '\n');
    if (acDescriptor.ListDevices(1, s_buffer, sizeof(s_buffer)))
        g_outputDevices = Split(s_buffer, '\n');
    g_devicesRead = true;
}

void Help(const char* acpHelp)
{
    if (acpHelp && *acpHelp)
    {
        PushStyleColor(ImGuiCol_Text, kTextDim);
        TextWrapped("%s", acpHelp);
        PopStyleColor();
    }
}

void DrawMeter(const STRLipSync::Descriptor& acDescriptor)
{
    STRLipSync::Meter meter{};
    const bool cHave = acDescriptor.GetMeter(&meter) != 0;

    if (!cHave)
        TextColored(kRed, "NO MICROPHONE");
    else if (meter.Whispering)
        TextColored(kPurple, "WHISPERING");
    else if (meter.Speaking)
        TextColored(kPink, "SPEAKING");
    else
        TextColored(kTextDim, "QUIET");
    SameLine();
    if (!cHave)
        TextColored(kTextDim, "Pick a microphone on the General page.");
    else if (meter.Calibration < 1.f)
        TextColored(kTextDim, "Learning your normal voice... %.0f%%", meter.Calibration * 100.f);
    else if (meter.Range > 1.3f)
        TextColored(kTextDim, "Yelling: heard %.1fx further", meter.Range);
    else if (meter.Range < 0.7f)
        TextColored(kTextDim, "Soft voice: heard only nearby");
    else
        TextColored(kTextDim, "Normal range");

    // Level against -80..0 dB; the text says where the gate is.
    const float cLevel = cHave ? std::clamp((meter.LevelDb + 80.f) / 80.f, 0.f, 1.f) : 0.f;
    char overlay[96]{};
    if (cHave)
        snprintf(overlay, sizeof(overlay), "%.0f dB (needs %.0f dB)  voice %.0f%%", meter.LevelDb, meter.GateDb, meter.Voicing * 100.f);
    PushStyleColor(ImGuiCol_PlotHistogram, cHave && meter.LevelDb > meter.GateDb ? kPink : kTextDim);
    ProgressBar(cLevel, ImVec2(-1.f, 0.f), overlay);
    PopStyleColor();
}

void DrawVoiceStatus(const STRLipSync::Descriptor& acDescriptor)
{
    static char s_status[2048];
    if (!acDescriptor.GetVoiceStatus(s_status, sizeof(s_status)))
        return;
    SeparatorText("Status");
    for (const auto& cLine : Split(s_status, '\n'))
    {
        ImVec4 colour = kTextDim;
        if (cLine.find("Hosting") != std::string::npos || cLine.find("Connected") != std::string::npos || cLine.find(": talking") != std::string::npos)
            colour = kGreen;
        else if (cLine.find("Waiting") != std::string::npos || cLine.find("muted") != std::string::npos)
            colour = kAmber;
        PushStyleColor(ImGuiCol_Text, colour);
        TextWrapped("%s", cLine.c_str());
        PopStyleColor();
    }
}

void DrawKey(const STRLipSync::Descriptor& acDescriptor, uint32_t aIndex, const char* acpValue)
{
    const auto cKey = static_cast<uint32_t>(strtoul(acpValue, nullptr, 0));
    if (g_captureIndex == static_cast<int>(aIndex))
    {
        Button("Press a key...##key", ImVec2(-1.f, 0.f));
        if (IsItemHovered())
            SetTooltip("Esc: cancel   Backspace: no key");

        // The next key that goes down (not one already held when the button was clicked) is the one.
        for (uint32_t vk = 0x08; vk < 0xFF; ++vk)
        {
            const bool cDown = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
            if (!cDown)
            {
                g_heldAtCapture[vk] = false;
                continue;
            }
            if (g_heldAtCapture[vk])
                continue;
            if (vk == VK_ESCAPE)
                g_captureIndex = -1;
            else if (vk == VK_BACK || vk == VK_DELETE)
            {
                acDescriptor.SetSetting(aIndex, "0");
                g_captureIndex = -1;
            }
            else if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_LCONTROL && vk != VK_RCONTROL &&
                     vk != VK_LMENU && vk != VK_RMENU && vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_MBUTTON)
            {
                char text[16];
                snprintf(text, sizeof(text), "0x%02X", vk);
                acDescriptor.SetSetting(aIndex, text);
                g_captureIndex = -1;
            }
            break;
        }
        g_capturingKey = g_captureIndex >= 0;
        return;
    }

    const std::string cButton = KeyName(cKey) + "##key";
    if (Button(cButton.c_str(), ImVec2(-1.f, 0.f)) && g_captureIndex < 0)
    {
        g_captureIndex = static_cast<int>(aIndex);
        for (uint32_t vk = 0; vk < 256; ++vk)
            g_heldAtCapture[vk] = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        g_capturingKey = true;
    }
    if (IsItemHovered())
        SetTooltip("Click, then press the key");
}

void DrawDevice(const STRLipSync::Descriptor& acDescriptor, uint32_t aIndex, const STRLipSync::SettingInfo& acInfo, const char* acpValue)
{
    const std::string cPreview = *acpValue ? TidyDeviceName(acpValue) : kWindowsDefault;
    SetNextItemWidth(-1.f);
    if (BeginCombo("##device", cPreview.c_str()))
    {
        // Read the devices again whenever the list opens, so a headset plugged in meanwhile shows up.
        if (IsWindowAppearing() || !g_devicesRead)
            ReadDevices(acDescriptor);
        const auto& cDevices = acInfo.Type == STRLipSync::kSettingOutputDevice ? g_outputDevices : g_inputDevices;
        if (Selectable(kWindowsDefault, *acpValue == 0))
            acDescriptor.SetSetting(aIndex, "");
        for (const auto& cDevice : cDevices)
        {
            // The setting is part of a name; it matches a device that contains it.
            const bool cSelected = *acpValue && cDevice.find(acpValue) != std::string::npos;
            if (Selectable(TidyDeviceName(cDevice).c_str(), cSelected))
                acDescriptor.SetSetting(aIndex, cDevice.c_str());
        }
        EndCombo();
    }
}

void DrawSetting(const STRLipSync::Descriptor& acDescriptor, uint32_t aIndex)
{
    const STRLipSync::SettingInfo* pInfo = acDescriptor.GetSettingInfo(aIndex);
    if (!pInfo)
        return;
    char value[512]{};
    acDescriptor.GetSetting(aIndex, value, sizeof(value));
    const char* pLabel = pInfo->Label ? pInfo->Label : pInfo->Key;
    const char* pUnit = pInfo->Unit ? pInfo->Unit : "";

    PushID(static_cast<int>(aIndex));
    AlignTextToFramePadding();
    Text("%s", pLabel);

    switch (pInfo->Type)
    {
    case STRLipSync::kSettingBool:
    {
        bool on = value[0] == '1';
        SameLine();
        if (Checkbox("##toggle", &on))
            acDescriptor.SetSetting(aIndex, on ? "1" : "0");
        break;
    }
    case STRLipSync::kSettingInt:
    case STRLipSync::kSettingFloat:
    {
        // Shown in friendlier units (metres, kbit/s).
        const bool cMetres = strcmp(pUnit, "units") == 0;
        const bool cKbit = strcmp(pUnit, "bit/s") == 0;
        const float cScale = cMetres ? 1.f / kGameUnitsPerMetre : (cKbit ? 0.001f : 1.f);
        auto it = g_dragging.find(aIndex);
        float shown = it != g_dragging.end() ? it->second : static_cast<float>(atof(value)) * cScale;
        const float cMin = pInfo->Min * cScale;
        const float cMax = pInfo->Max * cScale;

        const char* pFormat = pInfo->Type == STRLipSync::kSettingInt ? "%.0f" : "%.2f";
        if (cMetres)
            pFormat = "%.0f m";
        else if (cKbit)
            pFormat = "%.0f kbit/s";
        else if (strcmp(pUnit, "dB") == 0)
            pFormat = "%.0f dB";
        else if (strcmp(pUnit, "ms") == 0)
            pFormat = "%.0f ms";
        else if (strcmp(pUnit, "x") == 0)
            pFormat = "%.2fx";

        // Distances span far more than anyone uses, so they get a fine drag rather than a coarse slider.
        SetNextItemWidth(-1.f);
        if (cMetres || cMax - cMin > 2000.f)
            DragFloat("##value", &shown, cMetres ? 0.25f : (cMax - cMin) / 3000.f, cMin, cMax, pFormat, ImGuiSliderFlags_AlwaysClamp);
        else
            SliderFloat("##value", &shown, cMin, cMax, pFormat, ImGuiSliderFlags_AlwaysClamp);

        if (IsItemActive())
            g_dragging[aIndex] = shown;
        if (IsItemDeactivatedAfterEdit())
        {
            char text[32];
            const float cStored = shown / cScale;
            if (pInfo->Type == STRLipSync::kSettingInt)
                snprintf(text, sizeof(text), "%d", static_cast<int>(std::lround(cStored)));
            else
                snprintf(text, sizeof(text), "%g", cStored);
            acDescriptor.SetSetting(aIndex, text);
            g_dragging.erase(aIndex);
        }
        else if (!IsItemActive())
            g_dragging.erase(aIndex);
        break;
    }
    case STRLipSync::kSettingKey: DrawKey(acDescriptor, aIndex, value); break;
    case STRLipSync::kSettingInputDevice:
    case STRLipSync::kSettingOutputDevice: DrawDevice(acDescriptor, aIndex, *pInfo, value); break;
    case STRLipSync::kSettingChoice:
    {
        SetNextItemWidth(-1.f);
        if (BeginCombo("##choice", ChoiceLabel(value).c_str()))
        {
            for (const auto& cChoice : Split(pInfo->Choices, '|'))
            {
                if (Selectable(ChoiceLabel(cChoice).c_str(), _stricmp(cChoice.c_str(), value) == 0))
                    acDescriptor.SetSetting(aIndex, cChoice.c_str());
            }
            EndCombo();
        }
        break;
    }
    default: Text("%s", value); break;
    }

    Help(pInfo->Help);
    Spacing();
    PopID();
}

void DrawContents()
{
    const STRLipSync::Descriptor* pDescriptor = g_pDescriptor.load();
    if (!pDescriptor)
        return;
    const STRLipSync::Descriptor& descriptor = *pDescriptor;

    TextColored(kTextDim, "LipSync Together %s, by Mailamea", descriptor.ModVersion ? descriptor.ModVersion : "");
    DrawMeter(descriptor);
    Spacing();

    if (BeginTabBar("##LipSyncTogetherPages"))
    {
        for (const char* cpPage : kPages)
        {
            if (!BeginTabItem(cpPage))
                continue;
            if (strcmp(cpPage, "Voice chat") == 0)
                DrawVoiceStatus(descriptor);

            const uint32_t cCount = descriptor.GetSettingCount();
            std::string group;
            bool first = true;
            for (uint32_t i = 0; i < cCount; ++i)
            {
                const STRLipSync::SettingInfo* pInfo = descriptor.GetSettingInfo(i);
                if (!pInfo || !pInfo->Page || strcmp(pInfo->Page, cpPage) != 0)
                    continue;
                const std::string cGroup = pInfo->Group ? pInfo->Group : "";
                if (first || group != cGroup)
                {
                    group = cGroup;
                    first = false;
                    SeparatorText(group.c_str());
                }
                DrawSetting(descriptor, i);
            }
            EndTabItem();
        }
        EndTabBar();
    }

    // Saved state, Revert, Save.
    Separator();
    const bool cUnsaved = descriptor.HasUnsavedChanges() != 0;
    if (cUnsaved)
        TextColored(kAmber, "Changes are live, not saved yet");
    else
        TextColored(kTextDim, "All changes saved");
    BeginDisabled(!cUnsaved);
    if (Button("Revert"))
    {
        descriptor.ReloadSettings();
        g_dragging.clear();
    }
    SameLine();
    if (Button("Save"))
        descriptor.SaveSettings();
    EndDisabled();
}

void __stdcall RenderPage()
{
    DrawContents();
}

void __stdcall RenderWindow()
{
    if (!g_pWindow)
        return;
    SetNextWindowSize(ImVec2(520.f, 680.f), ImGuiCond_FirstUseEver);
    bool open = g_pWindow->IsOpen;
    if (Begin("LipSync Together##LipSyncTogetherSettings", &open, ImGuiWindowFlags_NoCollapse))
    {
        DrawContents();
        if (Button("Close"))
            open = false;
    }
    End();
    if (!open)
    {
        g_pWindow->IsOpen = false;
        g_captureIndex = -1;
        g_capturingKey = false;
    }
}
} // namespace

bool Window::Register(const STRLipSync::Descriptor* apDescriptor) noexcept
{
    g_pDescriptor = apDescriptor;
    if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule())
        return false;

    SKSEMenuFramework::SetSection("LipSync Together");
    SKSEMenuFramework::AddSectionItem("Settings", &RenderPage);
    // Input goes to the window while it is open, as with the 0.8 window and Skyrim Together's overlay.
    g_pWindow = SKSEMenuFramework::AddWindow(&RenderWindow, true);
    return true;
}

void Window::Toggle() noexcept
{
    if (!g_pWindow)
        return;
    const bool cOpen = !g_pWindow->IsOpen;
    if (cOpen)
        g_devicesRead = false;
    g_pWindow->IsOpen = cOpen;
}

bool Window::IsCapturingKey() noexcept
{
    return g_capturingKey;
}
