# PickpocketPlus (OBSE64 plugin) — version 0.22

Companion DLL for the PickpocketPlus UE4SS mod (Oblivion Remastered). The UE4SS mod calls these console commands:

    PPAddSneakXP <amount>                         bonus Sneak XP for a successful steal
    PPPrepareTarget <maxDistance> <skipProtected> unequipped copies of the worn items of the NPC in front of you,
                                                  so their worn gear shows up in the steal menu (they stay dressed)
    PPCleanupTarget <0|1|2>                       removes untaken copies / the real pieces you stole;
                                                  after a load: forgets, and repairs saves that captured copies

Files it writes next to the game exe (Binaries\Win64): PickpocketPlus_worn.txt (for the UE4SS mod) and
PickpocketPlus_ledger.txt (what was copied, used to repair saves).

Install: `Oblivion Remastered\OblivionRemastered\Binaries\Win64\OBSE\Plugins\PickpocketPlus.dll`
Requires: OBSE64, Address Library for OBSE Plugins, the PickpocketPlus UE4SS mod.

Built with CommonLibOB64 (GPL-3.0, see LICENSE and EXCEPTIONS).
