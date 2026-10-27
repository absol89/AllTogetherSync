// STCampfireSync: example plugin for the Skyrim Together Plugin API.
//
// By DJLegends (https://github.com/DJLegends1011/STCampfireSync), shipped here with permission. This copy is
// absol89/STCampfireSync's fix/renamed-plugin-api-export branch: it looks up SkyrimTogetherRequestPluginAPI, the
// export name of Skyrim Together builds since 1.8.7-5.
//
// Press the hotkey to place a campfire in front of you, or to pick up one you are looking at. Every party member
// sees the same campfires. The whole sync is one Channel and two Mod Messages:
//
//   place <id> <x> <y> <z> <rotZ>    a campfire exists here
//   remove <id>                      a campfire is gone
//
// Skyrim Together relays the bytes to the party and tells us who sent them. It never looks inside.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

// CommonLib refuses Windows headers that come before it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <random>
#include <unordered_map>

#include "SkyrimTogetherAPI.h"

namespace
{
namespace STAPI = SkyrimTogetherAPI;

constexpr const char* kChannelName = "DJL.CampfireSync";
// Explicitly relative to the game folder: GetPrivateProfile* looks in the Windows folder for paths it does not
// recognise as paths.
constexpr const char* kIniPath = R"(.\Data\SKSE\Plugins\STCampfireSync.ini)";

// Skyrim.esm 0x035F49 "Campfire01Burning": a burning campfire moveable static (campfire01burning.nif plus its
// looping fire sound). Overridable from the ini.
constexpr RE::FormID kDefaultCampfireBase = 0x035F49;
constexpr int kDefaultHotkey = 0x42; // DirectInput scan code of F8

constexpr float kPlaceDistance = 150.0f; // in front of the player
constexpr float kPickRange = 300.0f;     // how far away a campfire can be picked up from
constexpr float kPickMinCos = 0.866f;    // ... and it must be within 30 degrees of where the player faces
constexpr float kMaxCoordinate = 1.0e6f; // anything larger in a message is garbage

// One campfire, ours or a remote copy. The handle (not a raw pointer) survives the game unloading the cell.
// Owner is the player ID that last announced a remote copy. Player IDs only last one connection, so a campfire's
// ID never contains one: when its placer reconnects, they re-announce it and Owner moves to their new ID.
struct Campfire
{
    RE::ObjectRefHandle Ref;
    bool Own;
    std::uint32_t Owner;
};

STAPI::IVSkyrimTogether1* g_pApi = nullptr;
STAPI::ChannelHandle g_channel = 0;
RE::TESBoundObject* g_pBase = nullptr;
std::uint32_t g_hotkey = kDefaultHotkey;
std::mt19937 g_random{std::random_device{}()};

// Only touched on the game's main thread. The Skyrim Together callbacks hand their work over with AddTask.
std::unordered_map<std::uint32_t, Campfire> g_campfires;

// ---- Messages ---------------------------------------------------------------------------------------------

void SendToParty(const std::string& aMessage)
{
    g_pApi->Send(g_channel, aMessage.data(), aMessage.size());
}

std::string FormatPlace(std::uint32_t aId, const RE::NiPoint3& aPos, float aRotZ)
{
    return std::format("place {} {:.2f} {:.2f} {:.2f} {:.4f}", aId, aPos.x, aPos.y, aPos.z, aRotZ);
}

// Payloads are untrusted bytes: any token that is not exactly a number is a rejected message.
template <class T>
bool ParseNumber(std::string_view aToken, T& aOut)
{
    const auto [pEnd, ec] = std::from_chars(aToken.data(), aToken.data() + aToken.size(), aOut);
    if (ec != std::errc() || pEnd != aToken.data() + aToken.size())
        return false;
    if constexpr (std::is_floating_point_v<T>)
        return std::isfinite(aOut) && std::fabs(aOut) <= kMaxCoordinate;
    return true;
}

std::vector<std::string_view> Split(std::string_view aText)
{
    std::vector<std::string_view> tokens;
    while (!aText.empty())
    {
        const auto space = aText.find(' ');
        tokens.push_back(aText.substr(0, space));
        if (space == std::string_view::npos)
            break;
        aText.remove_prefix(space + 1);
    }
    return tokens;
}

// ---- Campfires --------------------------------------------------------------------------------------------

void Destroy(const Campfire& aCampfire)
{
    if (const auto pRef = aCampfire.Ref.get())
    {
        pRef->Disable();
        pRef->SetDelete(true);
    }
}

// Creates a campfire at aPos. Campfires last one game session, ours included: none is ever written to a save.
// A saved one would come back after a reload without this plugin knowing about it (the hotkey would stack a new
// one on top, and nobody else would ever see it), and a saved remote copy would outlive its placer.
bool Spawn(std::uint32_t aId, const RE::NiPoint3& aPos, float aRotZ, bool aOwn, std::uint32_t aOwner)
{
    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    if (!pPlayer || !g_pBase || g_campfires.contains(aId))
        return false;

    const auto pRef = pPlayer->PlaceObjectAtMe(g_pBase, false);
    if (!pRef)
        return false;

    // kRefPermanentlyDeleted (0x80) makes TESObjectREFR::CheckSaveGame return false, so the save drops the
    // reference. Set it before anything can save, and set nothing else: other bits in this word mean other things.
    pRef->inGameFormFlags.set(RE::TESForm::InGameFormFlag::kRefPermanentlyDeleted);

    // PlaceObjectAtMe copies the player's whole rotation, including the camera pitch while looking at the
    // ground, which tilts the campfire. Keep only the yaw so every client places it upright and identically.
    pRef->data.angle.x = 0.0f;
    pRef->data.angle.y = 0.0f;
    pRef->data.angle.z = aRotZ;
    pRef->SetPosition(aPos);

    g_campfires.emplace(aId, Campfire{pRef->GetHandle(), aOwn, aOwner});
    return true;
}

void Remove(std::uint32_t aId)
{
    const auto it = g_campfires.find(aId);
    if (it == g_campfires.end())
        return; // already gone, or never arrived
    Destroy(it->second);
    g_campfires.erase(it);
}

void RemoveRemoteCopiesIf(auto aPredicate)
{
    for (auto it = g_campfires.begin(); it != g_campfires.end();)
    {
        if (!it->second.Own && aPredicate(it->second))
        {
            Destroy(it->second);
            it = g_campfires.erase(it);
        }
        else
            ++it;
    }
}

// The nearest known campfire in front of the player, or 0.
std::uint32_t FindAimedCampfire(const RE::PlayerCharacter& aPlayer)
{
    const auto pos = aPlayer.GetPosition();
    const float yaw = aPlayer.GetAngleZ();
    const float forwardX = std::sin(yaw);
    const float forwardY = std::cos(yaw);

    std::uint32_t best = 0;
    float bestDistance = kPickRange;
    for (const auto& [id, campfire] : g_campfires)
    {
        const auto pRef = campfire.Ref.get();
        if (!pRef)
            continue;
        const auto delta = pRef->GetPosition() - pos;
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        if (distance > bestDistance || std::fabs(delta.z) > kPickRange)
            continue;
        if (distance > 1.0f && (delta.x * forwardX + delta.y * forwardY) / distance < kPickMinCos)
            continue;
        best = id;
        bestDistance = distance;
    }
    return best;
}

void OnHotkey()
{
    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    auto* pUI = RE::UI::GetSingleton();
    if (!pPlayer || !pUI || pUI->GameIsPaused())
        return;

    if (g_pApi->GetLocalPlayerId() == 0)
    {
        RE::DebugNotification("Campfire sync: not connected to a Skyrim Together server");
        return;
    }

    if (const auto aimedId = FindAimedCampfire(*pPlayer))
    {
        Remove(aimedId);
        SendToParty(std::format("remove {}", aimedId));
        return;
    }

    // A random ID, not one built from our player ID: that changes on every reconnect, while a campfire outlives
    // it. With a handful of campfires per party, a 32-bit clash is not a practical concern for an example.
    std::uint32_t id = 0;
    while (id == 0 || g_campfires.contains(id))
        id = g_random();
    const float yaw = pPlayer->GetAngleZ();
    auto pos = pPlayer->GetPosition();
    pos.x += std::sin(yaw) * kPlaceDistance;
    pos.y += std::cos(yaw) * kPlaceDistance;

    if (Spawn(id, pos, yaw, true, 0))
        SendToParty(FormatPlace(id, pos, yaw));
}

// ---- Skyrim Together callbacks ----------------------------------------------------------------------------
// These do not run on the main thread, so they only validate and queue. Every touch of the game goes through
// SKSE's task interface.

void HandleMessage(std::uint32_t aSenderId, const std::string& aMessage)
{
    const auto tokens = Split(aMessage);

    if (tokens.size() == 6 && tokens[0] == "place")
    {
        std::uint32_t id = 0;
        RE::NiPoint3 pos;
        float rotZ = 0.0f;
        if (!ParseNumber(tokens[1], id) || !ParseNumber(tokens[2], pos.x) || !ParseNumber(tokens[3], pos.y) ||
            !ParseNumber(tokens[4], pos.z) || !ParseNumber(tokens[5], rotZ))
            return;
        if (id == 0)
            return;
        // Already known: the placer reconnected under a new player ID and is re-announcing it. Follow them, so
        // the copy is removed when they leave. Never let anyone claim one of our own campfires.
        if (const auto it = g_campfires.find(id); it != g_campfires.end())
        {
            if (!it->second.Own)
                it->second.Owner = aSenderId;
            return;
        }
        Spawn(id, pos, rotZ, false, aSenderId);
    }
    else if (tokens.size() == 2 && tokens[0] == "remove")
    {
        std::uint32_t id = 0;
        if (ParseNumber(tokens[1], id))
            Remove(id);
    }
    // Anything else is not ours (or a newer version's). Ignore it.
}

void OnMessage(std::uint32_t aSenderId, const char* apBytes, size_t aLength)
{
    if (!apBytes || aLength == 0 || aLength > STAPI::kMaxMessageSize)
        return;
    SKSE::GetTaskInterface()->AddTask([aSenderId, message = std::string(apBytes, aLength)]() { HandleMessage(aSenderId, message); });
}

void OnPartyChanged(std::uint32_t aPlayerId, bool aJoined)
{
    SKSE::GetTaskInterface()->AddTask(
        [aPlayerId, aJoined]()
        {
            const auto localId = g_pApi->GetLocalPlayerId();
            if (aPlayerId == localId)
                return;

            if (aJoined)
            {
                // Late joiner: tell them about the campfires we placed and still have.
                for (const auto& [id, campfire] : g_campfires)
                {
                    const auto pRef = campfire.Ref.get();
                    if (!campfire.Own || !pRef)
                        continue;
                    const auto message = FormatPlace(id, pRef->GetPosition(), pRef->GetAngleZ());
                    g_pApi->SendTo(g_channel, aPlayerId, message.data(), message.size());
                }
                return;
            }

            RemoveRemoteCopiesIf([aPlayerId](const Campfire& acCampfire) { return acCampfire.Owner == aPlayerId; });

            // When we leave or disconnect, we get joined=false for every former member. Once we are alone
            // nobody is left to remove their campfires, so drop them all.
            if (localId == 0 || !g_pApi->IsInParty())
                RemoveRemoteCopiesIf([](const Campfire&) { return true; });
        });
}

// ---- Setup ------------------------------------------------------------------------------------------------

class HotkeySink : public RE::BSTEventSink<RE::InputEvent*>
{
public:
    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* apEvents, RE::BSTEventSource<RE::InputEvent*>*) override
    {
        for (auto* pEvent = apEvents ? *apEvents : nullptr; pEvent; pEvent = pEvent->next)
        {
            const auto* pButton = pEvent->AsButtonEvent();
            if (pButton && pButton->GetDevice() == RE::INPUT_DEVICE::kKeyboard && pButton->GetIDCode() == g_hotkey && pButton->IsDown())
                OnHotkey();
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

void LoadConfig()
{
    g_hotkey = GetPrivateProfileIntA("General", "HotkeyScanCode", kDefaultHotkey, kIniPath);

    char text[32]{};
    GetPrivateProfileStringA("General", "CampfireBaseFormID", "", text, sizeof(text), kIniPath);
    RE::FormID formId = kDefaultCampfireBase;
    if (text[0])
        formId = static_cast<RE::FormID>(std::strtoul(text, nullptr, 0));

    g_pBase = RE::TESForm::LookupByID<RE::TESBoundObject>(formId);
    if (g_pBase)
        SKSE::log::info("Campfire base {:08X}, hotkey scan code {}", formId, g_hotkey);
    else
        SKSE::log::error("No placeable form {:08X}: set CampfireBaseFormID in {}", formId, kIniPath);
}

void OnSkseMessage(SKSE::MessagingInterface::Message* apMessage)
{
    if (apMessage->type != SKSE::MessagingInterface::kDataLoaded)
        return;

    LoadConfig();
    static HotkeySink s_sink;
    RE::BSInputDeviceManager::GetSingleton()->AddEventSink(&s_sink);
}

void InitLog()
{
    auto path = SKSE::log::log_directory();
    if (!path)
        return;
    *path /= "STCampfireSync.log";
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

    // Skyrim Together exports kRequestPluginAPIName from its own executable. Without it we are not running under
    // Skyrim Together (or it is too old), and the plugin does nothing.
    const auto pRequest = reinterpret_cast<STAPI::RequestPluginAPI_t>(GetProcAddress(GetModuleHandleW(nullptr), STAPI::kRequestPluginAPIName));
    if (!pRequest)
    {
        SKSE::log::info("{} not found: not running under Skyrim Together, staying inactive", STAPI::kRequestPluginAPIName);
        return true;
    }

    g_pApi = static_cast<STAPI::IVSkyrimTogether1*>(pRequest(STAPI::InterfaceVersion::V1));
    if (!g_pApi)
    {
        SKSE::log::info("Skyrim Together does not offer plugin API v1, staying inactive");
        return true;
    }

    g_channel = g_pApi->RegisterChannel(kChannelName, OnMessage, OnPartyChanged);
    if (!g_channel)
    {
        SKSE::log::error("Could not register channel {}", kChannelName);
        g_pApi = nullptr;
        return true;
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnSkseMessage);
    SKSE::log::info("Registered channel {}", kChannelName);
    return true;
}
