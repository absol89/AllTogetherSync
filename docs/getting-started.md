# Getting started

This guide takes you from nothing to a plugin that works between two players. The plugin is
[`examples/HelloTogether`](../examples/HelloTogether/main.cpp): press F8 and the players near you get a notification
that you waved. Once that works, you copy it and make it do what your mod needs.

You should already know some C++. You don't need to know anything about Skyrim Together's own code.

## How a plugin syncs

Your plugin is an ordinary SKSE plugin. Skyrim Together gives it a way to send small messages to the same plugin on
the other players' games:

1. Your plugin registers a **channel**, a name like `HelloTogether.v1`.
2. It **sends** bytes on that channel. Skyrim Together passes them through its server without reading them.
3. The same plugin on the other players' games **receives** them on the same channel and acts on them.

You choose who receives a message:

- **The players near an actor** (`SendNear`): everyone whose game has that actor loaded, party or not. This is the one
  you'll use most. "Something happened to this character", and the people who can see the character get told.
- **Your party** (`Send`, or `SendTo` for one member).
- **Everyone on the server** (`SendToAll`): only for rare messages.

Actors are named by their **server ID**, which is the same on every player's game, unlike form IDs. The API converts
between the two.

The server keeps nothing. A player who arrives later never gets the messages sent before. [Recipes](recipes.md)
shows how the four plugins deal with that.

## What you need

- Windows, and Visual Studio 2022 with the **Desktop development with C++** workload (the free Community edition
  is fine).
- [xmake](https://xmake.io), the build tool these plugins use. Install it, then check that `xmake --version` works in
  a new terminal.
- [git](https://git-scm.com). These steps use Git Bash, which comes with it.
- Skyrim Special Edition 1.6.1170 or 1.7.104 with SKSE and Address Library for SKSE Plugins, and Skyrim Together
  1.8.7-6 or newer from [absol89/TiltedEvolutionScriptFixes](https://github.com/absol89/TiltedEvolutionScriptFixes).
- Someone to test with: a second PC, or a friend with the same setup.

## 1. Get the code

Put this repository and CommonLibSSE-NG next to each other in one folder. CommonLibSSE-NG is the library SKSE plugins
use to talk to the game. These plugins need commit `b93280e8` with a small patch for Skyrim 1.7.x.

```sh
mkdir SkyrimPlugins && cd SkyrimPlugins
git clone https://github.com/absol89/AllTogetherSync
git clone https://github.com/CharmedBaryon/CommonLibSSE-NG
git -C CommonLibSSE-NG checkout b93280e8
git -C CommonLibSSE-NG apply ../AllTogetherSync/patches/commonlibsse-ng-skyrim-1.7.patch
```

You should now have `SkyrimPlugins/AllTogetherSync` and `SkyrimPlugins/CommonLibSSE-NG`.

Without the patch, plugins built here fail to start on Skyrim 1.7.x, with "failed to open address library file" in
their log.

## 2. Build the example

```sh
cd AllTogetherSync
xmake f -p windows -a x64 -m release
xmake build HelloTogether
```

The first build downloads and builds the libraries, which takes a few minutes. Later builds take seconds.

`-p windows -a x64` matters when you build from Git Bash: without it xmake picks the MinGW compiler, and the build
fails.

You get `build/windows/x64/release/HelloTogether.dll` and `HelloTogether.pdb`.

## 3. Install it on both games

Copy `HelloTogether.dll` and `HelloTogether.pdb` into `Data\SKSE\Plugins` on both PCs. With Mod Organizer 2, make a
new empty mod, create `SKSE\Plugins` inside it, and put the two files there.

## 4. Try it

1. Start Skyrim Together on both PCs, connect both to the same server, and load in near each other.
2. Press **F8** on one of them. The other one shows "*name* waves at you" in the top left corner.
3. Invite the other player to a party. When they join, both games show "Player *n* says hello".

If nothing happens, open `Documents\My Games\Skyrim Special Edition\SKSE\HelloTogether.log`:

| The log says | Meaning |
|---|---|
| no log at all | SKSE didn't load the plugin. Check the folder, and that SKSE itself runs (`skse64.log` next to it). |
| `Plugin API (V2) not found` | This Skyrim Together build has no Plugin API: use 1.8.7-6 or newer from the fork. |
| `Ready: press F8 to wave` | The plugin works. Check that both players are connected and close enough to see each other. |
| `Wave not sent: result 1` | Not connected (see `SendResult` in [The Plugin API](plugin-api.md#send-results)). |

## 5. Read the example

[`main.cpp`](../examples/HelloTogether/main.cpp) is about 170 lines. Read it top to bottom; the comments explain each
step. The parts that matter for every plugin are:

**Finding the API.** `FindApi()` looks up one function exported by `SkyrimTogether.exe`. If the game runs without
Skyrim Together, the function isn't there and the plugin does nothing. Your plugin should behave the same way: never
crash, never nag, just stay inactive.

**Registering a channel.** `RegisterChannel` takes the channel name and two callbacks: one for party messages, one for
party changes. `SetNearMessageCallback` adds the callback for messages about actors.

**Sending.** `Wave()` gets the server ID of your own character (form ID `0x14`) and sends `"wave"` about it with
`SendNear`.

**Receiving.** `OnWave` runs on Skyrim Together's thread, not the game's. It checks the message, then hands the work to
SKSE's task queue, which runs it on the game's main thread. There it finds the actor and shows the notification.

Never touch the game (actors, forms, the UI) directly in a callback. Copy what you need and use
`SKSE::GetTaskInterface()->AddTask`.

## 6. Make your own

1. Copy `examples/HelloTogether` to `src/YourPlugin`.
2. Copy the `HelloTogether` target in `xmake.lua`, and change its name, its files and its description.
3. Give the channel your plugin's name, with a version: `YourPlugin.v1`. When you change what your messages look like,
   raise the version, so old and new copies of your plugin don't misread each other.
4. Decide what to send, and to whom (see the questions below).
5. Build with `xmake build YourPlugin` and test with two players.

Questions to answer before you write code:

- **Which actor is it about?** Most often the player's own character. You can only send about actors your game
  controls: your character, and NPCs Skyrim Together has given to you. Check with `IsOwnedLocally`.
- **What happens when the message is lost?** Use `Delivery::Reliable` for events that must arrive ("the parkour move
  started"). Use `Delivery::Unreliable` for a stream where each message replaces the previous one (positions 15 times
  a second), so a lost one is replaced by the next.
- **What does a player see who arrives later?** If the answer is "nothing", you need the announce-and-ask pattern from
  [Recipes](recipes.md#players-who-arrive-later).
- **How often do you send?** At most 50 messages a second per channel get through. More wait in a queue; see
  [Limits](plugin-api.md#limits).
- **What if the other player doesn't have the mod you sync?** Check for it, write one line in your log, and skip just
  that part. Never crash because another player's setup differs from yours.

## Next

- [The Plugin API](plugin-api.md): every function, every limit.
- [Recipes](recipes.md): solutions to the problems you'll run into next.
- The four plugins in `src/` are complete, tested examples. Start with `src/SkyParkourTogether/main.cpp`, the
  smallest.
