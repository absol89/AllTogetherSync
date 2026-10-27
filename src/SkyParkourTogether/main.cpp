// SkyParkour Together 2: shows SkyParkour's animations on the other players, as a standalone SKSE plugin on Skyrim
// Together's Plugin API V2. Nothing is patched into Skyrim Together.
//
// SkyParkour starts its moves by sending "SkyParkour..." events to the player's animation graph, after setting a
// few graph variables that pick the animation (ledge height, grab variant, ...). This plugin watches the player's
// graph for those events, reads the variables, and sends both as one message about the player's character. The
// other players' clients set the same variables on their copy of this player and send it the same event.
//
//   <event>|<type>:<variable>:<value>|...        type: i int, b bool, f float
//
// Skyrim Together relays the message to the players who have this character in range, and never stores it.
// Players need SkyParkour and this plugin to see each other's moves; a client without SkyParkour, or whose copy's
// animations lack SkyParkour's, skips them (logged) and everything else keeps working. Replaces mailamea's
// SkyParkourTogether.dll 1.x, which switched on the same work built into the Skyrim Together exe. It does not export
// that version's descriptor, so the built-in copy (in builds that have it) stays off.

#include "Common/Safety.h"

#include <charconv>
#include <format>

namespace
{
using namespace Together;

constexpr const char* kPluginName = "SkyParkourTogether";
constexpr const char* kChannelName = "SkyParkourTogether.v2";
constexpr std::string_view kEventPrefix = "SkyParkour";

struct Variable
{
    const char* Name;
    char Type;
};

// The graph variables SkyParkour's animations read (SkyParkourNG 3.6.5).
constexpr Variable kVariables[] = {
    {"SkyParkourLedge", 'i'},         // vault, climb, grab or step, and its height
    {"SkyParkourGrabVariant", 'b'},   // low or high grab
    {"SkyParkourLowerBody", 'b'},     // steps with a weapon out play on the legs only
    {"SkyParkourIsLandingRoll", 'b'}, // slide or landing roll
    {"SkyParkourSpeedMult", 'f'},     // playback speed setting
};

STAPI::IVSkyrimTogether2* g_pApi = nullptr;
STAPI::ChannelHandle g_channel = 0;

// The first event sent and the first one played on a copy are logged, so a session where nothing gets across
// shows in the log. Once each per game start; later failures are logged at most every so many events.
std::atomic<bool> g_loggedSend{false};
std::atomic<bool> g_loggedPlay{false};
std::atomic<std::uint32_t> g_sendFailures{0};

// ---- Sending (any thread: SkyParkour notifies the player's graph from wherever it runs) ---------------------

std::string Encode(const RE::IAnimationGraphManagerHolder* apGraph, std::string_view aEvent)
{
    std::string out(aEvent);
    for (const auto& cVariable : kVariables)
    {
        const RE::BSFixedString name(cVariable.Name);
        switch (cVariable.Type)
        {
        case 'i':
            if (std::int32_t value = 0; apGraph->GetGraphVariableInt(name, value))
                out += std::format("|i:{}:{}", cVariable.Name, value);
            break;
        case 'b':
            if (bool value = false; apGraph->GetGraphVariableBool(name, value))
                out += std::format("|b:{}:{}", cVariable.Name, value ? 1 : 0);
            break;
        case 'f':
            if (float value = 0.0f; apGraph->GetGraphVariableFloat(name, value))
                out += std::format("|f:{}:{:.3f}", cVariable.Name, value);
            break;
        }
    }
    return out;
}

void Send(const RE::IAnimationGraphManagerHolder* apGraph, std::string_view aEvent)
{
    // Quiet until the server has said it relays messages about actors (and when offline).
    if (!g_pApi->IsNearSupported())
        return;

    const auto cServerId = g_pApi->GetServerId(kPlayerFormId);
    if (cServerId == 0)
        return;

    std::string payload = Encode(apGraph, aEvent);
    if (payload.size() > STAPI::kMaxNearMessageSize)
        payload.assign(aEvent);

    const auto cResult = g_pApi->SendNear(g_channel, cServerId, STAPI::Delivery::Reliable, payload.data(), payload.size());
    if (cResult == STAPI::SendResult::Ok)
    {
        if (!g_loggedSend.exchange(true))
            SKSE::log::info("First event sent to the players in range: {}", payload);
    }
    else if (g_sendFailures++ % 50 == 0)
    {
        SKSE::log::warn("'{}' not sent: {}", aEvent, ToString(cResult));
    }
}

// IAnimationGraphManagerHolder::NotifyAnimationGraph, in the player's vtable only: copies of other players are
// Characters, so playing an event on them does not come back through here.
struct NotifyPlayerGraph
{
    static bool Thunk(RE::IAnimationGraphManagerHolder* apThis, const RE::BSFixedString& acEvent)
    {
        const bool cResult = Original(apThis, acEvent);

        // Only what the player's graph took: a refused event did not animate here either.
        if (cResult && g_channel && !acEvent.empty() && StartsWithNoCase(acEvent.c_str(), kEventPrefix))
            Send(apThis, acEvent.c_str());

        return cResult;
    }

