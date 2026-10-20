-- PickpocketPlus (OBSE64 plugin) — version 0.12
-- Console commands for the PickpocketPlus UE4SS mod: bonus Sneak XP and worn-gear copies.

includes("lib/commonlibob64")

set_project("PickpocketPlus")
set_version("0.12.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

target("PickpocketPlus")
    add_rules("commonlibob64.plugin", {
        name = "PickpocketPlus",
        author = "Solo",
        description = "PickpocketPlus: bonus Sneak XP for successful steals (console command PPAddSneakXP)"
    })
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
