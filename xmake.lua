set_xmakever("2.8.2")
set_project("TogetherSync")
set_version("2.0.2")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.release")

-- Release builds keep their PDBs: ship them next to the DLLs so crash logs name this code's functions.
if is_mode("release") then
    set_symbols("debug")
end

-- CommonLibSSE-NG checkout next to this repo (with STCampfireSync's patch for Skyrim 1.7.x applied).
includes("../CommonLibSSE-NG")

add_requires("zlib", "minhook", "libopus")

-- The example from docs/getting-started.md: wave at the players near you. Start new plugins from this one.
target("HelloTogether")
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "HelloTogether",
        author = "absol89",
        description = "Example Skyrim Together plugin: wave at the players near you (Plugin API V2)",
        options = { address_library = true }
    })
    add_files("examples/HelloTogether/*.cpp")
    add_includedirs("include")
    -- Warnings from CommonLib's own headers.
    add_cxxflags("cl::/wd4100", "cl::/wd4200", "cl::/wd4201")
target_end()

-- SkyParkour's animations on the other players, through Skyrim Together's Plugin API V2.
target("SkyParkourTogether")
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "SkyParkourTogether",
        author = "mailamea10, absol89",
        description = "Shares SkyParkour animations between Skyrim Together players (Plugin API V2)",
        options = { address_library = true }
    })
    add_files("src/SkyParkourTogether/*.cpp")
    add_includedirs("include", "src")
    add_syslinks("dbghelp")
    add_cxxflags("cl::/wd4100", "cl::/wd4200", "cl::/wd4201")
target_end()

-- RaceMenu appearances on the other players, through Skyrim Together's Plugin API V2 (V3 for face tints).
target("RaceMenuTogether")
    add_deps("commonlibsse-ng")
    add_packages("zlib")
    add_rules("commonlibsse-ng.plugin", {
        name = "RaceMenuTogether",
        author = "mailamea10, absol89",
        description = "Shares RaceMenu appearances between Skyrim Together players (Plugin API V2/V3)",
        options = { address_library = true }
    })
    add_files("src/RaceMenuTogether/*.cpp")
    add_includedirs("include", "src")
    add_syslinks("dbghelp")
    add_cxxflags("cl::/wd4100", "cl::/wd4200", "cl::/wd4201")
target_end()

-- Ragdolls on the other players, through Skyrim Together's Plugin API V3.
target("RagdollTogether")
    add_deps("commonlibsse-ng")
    add_packages("minhook")
    add_rules("commonlibsse-ng.plugin", {
        name = "RagdollTogether",
        author = "mailamea10, absol89",
        description = "Shares ragdolls between Skyrim Together players (Plugin API V3)",
        options = { address_library = true }
    })
    add_files("src/RagdollTogether/*.cpp")
    add_includedirs("include", "src")
    add_syslinks("dbghelp")
    add_cxxflags("cl::/wd4100", "cl::/wd4200", "cl::/wd4201")
target_end()

-- DJLegends' campfire sync, the first Plugin API example (V1, party messages). It keeps its own version.
target("STCampfireSync")
    set_version("0.1.0")
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "STCampfireSync",
        author = "DJLegends",
        description = "Campfire sync example for the Skyrim Together Plugin API",
        options = { address_library = true }
    })
    add_files("src/STCampfireSync/*.cpp")
    add_includedirs("include")
    add_cxxflags("cl::/wd4100", "cl::/wd4200", "cl::/wd4201")
target_end()

-- Mouths (and voices) of the other players, through Skyrim Together's Plugin API V2/V3. LipSync Together 0.8's own
-- microphone and voice code is in plugin/; its settings window uses SKSE Menu Framework (GPL-3.0 header in
-- include/third_party).
target("LipSyncTogether")
    add_deps("commonlibsse-ng")
    add_packages("libopus")
    add_rules("commonlibsse-ng.plugin", {
        name = "LipSyncTogether",
        author = "mailamea10, absol89",
        description = "Moves Skyrim Together players' mouths while they talk, and carries their voices (Plugin API V3)",
        options = { address_library = true }
    })
    add_files("src/LipSyncTogether/*.cpp", "src/LipSyncTogether/plugin/*.cpp")
    add_includedirs("include", "src", "src/LipSyncTogether")
    add_syslinks("dbghelp", "ws2_32", "ole32", "shell32", "user32", "bcrypt")
    add_cxxflags("cl::/wd4100", "cl::/wd4200", "cl::/wd4201")
target_end()
