# Changing Skyrim Together

Most plugins never need this: the Plugin API lets them send whatever they want to whoever can see it. But some things
only Skyrim Together itself knows or can do. Face tints are one example: Skyrim Together paints them on copies, so a
plugin that rebuilds a copy's head needed a way to have them repainted. That became `SetFaceTints` in V3.

When you hit such a limit, don't patch or replace `SkyrimTogether.exe` from your mod. Every player would need your exe,
it would break with each Skyrim Together update, and two mods doing it can't be installed together. Add what you need
to the API instead, through a pull request to
[absol89/TiltedEvolutionScriptFixes](https://github.com/absol89/TiltedEvolutionScriptFixes). Once it's in a build,
every plugin can use it.

## First: open an issue

Before writing code, open an issue on the fork. Describe what your plugin does, and what it needs from Skyrim Together
that it can't get. Often there's a way with the existing API, and if not, it's quicker to agree on the shape of a new
function before you build it.

## Where the API lives

All paths are in [absol89/TiltedEvolutionScriptFixes](https://github.com/absol89/TiltedEvolutionScriptFixes), branch
`1.8-playtest`:

| Path | What's there |
|---|---|
| `Code/plugin_api/include/SkyrimTogetherAPI.h` | The header plugins copy. The interface, constants and limits. |
| `Code/client/PluginAPI.cpp` | The exported `SkyrimTogetherRequestPluginAPI`, which hands out each version. |
| `Code/client/Services/PluginService.h`, `Code/client/Services/Generic/PluginService.cpp` | The implementation on the client: channels, sending, pacing, the questions about actors and players. |
| `Code/server/Services/ModMessageService.cpp` | The server's relay for party messages and `SendToAll`, with its checks and rate limit. |
| `Code/server/Services/AddonNearService.cpp` | The server's relay for `SendNear`: ownership check, range, rate limit. |
| `Code/encoding/Messages/` | The network messages: `SendModMessageRequest`, `NotifyModMessage`, `SendAddonNearRequest`, `NotifyAddonNear`, `AddonHelloRequest`, `NotifyAddonCapabilities`. |
| `Code/tests/encoding.cpp` | Tests that the network messages serialize and deserialize. |

## Adding a function: a new version

Plugins are compiled against a version of the interface. If you change a published version's functions (add one in the
middle, reorder them, change a signature), every plugin built against it calls the wrong function, and the game
crashes. So **V1, V2 and V3 never change.** New functions go into a new version:

1. In `SkyrimTogetherAPI.h`, add `V4 = 4` to `InterfaceVersion`, and a new class that extends the last one:

   ```cpp
   class IVSkyrimTogether4 : public IVSkyrimTogether3
   {
   public:
       // What it does, which thread may call it, and what it returns when it can't.
       virtual bool YourFunction(uint32_t aServerId) noexcept = 0;
   };
   ```

   Several new functions in one pull request can share one new version.
2. Make `PluginService` derive from `IVSkyrimTogether4` and implement the functions.
3. In `PluginAPI.cpp`, add `case InterfaceVersion::V4:` returning the service as `IVSkyrimTogether4*`.
4. Update the header comment at the top of the file, which lists what each version adds.
5. Copy the header to [AllTogetherSync](https://github.com/absol89/AllTogetherSync) `include/`, and add the
   function to `docs/plugin-api.md` there.

Rules for new functions, the same ones the existing functions follow:

- **Never throw, never crash on bad input.** Mark every function `noexcept`. Check every pointer and size you're given,
  and return a failure value instead.
- **Say which thread may call it.** Sending and questions work from any thread; lock what you share with Skyrim
  Together's update.
- **Don't keep the plugin's pointers.** Copy strings and buffers before returning.
- **Treat other players as untrusted.** Anything that arrives over the network can come from a modified client. The
  server checks sizes, ownership and rate limits; the client checks what it acts on.

## Adding a network message

Messages between the client and the server live in `Code/encoding/Messages/`, and both sides must agree on them. That
works because players and the server always run the same build: the server turns away clients from another build.

If your change needs a new message, follow an existing pair, such as `SendAddonNearRequest` and `NotifyAddonNear`, and
add a test to `Code/tests/encoding.cpp`. Every server-side handler must check what it receives as if a hostile client
sent it.

## Building and testing

The easiest way is the fork's own CI. Fork the repository on GitHub, enable Actions on your fork (GitHub turns them off
for new forks), then open **Actions → Playable Skyrim Together Build → Run workflow** and pick your branch. It builds the client for Skyrim 1.7.104 and 1.6.1170, plus the server, and
attaches them to the run. Install the client on two PCs, run the server from the same run, and test with your plugin.

To build locally, follow upstream's
[build guide](https://wiki.tiltedphoques.com/tilted-online/technical-documentation/build-guide).

## The pull request

Open it against the `1.8-playtest` branch. Say:

- what your plugin needs and why the existing API couldn't do it (link the issue);
- what you added, and which version it's in;
- how you tested it: which runtimes, how many players, whether you ran a server from the same build.

Fixes to Skyrim Together that aren't about the API, and that help every player, are also welcome upstream at
[tiltedphoques/TiltedEvolution](https://github.com/tiltedphoques/TiltedEvolution), on its `dev` branch.
