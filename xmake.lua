-- PickpocketPlus (OBSE64 plugin) — version 0.32
-- Companion DLL for the PickpocketPlus UE4SS mod: bonus Sneak XP and worn-gear copies (requests and XP/penalty via a status file).
-- Flat layout: main.cpp and pch.h sit in the main folder next to this file (the old src folder is ignored).

includes("lib/commonlibob64")

set_project("PickpocketPlus")
set_version("0.32.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

target("PickpocketPlus")
    add_rules("commonlibob64.plugin", {
        name = "PickpocketPlus",
        author = "Solo",
        description = "PickpocketPlus: bonus Sneak XP and worn-gear stealing for the PickpocketPlus UE4SS mod"
    })
    add_files("main.cpp")
    add_headerfiles("pch.h")
    add_includedirs(".")
    set_pcxxheader("pch.h")
