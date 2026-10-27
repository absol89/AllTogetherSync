#pragma once

// Skyrim Together Plugin API, versions 1 to 3.
//
// Lets an SKSE plugin sync its own state through Skyrim Together: the plugin registers a Channel and sends small
// Mod Messages on it, Skyrim Together relays them without interpreting them, and the same plugin on the other
// clients receives them. Version 1 sends to the sender's party. Version 2 adds messages about one actor, relayed to
// the players who have that actor in range, optionally unreliable, paced under the server's limit. Nothing is stored
// on the server. Version 3 lets a plugin hand Skyrim Together what it needs to redo its own work on a copy, such as
// face tints after a plugin rebuilt the copy's head, see how many of its messages about actors are waiting, and read
// what Skyrim Together knows about the session: the network time and each copy's playback time, the other players
// in range, and the server's address. This header is self-contained, copy it into your plugin.
//
// Usage:
//   auto pRequest = reinterpret_cast<SkyrimTogetherAPI::RequestPluginAPI_t>(
//       GetProcAddress(GetModuleHandleW(nullptr), SkyrimTogetherAPI::kRequestPluginAPIName));
//   // nullptr: not running under Skyrim Together, stay inactive.
//   auto* pApi = static_cast<SkyrimTogetherAPI::IVSkyrimTogether1*>(pRequest(SkyrimTogetherAPI::InterfaceVersion::V1));
//   // nullptr from V2: this Skyrim Together build predates version 2.
//   auto* pApi2 = static_cast<SkyrimTogetherAPI::IVSkyrimTogether2*>(pRequest(SkyrimTogetherAPI::InterfaceVersion::V2));
//   // nullptr from V3: this Skyrim Together build predates version 3. Do without what it adds; V2 still works.
//   auto* pApi3 = static_cast<SkyrimTogetherAPI::IVSkyrimTogether3*>(pRequest(SkyrimTogetherAPI::InterfaceVersion::V3));

#include <cstddef>
#include <cstdint>

namespace SkyrimTogetherAPI
{
enum class InterfaceVersion : uint8_t
{
    V1 = 1,
    V2 = 2,
    V3 = 3
};

enum class SendResult : uint8_t
{
    Ok,           // Queued for sending. Delivery is not guaranteed.
    NotConnected, // Not connected to a server
    NotInParty,   // Not in a party, or the target is not in the local party
    TooLarge,     // Payload is longer than kMaxMessageSize (kMaxNearMessageSize for SendNear)
    BadChannel,   // Not a handle returned by RegisterChannel
    NotSupported, // V2: the server does not relay messages about actors (see IsNearSupported)
    NotOwner,     // V2: this client neither controls that actor nor is that player
    QueueFull     // SendNear: kMaxQueuedNearMessages reliable messages already wait on this channel; try again later
};

// How a message about an actor travels. Unreliable messages may be lost or arrive out of order, which suits a
// stream where each message replaces the previous one; they never hold up the messages behind them.
enum class Delivery : uint8_t
{
    Reliable,
    Unreliable
};

// A Mod Message is opaque bytes counted by length. It need not be text or null-terminated.
constexpr size_t kMaxMessageSize = 256;
// V2: the payload of SendNear fits one network packet; split larger data yourself.
constexpr size_t kMaxNearMessageSize = 1200;
// SendNear: messages about actors leave at most this many per second per channel, after an initial burst of up to
// kNearBurst; the rest wait their turn on this client (the server relays 60 per second, burst 120, and drops the
// rest). At most kMaxQueuedNearMessages reliable messages wait per channel.
constexpr double kNearMessagesPerSecond = 50.0;
constexpr double kNearBurst = 100.0;
constexpr size_t kMaxQueuedNearMessages = 2048;
// Channel names are 1 to kMaxChannelNameSize bytes of [A-Za-z0-9._-].
constexpr size_t kMaxChannelNameSize = 64;

using ChannelHandle = uint32_t; // 0 is invalid

// The callbacks run during Skyrim Together's per-frame update, where its own sync code runs. That is not
// necessarily the game's main thread, and the thread can differ between calls. To change game state from the
// main thread, hand the work over with SKSE's task interface (SKSE::GetTaskInterface()->AddTask).

// aSenderId is the player ID the server saw the message come from.
using MessageCallback = void (*)(uint32_t aSenderId, const char* apBytes, size_t aLength);
// A player joined (aJoined) or left the local player's party.
using PartyChangedCallback = void (*)(uint32_t aPlayerId, bool aJoined);
// V2: a message about the actor with server ID aServerId, sent by the player who controls it.
using NearMessageCallback = void (*)(uint32_t aSenderId, uint32_t aServerId, const char* apBytes, size_t aLength);

class IVSkyrimTogether1
{
public:
    virtual ~IVSkyrimTogether1() = default;

    // Returns 0 if the name is invalid or already registered. Channels live until the process exits.
    // Either callback may be null.
    virtual ChannelHandle RegisterChannel(const char* apName, MessageCallback aOnMessage, PartyChangedCallback aOnPartyChanged) noexcept = 0;

    // Sends to the local player's party, sender excluded. Callable from any thread.
    virtual SendResult Send(ChannelHandle aChannel, const char* apBytes, size_t aLength) noexcept = 0;

    // Sends to one member of the local player's party. Callable from any thread.
    virtual SendResult SendTo(ChannelHandle aChannel, uint32_t aPlayerId, const char* apBytes, size_t aLength) noexcept = 0;

