# Recipes

Problems you'll run into when syncing a mod, and how the four plugins in `src/` solve them. Each section names the
code to read.

- [Players who arrive later](#players-who-arrive-later)
- [Sending more than one message holds](#sending-more-than-one-message-holds)
- [Replaying an event](#replaying-an-event)
- [Streaming movement](#streaming-movement)
- [Calling Papyrus functions](#calling-papyrus-functions)
- [Skyrim 1.6 and 1.7 are not the same](#skyrim-16-and-17-are-not-the-same)
- [When another player's setup differs](#when-another-players-setup-differs)
- [Failing loudly](#failing-loudly)

## Players who arrive later

The server stores nothing. If you send your appearance once when you join, a player who walks into range a minute
later never sees it.

**RaceMenuTogether** solves this with three kinds of message ([`src/RaceMenuTogether/main.cpp`](../src/RaceMenuTogether/main.cpp)):

| Message | Sent | Delivery | Meaning |
|---|---|---|---|
| `A\|<revision>` | every 5 s, about your character | unreliable | "My appearance is at revision 7." |
| `Q\|<server id>\|<revision>` | when someone in range announces a revision you don't have | reliable | "Please send me revision 7." |
| `D\|<revision>\|<piece>\|<pieces>\|<bytes>` | in answer to a request | reliable | One piece of the appearance. |

The revision goes up each time the appearance changes (RaceMenu closes). Everyone in range hears the announcements,
so a newcomer learns within 5 seconds that there's something to fetch, and asks for it. Players who already have the
current revision ignore the announcements; they cost one tiny message every 5 seconds.

A request is a message about the asking player's own character (it can only send about actors it controls), which
puts the requested server ID in the message body.

Use this whenever the state is "what something looks like now" rather than "what just happened".

## Sending more than one message holds

`SendNear` carries up to 1200 bytes. A RaceMenu preset is often 50–300 KB.

RaceMenuTogether compresses the preset with zlib, cuts it into 1100-byte pieces (leaving room for the header), and
numbers them. The receiver collects pieces per sender and revision, and only uses the data when all of them have
arrived. It also caps the total (200 pieces, 16 MB unpacked), so a broken or hostile sender can't make it allocate
without limit.

The pieces leave at 40 a second at most. That stays under the channel's budget of 50 a second, and leaves room for the
announcements and requests on the same channel. A newer appearance replaces the pieces of an older one that haven't
gone out yet.

## Replaying an event

Something happens on the owner's game once, and should happen on the copies once: an animation starting, a sound, a
spell effect.

**SkyParkourTogether** ([`src/SkyParkourTogether/main.cpp`](../src/SkyParkourTogether/main.cpp)) watches the player's
animation graph for SkyParkour's events. When one fires, it reads the graph variables that pick the animation (ledge
height, grab variant...) and sends the event and the variables as one reliable message:

```
SkyParkourClimb|i:SkyParkourLedge:3|b:SkyParkourGrabVariant:0|f:SkyParkourSpeedMult:1.000
```

The other games set the same variables on their copy of that player, then send it the same event. It's the smallest of
the four plugins, and the best one to read first.

Send everything needed to replay the event in the same message. Two messages can be paced apart, and the second could
arrive a moment after the event already played with the wrong values.

## Streaming movement

Something changes every frame and the copies should follow smoothly: a ragdoll falling, a mouth moving.

**RagdollTogether** ([`src/RagdollTogether/main.cpp`](../src/RagdollTogether/main.cpp)):

- Samples the bones 30 times a second and sends 15 times a second, **unreliably**. A lost pose is replaced by the next
  one anyway, and unreliable messages never wait behind a lost one.
- Sends the final pose, "now at rest", **reliably**: if that one were lost, the copy would hang in mid-fall.
- Stamps each pose with `GetNetworkTick()` from when it was sampled.
- On the copies, plays the poses at `GetPlaybackTick(serverId)` minus a small delay. Skyrim Together shows each copy a
  little in the past to smooth over the network, and this puts the body in the same moment as the copy's own movement,
  so the body doesn't fall ahead of the character or lag behind it. The delay leaves time for the next pose to arrive,
  so there's always one to blend towards.

**LipSyncTogether** ([`src/LipSyncTogether/Host.cpp`](../src/LipSyncTogether/Host.cpp)) sends the mouth's state
unreliably, at most 30 times a second. While it doesn't change, the state is repeated every 0.2 s to say "still
talking", and a copy that hears nothing for 0.6 s closes its mouth. A timeout like that is what keeps a lost "stop"
message from leaving a mouth open forever.

Streaming from many actors at once is fine. RagdollTogether can have a dozen bodies falling in a fight; the channel's
budget then sends each body less often, newest pose first, instead of dropping messages at random. If you stream
reliable messages, watch `GetNearQueued(channel)` and send less while it grows.

## Calling Papyrus functions

Many mods only offer their features to Papyrus scripts: RaceMenu's `CharGen` and `NiOverride`, Mfg Fix's
`MfgConsoleFunc`. You can call them from C++ by queuing the call on the game's script engine, just as a script would:

```cpp
auto* pVm = Together::GetVirtualMachine(); // not RE::BSScript::Internal::VirtualMachine::GetSingleton(), see below
if (!pVm)
    return false;
auto* pArguments = RE::MakeFunctionArguments(static_cast<RE::Actor*>(pActor), std::int32_t{0}, std::int32_t{id}, std::int32_t{value});
RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new MyCallback());
pVm->DispatchStaticCall("MfgConsoleFunc", "SetPhonemeModifier", pArguments, callback);
```

`DispatchStaticCall` returns false if that script or function doesn't exist (the mod isn't installed). The call runs
later on a script thread; the callback gets its result there. Use `GetVirtualMachine()` from
[`src/Common/Together.h`](../src/Common/Together.h): CommonLib's own `GetSingleton()` returns the wrong object on Skyrim
1.7.x (next section).

This is safer than calling a mod's native functions directly through pointers: the script engine checks the call, and
it works whichever way the mod registered its functions.

## Skyrim 1.6 and 1.7 are not the same

CommonLibSSE-NG describes the game's objects as they are in Skyrim 1.6.640. On 1.6.1170 and 1.7.104 most objects are
laid out the same, but not all, and CommonLib doesn't know. Reading a field it has at the wrong offset doesn't crash
right away: you get another field's value. That's worse, because nothing tells you.

Known differences:

| What | 1.6.1170 | 1.7.104 | CommonLib reads |
|---|---|---|---|
| The script engine in `SkyrimVM` | `+0x200` | `+0x210` | `+0x200` |
| The player's face tint masks in `PlayerCharacter` | `+0xB18` | `+0xB20` | `+0xB10` |

`Together::GetVirtualMachine()` picks the right offset for the runtime, then checks the game's own type information
before using what it finds. RaceMenuTogether's `ReadPlayerTints` reads the tints from the right offset per runtime.

Before you read a game object's fields directly, check them against Skyrim Together's own definitions in the fork,
under `Code/client/Games/Skyrim/`. They run on both runtimes and are checked with `static_assert`s. The only files with
different layouts per runtime are the ones containing `TP_SKYRIM_1_6_1170`. Calling the game's functions (through
Address Library IDs) and virtual functions is generally safe on both. Reading fields is where it goes wrong.

## When another player's setup differs

Players don't all have the same mods. Your plugin must not break the session for everyone because one player is
missing something.

The four plugins follow one rule: **sync what matches, skip only what's missing.**

- Check for each thing you depend on (a mod's DLL, a Papyrus script, an animation, a newer API version) when you first
  need it.
- If it's missing, write one line in your log saying what's skipped and why, and skip just that part. Don't log it
  again every frame.
- Keep everything else working. RaceMenuTogether without Plugin API V3 still syncs appearances, just without face
  tints. LipSyncTogether without Mfg Fix NG still sends your mouth to others, but can't move mouths on your game.

## Failing loudly

A plugin that silently stops working, or hangs the game, costs players hours of guessing. A crash with a log names the
problem.

[`src/Common/Safety.h`](../src/Common/Safety.h) gives the four plugins:

- **`InstallTerminateHandler`**: a C++ exception nobody catches crashes the game on purpose, at the point where it was
  thrown, so a crash logger (Crash Logger SSE, for example) records the plugin and function.
- **`StartFreezeWatchdog`**: if the game's main thread stops for 20 seconds while connected, it writes a minidump next
  to the plugin's logs (at most 3 per game start). Freezes then leave evidence instead of a frozen screen.
- **`Fail`**: logs a message and crashes deliberately, for states the plugin can't continue from.

Ship your PDB file next to your DLL. Crash logs then show your function names and line numbers.