    static inline REL::Relocation<decltype(Thunk)> Original;
};

// ---- Playing (main thread) -----------------------------------------------------------------------------------

const Variable* FindVariable(std::string_view aName, char aType) noexcept
{
    for (const auto& cVariable : kVariables)
    {
        if (cVariable.Type == aType && _stricmp(std::string(aName).c_str(), cVariable.Name) == 0)
            return &cVariable;
    }
    return nullptr;
}

template <class T> bool ParseNumber(std::string_view aText, T& aOut) noexcept
{
    const auto cResult = std::from_chars(aText.data(), aText.data() + aText.size(), aOut);
    return cResult.ec == std::errc{} && cResult.ptr == aText.data() + aText.size();
}

void Play(std::uint32_t aServerId, const std::string& acMessage)
{
    // The server vouches for who controls aServerId; we only check that it is somebody else's character here.
    if (g_pApi->IsOwnedLocally(aServerId))
        return;
    const auto cFormId = g_pApi->GetFormId(aServerId);
    auto* pActor = cFormId ? RE::TESForm::LookupByID<RE::Actor>(cFormId) : nullptr;
    if (!pActor || pActor->IsPlayerRef())
        return;

    std::string_view rest(acMessage);
    const auto cFirstBar = rest.find('|');
    const std::string_view cEvent = rest.substr(0, cFirstBar);
    if (cEvent.empty() || cEvent.size() > 128 || !StartsWithNoCase(cEvent, kEventPrefix))
        return;

    // Variables first, so the copy picks the animation the owner played. Only the ones listed above.
    while (cFirstBar != std::string_view::npos && !rest.empty())
    {
        const auto cBar = rest.find('|');
        if (cBar == std::string_view::npos)
            break;
        rest.remove_prefix(cBar + 1);
        const std::string_view cSpec = rest.substr(0, rest.find('|'));

        // "t:name:value"
        const auto cLast = cSpec.rfind(':');
        if (cSpec.size() < 5 || cSpec[1] != ':' || cLast <= 2)
            continue;
        const char cType = cSpec[0];
        const Variable* pVariable = FindVariable(cSpec.substr(2, cLast - 2), cType);
        if (!pVariable)
            continue;

        const std::string_view cValue = cSpec.substr(cLast + 1);
        const RE::BSFixedString name(pVariable->Name);
        if (cType == 'i')
        {
            if (std::int32_t value = 0; ParseNumber(cValue, value))
                pActor->SetGraphVariableInt(name, value);
        }
        else if (cType == 'b')
        {
            pActor->SetGraphVariableBool(name, cValue == "1");
        }
        else if (cType == 'f')
        {
            if (float value = 0.0f; ParseNumber(cValue, value))
                pActor->SetGraphVariableFloat(name, value);
        }
    }

    const bool cPlayed = pActor->NotifyAnimationGraph(RE::BSFixedString(std::string(cEvent).c_str()));
    if (cPlayed)
    {
        if (!g_loggedPlay.exchange(true))
            SKSE::log::info("First event from another player played: '{}' on {:08X}", cEvent, cFormId);
    }
    else
    {
        // A refusal usually means this client's behavior files lack SkyParkour's animations for that character.
        static std::uint32_t s_refusals = 0;
        if (s_refusals++ % 50 == 0)
            SKSE::log::warn("'{}' refused by the graph of {:08X}", cEvent, cFormId);
    }
}

void OnNearMessage(std::uint32_t, std::uint32_t aServerId, const char* apBytes, size_t aLength)
{
    if (!apBytes || aLength == 0 || aLength > STAPI::kMaxNearMessageSize)
        return;
    SKSE::GetTaskInterface()->AddTask([aServerId, message = std::string(apBytes, aLength)]() { Play(aServerId, message); });
}

void OnSkseMessage(SKSE::MessagingInterface::Message* apMessage)
{
    if (apMessage->type != SKSE::MessagingInterface::kDataLoaded)
        return;

    if (!GetModuleHandleW(L"SkyParkourNG.dll"))
        SKSE::log::warn("SkyParkourNG.dll is not loaded: nothing to send, and other players' moves cannot play here");
    Safety::StartFreezeWatchdog(g_pApi);
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface* apSkse)
{
    SKSE::Init(apSkse);
    InitLog("SkyParkourTogether.log");
    Safety::InstallTerminateHandler(kPluginName);

    g_pApi = FindApi();
    if (!g_pApi)
        return true;

    g_channel = g_pApi->RegisterChannel(kChannelName, nullptr, nullptr);
    if (!g_channel || !g_pApi->SetNearMessageCallback(g_channel, OnNearMessage))
    {
        SKSE::log::error("Could not register channel {}", kChannelName);
        g_channel = 0;
        return true;
    }

    REL::Relocation<std::uintptr_t> playerGraphVtable{RE::VTABLE_PlayerCharacter[3]};
    NotifyPlayerGraph::Original = playerGraphVtable.write_vfunc(0x1, NotifyPlayerGraph::Thunk);

    SKSE::GetMessagingInterface()->RegisterListener(OnSkseMessage);
    SKSE::log::info("Registered channel {}, watching the player's SkyParkour events", kChannelName);
    return true;
}
