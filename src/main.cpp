// PickpocketPlus (OBSE64 plugin) — version 0.8
// v0.8: PPStripTarget <maxDistance> <skipQuestAndScripted>: quest and scripted items stay on the NPC
//       (default 1). Removed the console-selection and target-file experiments (the list can't be refreshed).
// v0.7: writes the stripped NPC's reference ID to PickpocketPlus_target.txt (game folder) so the UE4SS mod
//       can address them directly ("<id>.additem ...") to refresh the steal menu.
// v0.6: target = the NPC you activated (falls back to the nearest NPC); the stripped NPC becomes the console's
//       selected reference, so the UE4SS mod can refresh the steal menu with a quick additem/removeitem.
// v0.5: worn-gear stealing (test build): PPStripTarget <maxDistance> unequips the nearest NPC's worn items,
//       PPRestoreTarget re-equips whatever is left. All console commands are listed in the log once.
// v0.4: adds the XP straight to the player's Sneak experience (the game's UseSkill calls did nothing / crashed).
//       If that crosses the level threshold, the game levels Sneak up on its next own Sneak XP gain
//       (e.g. when you close the steal menu).
// v0.3: build fix. v0.2: XP diagnostics. v0.1: first version.
//
// Takes over one unused debug console command and turns it into:
//     PPAddSneakXP <amount>        (short name: ppxp)
// which adds <amount> Sneak XP, in the same units as the game's own pickpocket XP (2 per pickpocket).
// "PPAddSneakXP 0" only logs the current Sneak XP. The PickpocketPlus UE4SS mod calls it after every steal.

namespace PPXP
{
	constexpr std::int32_t kSneak = 31;       // actor value index of Sneak
	constexpr std::int32_t kFirstSkill = 12;  // TESDataHandler::skill[0] is Armorer (12)

	RE::SCRIPT_PARAMETER g_params[] = {
		{ "amount", RE::SCRIPT_PARAM_TYPE::kFloat, false }
	};

	bool Execute(
		const RE::SCRIPT_PARAMETER* a_parameters,
		const char*                 a_compiledParams,
		RE::TESObjectREFR*          a_refObject,
		RE::TESObjectREFR*          a_container,
		RE::Script*                 a_script,
		RE::ScriptLocals*           a_scriptLocals,
		double&                     a_result,
		std::uint32_t&              a_offset)
	{
		a_result = 0.0;

		float amount = 0.0f;
		if (!RE::Script::ParseParameters(a_parameters, a_compiledParams, a_offset, a_refObject, a_container, a_script, a_scriptLocals, &amount)) {
			REX::WARN("PPAddSneakXP: could not read the amount");
			return true;
		}
		auto player = RE::PlayerCharacter::GetSingleton();
		auto data = RE::TESDataHandler::GetSingleton();
		if (!player || !data) {
			REX::WARN("PPAddSneakXP: player or game data not ready");
			return true;
		}

		constexpr std::int32_t slot = kSneak - kFirstSkill;

		const float before = player->skillUsage[slot];
		const float needed = player->skillAdvancementUses[slot];
		REX::INFO("Sneak XP now {:.3f} / {:.3f} needed for the next level", before, needed);
		if (amount <= 0.0f) {
			return true;
		}

		player->skillUsage[slot] = before + amount;
		const float after = player->skillUsage[slot];
		REX::INFO("PPAddSneakXP: +{:.3f} -> Sneak XP {:.3f} / {:.3f}", amount, after, needed);
		if (needed > 0.0f && after >= needed) {
			REX::INFO("over the threshold: Sneak levels up on the game's next own Sneak XP gain");
		}

		a_result = amount;
		return true;
	}

	// ---------------- worn gear ----------------
	RE::TESFormID                     g_targetID = 0;
	std::vector<RE::TESBoundObject*>  g_stripped;

