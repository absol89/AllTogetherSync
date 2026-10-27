# The Plugin API

Everything Skyrim Together's Plugin API does, in one place. The header,
[`include/SkyrimTogetherAPI.h`](../include/SkyrimTogetherAPI.h), has the same information next to each declaration.

- [Getting the API](#getting-the-api)
- [Channels](#channels)
- [Who receives a message](#who-receives-a-message)
- [Server IDs and ownership](#server-ids-and-ownership)
- [Threads](#threads)
- [Limits](#limits)
- [Send results](#send-results)
- [Versions](#versions)
- [Function reference](#function-reference)

## Getting the API

`SkyrimTogether.exe` exports one function, `SkyrimTogetherRequestPluginAPI`. Ask it for the version you need:

```cpp
#include "SkyrimTogetherAPI.h"
namespace STAPI = SkyrimTogetherAPI;

const auto pRequest = reinterpret_cast<STAPI::RequestPluginAPI_t>(
    GetProcAddress(GetModuleHandleW(nullptr), STAPI::kRequestPluginAPIName));
auto* pApi = pRequest ? static_cast<STAPI::IVSkyrimTogether2*>(pRequest(STAPI::InterfaceVersion::V2)) : nullptr;
```

- No `pRequest`: the game isn't running under Skyrim Together, or under a build without the API. Stay inactive.
- `pRequest` returns null: this build doesn't have that version. Ask for a lower one, or stay inactive.

Do this in `SKSEPluginLoad`. The object you get lives until the game exits; don't delete it.

Look up only `SkyrimTogetherRequestPluginAPI`. Older builds exported it as `RequestPluginAPI`, a name other mods also
look up in other DLLs. When those DLLs were missing, those mods got Skyrim Together's interface instead, called it as
their own, and froze the game. Builds since 1.8.7-5 use the new name only.

## Channels

A channel is a name your plugin owns. Messages sent on it arrive only at the same channel on the other players'
games.

```cpp
STAPI::ChannelHandle channel = pApi->RegisterChannel("YourPlugin.v1", OnPartyMessage, OnPartyChanged);
pApi->SetNearMessageCallback(channel, OnNearMessage);
```

- Names are 1 to 64 characters of `A-Z a-z 0-9 . _ -`. `RegisterChannel` returns 0 for an invalid name, or one that's
  already registered.
- Use your plugin's name, plus a version you raise when your message format changes. Players with different versions
  then simply don't hear each other, instead of misreading each other's messages.
- Either callback may be null. Channels last until the game exits.
- A plugin can register several channels, for example one for rare events and one for a stream, so that the stream
  doesn't use up the events' budget.

A message is just bytes, up to a size limit. It doesn't need to be text, and it isn't null-terminated. Treat what you
receive like input from a stranger: check its size and contents before using it. A player with a modified client can
send anything.

## Who receives a message

| Function | Goes to | Reliable | Max size | Version |
|---|---|---|---|---|
| `SendNear` | Every other player who has that actor loaded, party or not | your choice | 1200 bytes | V2 |
| `Send` | Every other member of your party | yes | 256 bytes | V1 |
| `SendTo` | One member of your party | yes | 256 bytes | V1 |
| `SendToAll` | Every other player on the server | yes | 256 bytes | V2 |

**"Has that actor loaded"** means the same range in which Skyrim Together syncs actors at all: the other player's game
has that actor's cell loaded. If they can see the actor, they get the message. This is what you want for anything
visible, such as an appearance, an animation or a sound.

**Party messages** suit things that belong to the group rather than a place, such as a shared camp or a quest choice.

**`SendToAll`** is for rare things, such as handing out a session key. It shares the party messages' rate limit.

Nothing is stored. A message reaches the players who qualify at the moment it's relayed, and nobody else, ever.

**Reliable** messages arrive, in the order they were sent. **Unreliable** ones (`SendNear` with
`Delivery::Unreliable`) may be lost or arrive out of order, but they never wait behind lost ones. Use unreliable for a
stream where each message replaces the previous one.

## Server IDs and ownership

Form IDs of actors differ between players' games (anything spawned at runtime gets a different `0xFF......` ID on each
one). Skyrim Together gives every actor it syncs a **server ID**, the same on every game. Messages about actors use
server IDs:

- `GetServerId(formId)`: the server ID of an actor on your game, or 0 if Skyrim Together doesn't sync it.
- `GetFormId(serverId)`: which actor that is on your game, or 0 if it isn't loaded here.

Every synced actor is **controlled** by one player's game: the player's own character by that player, and each NPC by
whichever player Skyrim Together gave it to. Only that game may send `SendNear` about it; the server drops anything
else. Check with `IsOwnedLocally(serverId)` before sending. NPCs change hands as players move around, so check every
time, not once.

The other players' characters on your game are **copies**: actors Skyrim Together moves to match the real ones.
`GetPlayers` (V3) lists them.

## Threads

- **Callbacks** run during Skyrim Together's per-frame update. That's not necessarily the game's main thread, and it
  can change between calls. Don't touch the game from there: copy the message and use
  `SKSE::GetTaskInterface()->AddTask(...)` to run the rest on the main thread.
- **Sending** (`Send`, `SendTo`, `SendNear`, `SendToAll`) works from any thread.
- **Questions** (`GetServerId`, `GetFormId`, `IsOwnedLocally`, `GetPlayers`, ...) work from any thread. They answer as
  of the last frame.
- Keep callbacks short. They run inside Skyrim Together's update, and a slow one slows down syncing for everything.

## Limits

| | Party messages (`Send`, `SendTo`, `SendToAll`) | Messages about actors (`SendNear`) |
|---|---|---|
| Size | 256 bytes | 1200 bytes |
| Counted | per player, all channels together | per player and channel |
| Server limit | 20 per second, burst 40 | 60 per second, burst 120 |
| Over the limit | **dropped by the server, silently** | paced on your side: they wait, see below |
| Other | | at most 32 channels per player |

`SendNear` paces itself so the server never drops anything. Each channel sends at most 50 messages a second, after a
burst of up to 100. The rest wait on your game:

- Reliable messages wait in order, up to 2048 per channel. Beyond that, `SendNear` returns `QueueFull`.
- Of the unreliable ones, only the newest per actor waits. An older one waiting for the same actor is replaced, which
  is what a stream wants.
- `GetNearQueued(channel)` (V3) says how many are waiting. If it keeps growing, send less.

Party messages are not paced on your side. Stay well under 20 a second, or the server drops the excess without telling
you.

For data larger than one message, split it into pieces and put it back together on arrival (see
[Recipes](recipes.md#sending-more-than-one-message-holds)).

## Send results

| Result | Meaning |
|---|---|
| `Ok` | Queued for sending. With reliable delivery, it will arrive to whoever qualifies when it's relayed. |
| `NotConnected` | Not connected to a server. |
| `NotInParty` | `Send` or `SendTo`: you're not in a party, or the target isn't in yours. |
| `TooLarge` | Over 256 bytes (1200 for `SendNear`). |
| `BadChannel` | Not a handle from `RegisterChannel`. |
| `NotSupported` | `SendNear`: the server doesn't relay messages about actors (see `IsNearSupported`). |
| `NotOwner` | `SendNear`: your game doesn't control that actor. |
| `QueueFull` | `SendNear`: 2048 reliable messages already wait on this channel. Try again later. |

## Versions

| Version | Adds | First build |
|---|---|---|
| V1 | Channels, party messages | 1.8.7-5 under this export name |
| V2 | Messages about actors (`SendNear`), `SendToAll`, server IDs and ownership | 1.8.7-5 |
| V3 | Face tints for copies, queue size, network and playback time, players in range, server address | 1.8.7-6 |

Each version contains the ones before it: an `IVSkyrimTogether3*` is also an `IVSkyrimTogether2*`.

Ask for the lowest version you need. If you can use V3 but don't need it, ask for both, and do without the V3 parts
when it returns null:

```cpp
auto* pApi = static_cast<STAPI::IVSkyrimTogether2*>(pRequest(STAPI::InterfaceVersion::V2));
auto* pApi3 = static_cast<STAPI::IVSkyrimTogether3*>(pRequest(STAPI::InterfaceVersion::V3)); // may be null
```

Published versions never change, so a plugin built today keeps working with later Skyrim Together builds. New
functions come as a new version (V4, ...).

## Function reference

### V1

**`RegisterChannel(name, onMessage, onPartyChanged)`** → `ChannelHandle`
Registers a channel; see [Channels](#channels). Returns 0 for an invalid or taken name.
- `onMessage(senderId, bytes, length)`: a party message, or a `SendToAll` message, arrived. `senderId` is the player
  the server received it from.
- `onPartyChanged(playerId, joined)`: a player joined (`true`) or left (`false`) your party. When you join a party,
  you get one call for each member already in it.

**`Send(channel, bytes, length)`** → `SendResult`
To every other member of your party.

**`SendTo(channel, playerId, bytes, length)`** → `SendResult`
To one member of your party.

**`GetLocalPlayerId()`** → `uint32_t`
Your player ID, the same one other players see as `senderId`. 0 while not connected.

**`IsInParty()`** → `bool`

### V2

**`SetNearMessageCallback(channel, onNearMessage)`** → `bool`
Sets the callback for messages about actors on this channel: `onNearMessage(senderId, serverId, bytes, length)`.
`serverId` is the actor the message is about; `senderId` the player whose game controls it.

**`SendNear(channel, serverId, delivery, bytes, length)`** → `SendResult`
About the actor `serverId`, to every other player who has it loaded. Your game must control the actor. Paced, see
[Limits](#limits).

**`SendToAll(channel, bytes, length)`** → `SendResult`
To every other player on the server. It arrives at their `onMessage`, like a party message. Only for rare messages.

**`IsNearSupported()`** → `bool`
Whether the server relays `SendNear`. False while not connected, and for a moment after connecting until the server
has answered.

**`GetServerId(formId)`** → `uint32_t`, **`GetFormId(serverId)`** → `uint32_t`, **`IsOwnedLocally(serverId)`** → `bool`
See [Server IDs and ownership](#server-ids-and-ownership).

### V3

**`SetFaceTints(serverId, tints, count)`** → `bool`
Paints face tints (skin tone, warpaint, dirt...) on your game's copy of that actor, the way Skyrim Together paints
copies itself, and keeps them for that copy from then on. Use it if your plugin rebuilds a copy's head, which drops the
tints Skyrim Together painted (loading a RaceMenu preset does this). Applied during the next frame. At most 256 tints,
texture paths up to 512 characters. The texture strings are copied before the call returns.

**`GetNearQueued(channel)`** → `size_t`
How many messages about actors wait for this channel's budget: the reliable ones, plus one per actor with an
unreliable one waiting.

**`GetNetworkTick()`** → `uint64_t`
Skyrim Together's clock in milliseconds, kept in step with the server, so the same moment has the same tick on every
game. 0 while not connected.

**`GetPlaybackTick(serverId, &tick)`** → `bool`
Your game shows copies a little in the past, to smooth over the network. This is the network tick your game is
showing that copy at. Stamp what you send with `GetNetworkTick()`, then play it on the copy when its playback tick
reaches that stamp, and it lines up with the copy's movement. False if `serverId` isn't a copy moving on your game.

**`GetPlayers(players, max)`** → `size_t`
The other players whose characters are loaded on your game: player ID, server ID and form ID of each. Writes up to
`max` of them and returns how many there are; call with `max` 0 to count.

**`GetServerAddress(host, size, &port)`** → `bool`
The host name or address you're connected to, and the port. For plugins that open their own connection to the same
machine. False while not connected, or if `host` is too small.
