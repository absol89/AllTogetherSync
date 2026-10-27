# AllTogetherSync

Make your Skyrim mods work in multiplayer: SKSE plugins that sync through Skyrim Together, without patching or
replacing Skyrim Together itself.

This repository has two things:

- **Four ready-made plugins** that sync popular mods between Skyrim Together players: RaceMenu appearances,
  SkyParkour moves, ragdolls, and LipSync mouths and voices.
- **Everything you need to write your own:** the Plugin API header, a small example plugin, and guides.

The Plugin API itself is built into Skyrim Together's launcher, `SkyrimTogether.exe`, in the builds from
[absol89/TiltedEvolutionScriptFixes](https://github.com/absol89/TiltedEvolutionScriptFixes). If your plugin needs
something from Skyrim Together that the API can't do yet, that's where the change goes: see
[Changing Skyrim Together](docs/changing-skyrim-together.md).

## For players

### The plugins

| Plugin | What other players see | You also need |
|---|---|---|
| `RaceMenuTogether.dll` | Your RaceMenu look: sculpts, sliders, body morphs, height and bone scales, overlays, face tints | RaceMenu |
| `SkyParkourTogether.dll` | Your vaults, climbs and ledge grabs | SkyParkour |
| `RagdollTogether.dll` | Bodies falling the same way they fell for the owner | – |
| `LipSyncTogether.dll` | Your mouth moving while you talk, and your voice | Mfg Fix NG; SKSE Menu Framework for the settings window |

Install only the ones you want. Each one works on its own.

**Everyone in the session needs the same plugins, and the same mods behind them.** A RaceMenu preset only names the
meshes, textures and morphs it uses; it doesn't carry them. If you use a body mod, a hair mod or BodySlide morphs,
the other players need them too, built the same way (in BodySlide, tick "Build Morphs").

If a player is missing something, only that part is skipped on their side (the plugin's log says so), and everything
else keeps working.

### Requirements

- Skyrim Special Edition **1.6.1170** or **1.7.104**, with SKSE and Address Library for SKSE Plugins.
- Skyrim Together from [absol89/TiltedEvolutionScriptFixes](https://github.com/absol89/TiltedEvolutionScriptFixes),
  **1.8.7-6 or newer**. Official Skyrim Together releases don't have the Plugin API; on them these plugins stay
  inactive.
- Nothing on the server. Any server running the same Skyrim Together build works.

### Installing

Install the release zip with your mod manager like any SKSE plugin. It puts the DLLs in `Data\SKSE\Plugins` and the
license and notices in `Data\Docs\TogetherSync`. Nothing replaces Skyrim Together's own files.

Don't use these together with mailamea10's AllTogether bundle or its `SkyrimTogether.exe`. These plugins replace that
bundle's add-ons, and both would sync the same things.

### Checking that it works

Each plugin writes a log next to SKSE's own, in `Documents\My Games\Skyrim Special Edition\SKSE`, for example
`RaceMenuTogether.log`. The first lines say whether it found Skyrim Together's Plugin API and what it needs. If
something doesn't sync, that log says why.

The plugins don't fail silently. If one of them hits an error it can't recover from, it crashes on purpose so your
crash logger records where. If the game stops responding for 20 seconds while you're connected, they write a minidump
next to the logs (at most 3 per game start). Please attach both to bug reports.

### Other plugins on this API

- [STCampfireSync](https://github.com/DJLegends1011/STCampfireSync) by DJLegends: places campfires for your whole
  party. It was the first plugin written for the Plugin API. Builds 1.8.7-5 and newer need the version from
  [absol89/STCampfireSync](https://github.com/absol89/STCampfireSync/tree/fix/renamed-plugin-api-export), which looks
  up the API's new export name.

Made one? Open an issue or pull request to add it here.

## For modders

Start with [Getting started](docs/getting-started.md). It takes you from an empty folder to a plugin that works
between two players.

| Guide | What it covers |
|---|---|
| [Getting started](docs/getting-started.md) | Tools, building, the example plugin, testing with two players |
| [The Plugin API](docs/plugin-api.md) | Channels, who receives what, threads, limits, versions, every function |
| [Recipes](docs/recipes.md) | Solutions from the four plugins: late joiners, streams, Papyrus calls, 1.6 and 1.7 differences |
| [Changing Skyrim Together](docs/changing-skyrim-together.md) | When the API isn't enough, and how to add to it |

What's where:

```
include/SkyrimTogetherAPI.h    The Plugin API. Copy it into your plugin; it needs nothing else.
examples/HelloTogether/        The example plugin: wave at the players near you.
src/Common/                    Shared code of the four plugins: finding the API and the Papyrus VM, crash safety.
src/RaceMenuTogether/          The four plugins. Each one is a complete example of a different kind of sync.
src/SkyParkourTogether/
src/RagdollTogether/
src/LipSyncTogether/
patches/                       The CommonLibSSE-NG change needed for Skyrim 1.7.x.
dist/                          Files shipped next to the DLLs (LipSync's ini, third-party notices).
```

`include/SkyrimTogetherAPI.h` is a copy. The original is
[`Code/plugin_api/include/SkyrimTogetherAPI.h`](https://github.com/absol89/TiltedEvolutionScriptFixes/blob/1.8-playtest/Code/plugin_api/include/SkyrimTogetherAPI.h)
in the Skyrim Together fork, and the two are kept identical.

## Building

You need Visual Studio 2022 with the "Desktop development with C++" workload, [xmake](https://xmake.io) and git. Put a
[CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) checkout next to this repository, at commit
`b93280e8`, with the 1.7 patch applied:

```sh
git clone https://github.com/CharmedBaryon/CommonLibSSE-NG ../CommonLibSSE-NG
git -C ../CommonLibSSE-NG checkout b93280e8
git -C ../CommonLibSSE-NG apply "$PWD/patches/commonlibsse-ng-skyrim-1.7.patch"
```

Then build in this folder. From Git Bash keep `-p windows -a x64`, or xmake picks MinGW:

```sh
xmake f -p windows -a x64 -m release
xmake build HelloTogether
xmake build RaceMenuTogether
```

The DLLs and their PDBs land in `build/windows/x64/release/`. Ship the PDB next to the DLL: crash logs then name your
functions instead of showing bare addresses.

## Credits and license

The four plugins are based on mailamea10's Together add-ons
([mailamea10/TiltedEvolution-dwmapi.dll](https://github.com/mailamea10/TiltedEvolution-dwmapi.dll)), which first
brought these features to Skyrim Together, and on the Plugin API V2 she designed. absol89 rebuilt them as standalone
plugins and extended the API. RaceMenu is by expired6978; SkyParkour, Mfg Fix NG and SKSE Menu Framework are by their
own authors. These plugins only call those mods and include none of their files. The HelloTogether example follows
the layout of DJLegends' STCampfireSync, the first Plugin API example.

GNU General Public License v3 (`LICENSE`), like Skyrim Together and mailamea10's add-ons. What else is built into the
DLLs, and under which license, is in `dist/THIRD-PARTY-NOTICES.txt`. A release zip carries both in
`Docs/TogetherSync/`, and its source is published next to it as `TogetherSync-<version>-source.zip`.
