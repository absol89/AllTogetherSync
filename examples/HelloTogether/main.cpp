// HelloTogether: the smallest useful Skyrim Together plugin, to copy from.
//
// Press F8 and every player who has your character in range gets a notification that you waved. When someone joins
// your party, your client says hello to them over the party channel.
//
// It shows the four things every plugin does:
//   1. find the Plugin API in SkyrimTogether.exe (and stay quiet when the game runs without Skyrim Together);
//   2. register a channel, with callbacks for what arrives on it;
//   3. send: about an actor to the players near it (SendNear), or to party members (Send / SendTo);
//   4. move from the callback's thread to the game's main thread before touching the game.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

// CommonLib must come before Windows.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "SkyrimTogetherAPI.h"

#include <string>

namespace
{
namespace STAPI = SkyrimTogetherAPI;

// Name channels after your plugin. Add a version when you change the message format, so old and new copies of the
// plugin don't misread each other: "HelloTogether.v2" and "HelloTogether.v1" never see each other's messages.
constexpr const char* kChannelName = "HelloTogether.v1";

// Keyboard scan code (DirectInput): 0x42 is F8. Skyrim Together uses F2, F3, F6, F7 and Right Ctrl.
constexpr std::uint32_t kWaveKey = 0x42;

// The local player's form ID. Skyrim Together syncs the player's character like any other actor.
constexpr RE::FormID kPlayerFormId = 0x14;

STAPI::IVSkyrimTogether2* g_pApi = nullptr;
STAPI::ChannelHandle g_channel = 0;

// ---- 1. Finding the API -------------------------------------------------------------------------------------------

// SkyrimTogether.exe exports one function. Without Skyrim Together (plain SKSE, or an old Skyrim Together build) it
// is missing, and the plugin simply does nothing.
STAPI::IVSkyrimTogether2* FindApi()
{
    const auto pRequest = reinterpret_cast<STAPI::RequestPluginAPI_t>(GetProcAddress(GetModuleHandleW(nullptr), STAPI::kRequestPluginAPIName));
    if (!pRequest)
        return nullptr;
    return static_cast<STAPI::IVSkyrimTogether2*>(pRequest(STAPI::InterfaceVersion::V2));
}

// ---- 3. Receiving -------------------------------------------------------------------------------------------------

// These callbacks run on Skyrim Together's update thread, not necessarily the game's main thread. Copy what you
// need and hand the game work to SKSE's task queue, which runs it on the main thread.

void OnWave(std::uint32_t aSenderId, std::uint32_t aServerId, const char* apBytes, std::size_t aLength)
{
    // Messages are bytes, not strings: check them like input from a stranger, because that is what they are.
    if (std::string_view(apBytes, aLength) != "wave")
        return;

    SKSE::GetTaskInterface()->AddTask([aSenderId, aServerId]() {
        // The server ID names the actor on every client; GetFormId says which form it is on this one.
        const auto formId = g_pApi->GetFormId(aServerId);
        const auto* pActor = formId ? RE::TESForm::LookupByID<RE::Actor>(formId) : nullptr;
        const std::string name = pActor && pActor->GetName() && *pActor->GetName() ? pActor->GetName() : "Player " + std::to_string(aSenderId);
        RE::DebugNotification((name + " waves at you").c_str());
    });
}

void OnPartyMessage(std::uint32_t aSenderId, const char* apBytes, std::size_t aLength)
{
    if (std::string_view(apBytes, aLength) != "hello")
        return;

    SKSE::GetTaskInterface()->AddTask([aSenderId]() { RE::DebugNotification(("Player " + std::to_string(aSenderId) + " says hello").c_str()); });
}

void OnPartyChanged(std::uint32_t aPlayerId, bool aJoined)
{
    if (aJoined)
        g_pApi->SendTo(g_channel, aPlayerId, "hello", 5);
}

// ---- 3. Sending ---------------------------------------------------------------------------------------------------

void Wave()
{
    // A message about an actor goes to the players who have that actor loaded, party or not. Only the player who
    // controls an actor may send about it; your own character is always yours.
    const auto serverId = g_pApi->GetServerId(kPlayerFormId);
    if (serverId == 0)
    {
        RE::DebugNotification("HelloTogether: not connected to a Skyrim Together server");
        return;
    }

    const auto result = g_pApi->SendNear(g_channel, serverId, STAPI::Delivery::Reliable, "wave", 4);
    if (result != STAPI::SendResult::Ok)
        SKSE::log::warn("Wave not sent: result {}", static_cast<int>(result));
}

// Watches the keyboard. Input events arrive on the main thread, so this can touch the game directly.
class KeySink final : public RE::BSTEventSink<RE::InputEvent*>
{
public:
    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* apEvents, RE::BSTEventSource<RE::InputEvent*>*) override
    {
        for (auto* pEvent = apEvents ? *apEvents : nullptr; pEvent; pEvent = pEvent->next)
        {
            const auto* pButton = pEvent->AsButtonEvent();
            if (pButton && pButton->GetDevice() == RE::INPUT_DEVICE::kKeyboard && pButton->GetIDCode() == kWaveKey && pButton->IsDown())
                Wave();
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

void OnSkseMessage(SKSE::MessagingInterface::Message* apMessage)
{
    // The input system exists once the game's data has loaded.
    if (apMessage->type == SKSE::MessagingInterface::kDataLoaded)
    {
        static KeySink s_sink;
        RE::BSInputDeviceManager::GetSingleton()->AddEventSink(&s_sink);
    }
}

void InitLog()
{
    auto path = SKSE::log::log_directory();
    if (!path)
        return;
    *path /= "HelloTogether.log";
    auto logger = std::make_shared<spdlog::logger>("global", std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface* apSkse)
{
    SKSE::Init(apSkse);
    InitLog();

    g_pApi = FindApi();
    if (!g_pApi)
    {
        SKSE::log::info("Skyrim Together's Plugin API (V2) not found: staying inactive");
        return true;
    }

    // ---- 2. Registering the channel ------------------------------------------------------------------------------
    g_channel = g_pApi->RegisterChannel(kChannelName, OnPartyMessage, OnPartyChanged);
    if (g_channel == 0 || !g_pApi->SetNearMessageCallback(g_channel, OnWave))
    {
        SKSE::log::error("Could not register channel {}", kChannelName);
        return true;
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnSkseMessage);
    SKSE::log::info("Ready: press F8 to wave");
    return true;
}