    // 0 while not connected.
    virtual uint32_t GetLocalPlayerId() const noexcept = 0;
    virtual bool IsInParty() const noexcept = 0;
};

class IVSkyrimTogether2 : public IVSkyrimTogether1
{
public:
    // Sets the callback for messages about actors on a channel from RegisterChannel. False for a bad handle.
    virtual bool SetNearMessageCallback(ChannelHandle aChannel, NearMessageCallback aOnNearMessage) noexcept = 0;

    // Sends a message about the actor aServerId to every other player who has it in range, party or not. Only the
    // player controlling that actor (or the player that actor is) may send about it; the server enforces this.
    // Messages leave within the channel's budget (kNearMessagesPerSecond); beyond it they wait: reliable ones in order,
    // and of the unreliable ones only the newest per actor, actors taking turns. Callable from any thread.
    virtual SendResult SendNear(ChannelHandle aChannel, uint32_t aServerId, Delivery aDelivery, const char* apBytes, size_t aLength) noexcept = 0;

    // Sends to every other player on the server, party or not, reliably. Up to kMaxMessageSize, and rate limited
    // together with Send: meant for rare messages such as a session key, not for streams. Callable from any thread.
    virtual SendResult SendToAll(ChannelHandle aChannel, const char* apBytes, size_t aLength) noexcept = 0;

    // Whether the server this client is connected to relays SendNear. False while not connected, and for a short
    // while after connecting until the server has answered. Servers without it log nothing further.
    virtual bool IsNearSupported() const noexcept = 0;

    // The server ID of a synced actor, 0 if Skyrim Together does not sync it (yet). Callable from any thread; the
    // answer is as of the last frame.
    virtual uint32_t GetServerId(uint32_t aFormId) const noexcept = 0;
    // The form ID of the actor with that server ID on this client, 0 if it is not loaded here.
    virtual uint32_t GetFormId(uint32_t aServerId) const noexcept = 0;
    // Whether this client controls the actor with that server ID, so SendNear about it will be relayed.
    virtual bool IsOwnedLocally(uint32_t aServerId) const noexcept = 0;
};

// V3: one layer of a character's face tint (skin tone, warpaint, dirt, ...), as the game's TintMask holds it.
struct FaceTint
{
    float Alpha;
    // The TintMask's color as stored: red, green, blue and alpha bytes, in that order in memory.
    uint32_t Color;
    // The TintMask's type (skin tone, warpaint, ...).
    uint32_t Type;
    // The tint mask texture's path, as the game names it. Not kept: copied before the call returns.
    const char* Texture;
};
// V3: at most this many tints per face, and this long a texture path.
constexpr size_t kMaxFaceTints = 256;
constexpr size_t kMaxFaceTintTextureSize = 512;

// V3: another player whose character is loaded on this client.
struct PlayerInfo
{
    uint32_t PlayerId; // Skyrim Together's player ID, the same on every client
    uint32_t ServerId; // the server ID of that player's character
    uint32_t FormId;   // that character's form ID on this client
};

class IVSkyrimTogether3 : public IVSkyrimTogether2
{
public:
    // Paints these tints on the face of this client's copy of the actor with server ID aServerId (another player's
    // character, or an actor another player controls), the way Skyrim Together paints copies itself, and keeps them
    // for that copy from then on. For plugins that rebuild a copy's head, which drops the painted tints (loading a
    // RaceMenu preset does). Applied during the next update; dropped if aServerId is not a copy here by then. False for
    // bad arguments or while not connected. Callable from any thread.
    virtual bool SetFaceTints(uint32_t aServerId, const FaceTint* apTints, size_t aCount) noexcept = 0;

    // How many messages about actors wait for this channel's budget (reliable messages, plus one per actor with an
    // unreliable one waiting). For a plugin that streams: hold back while it grows. 0 for a bad handle. Callable from
    // any thread.
    virtual size_t GetNearQueued(ChannelHandle aChannel) const noexcept = 0;

    // Skyrim Together's network time in milliseconds, kept in step with the server, so the same moment has the same
    // tick on every client. 0 while not connected. Advances with the local clock between frames. Callable from any thread.
    virtual uint64_t GetNetworkTick() const noexcept = 0;

    // The network tick this client is showing the copy of actor aServerId at: copies play the owner's movement a little
    // behind, to smooth over the network. Stamp what you send about an actor with GetNetworkTick, and play it back on
    // the copy at this tick to stay in step with the copy's movement. False if aServerId is not a copy whose movement
    // plays here. As of the last frame, advanced with the local clock. Callable from any thread.
    virtual bool GetPlaybackTick(uint32_t aServerId, double* apTick) const noexcept = 0;

    // The other players whose characters are loaded on this client, as of the last frame (the local player is not
    // one of them). Writes up to aMax of them to apPlayers and returns how many there are: call with aMax 0 to count.
    // Callable from any thread.
    virtual size_t GetPlayers(PlayerInfo* apPlayers, size_t aMax) const noexcept = 0;

    // The server this client is connected to: its host name or address, without the port, into apHost (up to aSize
    // bytes with the terminating zero), and its port. False while not connected, or if apHost is too small. Callable
    // from any thread.
    virtual bool GetServerAddress(char* apHost, size_t aSize, uint16_t* apPort) const noexcept = 0;
};

// Exported by SkyrimTogether.exe under kRequestPluginAPIName. Returns nullptr for a version it does not know.
using RequestPluginAPI_t = void* (*)(InterfaceVersion aVersion);

// Not "RequestPluginAPI": many plugins look that name up in other mods (TrueHUD, True Directional Movement, NPCs Names
// Distributor) with GetProcAddress(GetModuleHandle("Mod.dll"), ...), and when the mod is missing the null handle
// means the main module. Under that name they got this interface instead and called its functions as their own.
constexpr const char* kRequestPluginAPIName = "SkyrimTogetherRequestPluginAPI";
} // namespace SkyrimTogetherAPI