	RE::Actor* AsNPC(RE::TESObjectREFR* a_ref)
	{
		return (a_ref && a_ref->Is(RE::FormType::ActorCharacter) && a_ref != RE::PlayerCharacter::GetSingleton()) ?
		           static_cast<RE::Actor*>(a_ref) :
		           nullptr;
	}

	RE::SCRIPT_PARAMETER g_distParams[] = {
		{ "maxDistance", RE::SCRIPT_PARAM_TYPE::kFloat, true }
	};
	RE::SCRIPT_PARAMETER g_stripParams[] = {
		{ "maxDistance", RE::SCRIPT_PARAM_TYPE::kFloat, true },
		{ "skipQuestAndScripted", RE::SCRIPT_PARAM_TYPE::kInteger, true }
	};

	// quest items and items with a script attached stay on the NPC (stealing them can break quests)
	bool IsProtected(RE::TESBoundObject* a_object)
	{
		if (a_object->formFlags & RE::TESForm::RecordFlags::kQuestItem) {
			return true;
		}
		if (auto scriptable = RE::oblivion_cast<RE::TESScriptableForm*>(a_object); scriptable && scriptable->formScript) {
			return true;
		}
		return false;
	}

	RE::Actor* FindNearestNPC(float a_maxDist)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->parentCell) {
			return nullptr;
		}
		const auto& p = player->data.location;
		RE::Actor* best = nullptr;
		float      bestDist = a_maxDist;
		for (auto ref : player->parentCell->listReferences) {
			if (!ref || ref == player || !ref->Is(RE::FormType::ActorCharacter)) {
				continue;
			}
			const auto& l = ref->data.location;
			const float dx = l.x - p.x, dy = l.y - p.y, dz = l.z - p.z;
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d <= bestDist) {
				best = static_cast<RE::Actor*>(ref);
				bestDist = d;
			}
		}
		if (best) {
			REX::INFO("nearest NPC: form {:08X} at distance {:.0f}", best->formID, bestDist);
		}
		return best;
	}

	bool ExecuteStrip(
		const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject,
		RE::TESObjectREFR* a_container, RE::Script* a_script, RE::ScriptLocals* a_scriptLocals, double& a_result, std::uint32_t& a_offset)
	{
		a_result = 0.0;
		float        maxDist = 300.0f;
		std::int32_t skipProtected = 1;
		RE::Script::ParseParameters(a_parameters, a_compiledParams, a_offset, a_refObject, a_container, a_script, a_scriptLocals, &maxDist, &skipProtected);
		if (maxDist <= 0.0f) {
			maxDist = 300.0f;
		}

		auto im = RE::InterfaceManager::GetInstance(false, false);
		RE::Actor* npc = nullptr;
		if (im) {
			npc = AsNPC(im->activateRef);
			if (npc) {
				REX::INFO("target from activation: {:08X}", npc->formID);
			} else if ((npc = AsNPC(im->crosshairRef))) {
				REX::INFO("target from crosshair: {:08X}", npc->formID);
			}
		}
		if (!npc) {
			npc = FindNearestNPC(maxDist);
		}
		if (!npc) {
			REX::INFO("PPStripTarget: no NPC within {:.0f}", maxDist);
			return true;
		}
		auto xChanges = npc->extra.GetExtraData<RE::ExtraContainerChanges>();
		if (!xChanges || !xChanges->changes || !xChanges->changes->list) {
			REX::INFO("PPStripTarget: NPC has no inventory changes");
			return true;
		}

		// collect first, unequip after (don't change the list while walking it)
		std::vector<std::pair<RE::TESBoundObject*, RE::ExtraDataList*>> worn;
		for (auto change : *xChanges->changes->list) {
			if (!change || !change->object || !change->extraData) {
				continue;
			}
			for (auto xList : *change->extraData) {
				if (xList && (xList->GetExtraData(RE::EXTRA_DATA_TYPE::Worn) || xList->GetExtraData(RE::EXTRA_DATA_TYPE::WornLeft))) {
					if (skipProtected && IsProtected(change->object)) {
						REX::INFO("  {:08X} is a quest/scripted item - left on", change->object->formID);
						continue;
					}
					worn.emplace_back(change->object, xList);
				}
			}
		}

		g_targetID = npc->formID;
		g_stripped.clear();
		for (auto& [object, xList] : worn) {
			const bool ok = npc->UnequipObject(object, 1, xList, true);
			REX::INFO("  unequip {:08X}: {}", object->formID, ok ? "ok" : "failed");
			g_stripped.push_back(object);
		}
		REX::INFO("PPStripTarget: {} worn item(s) unequipped on {:08X}", g_stripped.size(), g_targetID);
		a_result = static_cast<double>(g_stripped.size());
		return true;
	}

	bool ExecuteRestore(
		const RE::SCRIPT_PARAMETER*, const char*, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, double& a_result, std::uint32_t&)
	{
		a_result = 0.0;
		if (!g_targetID || g_stripped.empty()) {
			return true;
		}
		auto form = RE::TESForm::LookupByID(g_targetID);
		auto npc = (form && form->Is(RE::FormType::ActorCharacter)) ? static_cast<RE::Actor*>(form) : nullptr;
		if (!npc) {
			REX::INFO("PPRestoreTarget: NPC {:08X} not found", g_targetID);
			g_stripped.clear();
			g_targetID = 0;
			return true;
		}
		int restored = 0;
		for (auto object : g_stripped) {
			if (npc->GetItemCountInContainer(object) > 0) {
				npc->EquipObject(object, 1, nullptr);
				++restored;
			} else {
				REX::INFO("  {:08X} is gone (stolen)", object->formID);
			}
		}
		REX::INFO("PPRestoreTarget: {} item(s) re-equipped on {:08X}", restored, g_targetID);

		a_result = restored;
		g_stripped.clear();
		g_targetID = 0;
		return true;
	}

	// ---------------- console command takeover ----------------
	template <std::uint16_t N>
	bool Hijack(std::string_view a_victim, const char* a_name, const char* a_short, const char* a_help,
		RE::SCRIPT_PARAMETER (&a_params)[N], RE::SCRIPT_FUNCTION::Execute_t* a_fn)
	{
		auto cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(a_victim);
		if (!cmd) {
			return false;
		}
		REX::INFO("replacing unused console command '{}' with {}", a_victim, a_name);
		cmd->functionName = a_name;
		cmd->shortName = a_short;
		cmd->helpString = a_help;
		cmd->referenceFunction = false;
		cmd->SetParameters(a_params);
		cmd->executeFunction = a_fn;
		cmd->editorFilter = false;
		return true;
	}

	void Install()
	{
		// unused debug commands we may take over, in order
		constexpr std::array victims{
			"OutputMemContexts"sv, "OutputMemStats"sv, "OutputArchiveProfile"sv, "TestCode"sv,
			"OutputLocalMapPictures"sv, "TestSeenData"sv
		};
		std::size_t next = 0;
		auto take = [&](const char* a_name, const char* a_short, const char* a_help, auto& a_params, RE::SCRIPT_FUNCTION::Execute_t* a_fn) {
			while (next < victims.size()) {
				if (Hijack(victims[next++], a_name, a_short, a_help, a_params, a_fn)) {
					return;
				}
			}
			REX::ERROR("no free console command left for {}", a_name);
		};

		take("PPAddSneakXP", "ppxp", "PickpocketPlus: adds Sneak XP (PPAddSneakXP <amount>)", g_params, &Execute);
		take("PPStripTarget", "ppstrip", "PickpocketPlus: unequips the NPC's worn items (PPStripTarget <maxDistance> <skipQuestAndScripted>)", g_stripParams, &ExecuteStrip);
		take("PPRestoreTarget", "pprestore", "PickpocketPlus: re-equips what PPStripTarget took off", g_distParams, &ExecuteRestore);
	}
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	REX::INFO("PickpocketPlus 0.8 loaded");
	PPXP::Install();
	return true;
}
