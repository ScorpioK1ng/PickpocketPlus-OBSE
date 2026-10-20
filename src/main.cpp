// PickpocketPlus (OBSE64 plugin) — version 0.20
// v0.20: self-repair after loading a save: every NPC that got copies is remembered in PickpocketPlus_ledger.txt
//        (NPC, item, count before the copy). "PPCleanupTarget 2" (sent after each load) removes any extra copies
//        a save captured, so a save made with copies active heals itself on load.
// v0.19: switching to another NPC while sneaking cleans up the old one and prepares the new one in the same
//        call (the crashes turned out to be in the console, not in adding/removing items).
// v0.18: the trace also records the start and end of every PickpocketPlus console command (the crash is in the
//        game's console handling - this shows whether one of ours was running at that moment).
// v0.17: targets the NPC in FRONT of you (within a 50° cone), the nearest one only as a fallback (crowded
//        taverns picked the wrong NPC); writes PickpocketPlus_trace.txt right before and after every add /
//        remove / equip call, so after a crash the last line shows exactly which call crashed.
// v0.16: crash safety: every crash so far happened while changing an NPC's inventory shortly after a previous
//        change (the NPC's AI still re-checking its gear). Now: copies are not added to an NPC within 5 s of
//        their last cleanup, and an AI-removed piece is put back only after 2 s.
// v0.15: a piece the AI took off (e.g. the shield) is put back after 0.5 s of settling instead of 2 s.
// v0.14: "PPCleanupTarget 1" only forgets the prepared NPC without touching their inventory (used after a save
//        is loaded: the reloaded NPC has no copies, so a normal cleanup would wrongly take real pieces).
// v0.13: crash safety: a piece the AI took off is put back on only once, and only after the NPC has had
//        2 s to settle after the copies were added (no tug-of-war with the AI).
// v0.12: keeps the prepared NPC dressed: if the game's AI takes a real piece off after the copies were added
//        (e.g. the shield), it is put back on (up to 3 tries per item) on the next prepare call.
// v0.11: PickpocketPlus_worn.txt lines are "name|value|weight" (the UE4SS mod can match items by value and
//        weight if the names differ from the menu's); each line is also logged.
// v0.10: target = crosshair NPC if the game reports one, otherwise the nearest NPC in reach (the crosshair
//        target turned out to be empty in the Remaster); dead/unconscious NPCs are never touched (no duplicate
//        loot on corpses).
// v0.9: worn gear via COPIES (the NPC stays dressed): while you sneak and aim at an NPC, PPPrepareTarget adds an
//       unequipped copy of each worn item, so the steal menu lists them from the start. PPCleanupTarget removes
//       the copies you left - or the real worn piece, if you stole its copy. Worn item names go to
//       PickpocketPlus_worn.txt for the UE4SS mod. The undress method (PPStripTarget/PPRestoreTarget) is gone.
// v0.8: quest/scripted items stay on. v0.4-0.7: XP written straight to Sneak experience, strip experiments.
// v0.1-0.3: first version, build fixes.
//
// Console commands (each takes over an unused debug command):
//     PPAddSneakXP <amount>                          adds Sneak XP (game units: 2 per pickpocket)
//     PPPrepareTarget <maxDistance> <skipProtected>  copies the crosshair NPC's worn items into their inventory
//     PPCleanupTarget                                removes leftover copies / stolen worn pieces

namespace PPXP
{
	constexpr std::int32_t kSneak = 31;       // actor value index of Sneak
	constexpr std::int32_t kFirstSkill = 12;  // TESDataHandler::skill[0] is Armorer (12)

	// flushed line-by-line, so it survives a crash (the normal log may lose its last lines)
	void Trace(const std::string& a_line)
	{
		static std::ofstream file("PickpocketPlus_trace.txt", std::ios::trunc);
		if (file) {
			const auto now = std::chrono::system_clock::now();
			file << std::format("{:%H:%M:%S} {}", std::chrono::floor<std::chrono::milliseconds>(now), a_line) << std::endl;
		}
	}

	// ---------------- Sneak XP ----------------
	RE::SCRIPT_PARAMETER g_xpParams[] = {
		{ "amount", RE::SCRIPT_PARAM_TYPE::kFloat, false }
	};

	bool ExecuteAddXP(
		const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject,
		RE::TESObjectREFR* a_container, RE::Script* a_script, RE::ScriptLocals* a_scriptLocals, double& a_result, std::uint32_t& a_offset)
	{
		Trace("cmd PPAddSneakXP start");
		struct TraceEnd { ~TraceEnd() { Trace("cmd PPAddSneakXP end"); } } traceEnd;
		a_result = 0.0;
		float amount = 0.0f;
		if (!RE::Script::ParseParameters(a_parameters, a_compiledParams, a_offset, a_refObject, a_container, a_script, a_scriptLocals, &amount)) {
			return true;
		}
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return true;
		}
		constexpr std::int32_t slot = kSneak - kFirstSkill;
		const float before = player->skillUsage[slot];
		const float needed = player->skillAdvancementUses[slot];
		if (amount > 0.0f) {
			player->skillUsage[slot] = before + amount;
		}
		REX::INFO("PPAddSneakXP: +{:.3f} -> Sneak XP {:.3f} / {:.3f}", amount, player->skillUsage[slot], needed);
		a_result = amount;
		return true;
	}

	// ---------------- worn-gear copies ----------------
	struct Copy
	{
		RE::TESBoundObject* object;
		std::int32_t        countWithCopy;  // NPC's count of this item right after the copy was added
		int                 redressTries = 0;
	};

	RE::TESFormID                         g_prepID = 0;
	std::vector<Copy>                     g_copies;
	std::chrono::steady_clock::time_point g_prepTime{};
	RE::TESFormID                         g_lastCleanID = 0;
	std::chrono::steady_clock::time_point g_lastCleanTime{};

	RE::SCRIPT_PARAMETER g_prepParams[] = {
		{ "maxDistance", RE::SCRIPT_PARAM_TYPE::kFloat, true },
		{ "skipProtected", RE::SCRIPT_PARAM_TYPE::kInt, true }
	};
	RE::SCRIPT_PARAMETER g_cleanupParams[] = {
		{ "forgetOnly", RE::SCRIPT_PARAM_TYPE::kInt, true }
	};

	RE::Actor* AsNPC(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || !a_ref->Is(RE::FormType::ActorCharacter) || a_ref == RE::PlayerCharacter::GetSingleton()) {
			return nullptr;
		}
		auto actor = static_cast<RE::Actor*>(a_ref);
		return actor->lifeState == RE::ACTOR_LIFE_STATE::kAlive ? actor : nullptr;
	}

	RE::Actor* LookupNPC(RE::TESFormID a_id)
	{
		if (!a_id) {
			return nullptr;
		}
		auto form = RE::TESForm::LookupByID(a_id);
		if (!form || !form->Is(RE::FormType::ActorCharacter)) {
			return nullptr;
		}
		return static_cast<RE::Actor*>(form);
	}

	float DistanceToPlayer(RE::TESObjectREFR* a_ref)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_ref) {
			return 1e9f;
		}
		const auto& p = player->data.location;
		const auto& l = a_ref->data.location;
		const float dx = l.x - p.x, dy = l.y - p.y, dz = l.z - p.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	// the NPC you're facing: smallest angle to your view direction (within 50 degrees), nearest as fallback
	RE::Actor* FindTargetNPC(float a_maxDist)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->parentCell) {
			return nullptr;
		}
		const auto& p = player->data.location;
		const float yaw = player->data.angle.z;
		const float fx = std::sin(yaw), fy = std::cos(yaw);
		RE::Actor* best = nullptr;
		float      bestAngle = 50.0f;
		for (auto ref : player->parentCell->listReferences) {
			auto npc = AsNPC(ref);
			if (!npc) {
				continue;
			}
			const float d = DistanceToPlayer(npc);
			if (d > a_maxDist || d < 1.0f) {
				continue;
			}
			const float dx = npc->data.location.x - p.x, dy = npc->data.location.y - p.y;
			const float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1.0f) {
				continue;
			}
			float cosA = (dx * fx + dy * fy) / len;
			cosA = std::clamp(cosA, -1.0f, 1.0f);
			const float angle = std::acos(cosA) * 57.2958f;
			if (angle < bestAngle) {
				best = npc;
				bestAngle = angle;
			}
		}
		return best;
	}

	RE::Actor* FindNearestNPC(float a_maxDist)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->parentCell) {
			return nullptr;
		}
		RE::Actor* best = nullptr;
		float      bestDist = a_maxDist;
		for (auto ref : player->parentCell->listReferences) {
			if (auto npc = AsNPC(ref)) {
				const float d = DistanceToPlayer(npc);
				if (d <= bestDist) {
					best = npc;
					bestDist = d;
				}
			}
		}
		return best;
	}

	// quest items and items with a script attached stay untouched (stealing them can break quests)
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

	RE::InventoryChanges* ChangesOf(RE::Actor* a_npc)
	{
		auto x = a_npc->extra.GetExtraData<RE::ExtraContainerChanges>();
		return (x && x->changes && x->changes->list) ? x->changes : nullptr;
	}

	bool IsWornList(RE::ExtraDataList* a_xList)
	{
		return a_xList && (a_xList->GetExtraData(RE::EXTRA_DATA_TYPE::Worn) || a_xList->GetExtraData(RE::EXTRA_DATA_TYPE::WornLeft));
	}

	// the worn instance of an item on the NPC (nullptr if not worn)
	RE::ExtraDataList* WornListOf(RE::Actor* a_npc, RE::TESBoundObject* a_object)
	{
		auto changes = ChangesOf(a_npc);
		if (!changes) {
			return nullptr;
		}
		for (auto change : *changes->list) {
			if (change && change->object == a_object && change->extraData) {
				for (auto xList : *change->extraData) {
					if (IsWornList(xList)) {
						return xList;
					}
				}
			}
		}
		return nullptr;
	}

	// names for the UE4SS mod (it marks these items in the steal list)
	std::string NameOf(RE::TESBoundObject* a_object)
	{
		if (auto name = RE::TESFullName::GetFullName(a_object); name && *name) {
			return name;
		}
		if (auto full = RE::oblivion_cast<RE::TESFullName*>(a_object); full && full->fullName.c_str()) {
			return full->fullName.c_str();
		}
		return {};
	}

	void WriteWornFile(const std::vector<Copy>& a_copies)
	{
		std::ofstream file("PickpocketPlus_worn.txt", std::ios::trunc);
		if (!file) {
			REX::WARN("could not write PickpocketPlus_worn.txt");
			return;
		}
		for (auto& c : a_copies) {
			std::uint32_t value = 0;
			float         weight = 0.0f;
			if (auto v = RE::oblivion_cast<RE::TESValueForm*>(c.object)) {
				value = v->value;
			}
			if (auto w = RE::oblivion_cast<RE::TESWeightForm*>(c.object)) {
				weight = w->weight;
			}
			const auto line = std::format("{}|{}|{:.2f}", NameOf(c.object), value, weight);
			file << line << "\n";
			REX::INFO("  worn copy: {}", line);
		}
	}

	// ---------------- ledger: what we copied, so saves that captured copies can be repaired ----------------
	struct LedgerEntry
	{
		RE::TESFormID npc;
		RE::TESFormID object;
		std::int32_t  baseline;  // NPC's count of the item before the copy was added
	};
	constexpr std::size_t kLedgerMax = 60;

	std::vector<LedgerEntry> LoadLedger()
	{
		std::vector<LedgerEntry> list;
		std::ifstream            file("PickpocketPlus_ledger.txt");
		LedgerEntry              e{};
		while (file >> std::hex >> e.npc >> e.object >> std::dec >> e.baseline) {
			list.push_back(e);
		}
		return list;
	}

	void SaveLedger(const std::vector<LedgerEntry>& a_list)
	{
		std::ofstream file("PickpocketPlus_ledger.txt", std::ios::trunc);
		for (auto& e : a_list) {
			file << std::format("{:08X} {:08X} {}\n", e.npc, e.object, e.baseline);
		}
	}

	void RememberCopy(RE::TESFormID a_npc, RE::TESFormID a_object, std::int32_t a_baseline)
	{
		auto list = LoadLedger();
		std::erase_if(list, [&](const LedgerEntry& e) { return e.npc == a_npc && e.object == a_object; });
		list.push_back({ a_npc, a_object, a_baseline });
		if (list.size() > kLedgerMax) {
			list.erase(list.begin(), list.begin() + (list.size() - kLedgerMax));
		}
		SaveLedger(list);
	}

	// after a load: any NPC holding more of an item than before we ever copied it gets the extras removed
	void RepairAfterLoad()
	{
		int repaired = 0;
		for (auto& e : LoadLedger()) {
			auto npc = LookupNPC(e.npc);
			auto form = RE::TESForm::LookupByID(e.object);
			if (!npc || !form || !npc->parentCell) {
				continue;  // not loaded right now: checked again after the next load
			}
			auto object = static_cast<RE::TESBoundObject*>(form);
			const std::int32_t now = npc->GetItemCountInContainer(object);
			for (std::int32_t extra = now - e.baseline; extra > 0; --extra) {
				Trace(std::format("repair: remove saved copy {:08X} from {:08X}", e.object, e.npc));
				npc->RemoveItem(object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
				Trace("  done");
				++repaired;
			}
			if (now > e.baseline && !WornListOf(npc, object) && npc->GetItemCountInContainer(object) > 0) {
				npc->EquipObject(object, 1, nullptr);
			}
		}
		REX::INFO("PPCleanupTarget: repair after load - {} saved copies removed", repaired);
	}

	void Cleanup()
	{
		auto npc = LookupNPC(g_prepID);
		if (npc) {
			int removedCopies = 0, takenWorn = 0;
			for (auto& c : g_copies) {
				const std::int32_t now = npc->GetItemCountInContainer(c.object);
				if (now >= c.countWithCopy) {
					// copy still there: remove it, and make sure the real piece is still worn
					Trace(std::format("remove copy {:08X} from {:08X}", c.object->formID, g_prepID));
					npc->RemoveItem(c.object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
					Trace("  done");
					++removedCopies;
					if (!WornListOf(npc, c.object) && npc->GetItemCountInContainer(c.object) > 0) {
						Trace(std::format("re-equip {:08X} on {:08X}", c.object->formID, g_prepID));
						npc->EquipObject(c.object, 1, nullptr);
						Trace("  done");
					}
				} else {
					// the copy was stolen: the real worn piece goes too
					if (auto worn = WornListOf(npc, c.object)) {
						Trace(std::format("unequip stolen {:08X} on {:08X}", c.object->formID, g_prepID));
						npc->UnequipObject(c.object, 1, worn, true);
						Trace("  done");
					}
					if (npc->GetItemCountInContainer(c.object) > 0) {
						Trace(std::format("remove stolen {:08X} from {:08X}", c.object->formID, g_prepID));
						npc->RemoveItem(c.object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
						Trace("  done");
					}
					++takenWorn;
					REX::INFO("  {:08X} was stolen - real worn piece removed", c.object->formID);
				}
			}
			REX::INFO("PPCleanupTarget: {:08X}: {} copies removed, {} worn pieces taken", g_prepID, removedCopies, takenWorn);
		}
		g_lastCleanID = g_prepID;
		g_lastCleanTime = std::chrono::steady_clock::now();
		g_copies.clear();
		g_prepID = 0;
		WriteWornFile(g_copies);
	}

	bool ExecutePrepare(
		const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject,
		RE::TESObjectREFR* a_container, RE::Script* a_script, RE::ScriptLocals* a_scriptLocals, double& a_result, std::uint32_t& a_offset)
	{
		Trace("cmd PPPrepareTarget start");
		struct TraceEnd { ~TraceEnd() { Trace("cmd PPPrepareTarget end"); } } traceEnd;
		a_result = 0.0;
		float        maxDist = 300.0f;
		std::int32_t skipProtected = 1;
		RE::Script::ParseParameters(a_parameters, a_compiledParams, a_offset, a_refObject, a_container, a_script, a_scriptLocals, &maxDist, &skipProtected);
		if (maxDist <= 0.0f) {
			maxDist = 300.0f;
		}

		auto im = RE::InterfaceManager::GetInstance(false, false);
		auto npc = im ? AsNPC(im->crosshairRef) : nullptr;
		if (npc && DistanceToPlayer(npc) > maxDist) {
			npc = nullptr;
		}
		if (!npc) {
			npc = FindTargetNPC(maxDist);
		}
		if (!npc) {
			npc = FindNearestNPC(maxDist);
		}
		if (!npc) {
			// nobody in reach: drop the prepared NPC once we've walked away from them
			if (g_prepID) {
				auto prepared = LookupNPC(g_prepID);
				if (!prepared || DistanceToPlayer(prepared) > maxDist * 1.5f) {
					Cleanup();
				}
			}
			return true;
		}
		if (npc->formID == g_prepID) {
			// already prepared: make sure the AI didn't take a real piece off (e.g. the shield)
			if (std::chrono::steady_clock::now() - g_prepTime < std::chrono::seconds(2)) {
				return true;  // let the NPC settle first
			}
			for (auto& c : g_copies) {
				if (c.redressTries < 1 && !WornListOf(npc, c.object) && npc->GetItemCountInContainer(c.object) > 0) {
					++c.redressTries;
					Trace(std::format("put back {:08X} on {:08X}", c.object->formID, npc->formID));
					npc->EquipObject(c.object, 1, nullptr);
					Trace("  done");
					REX::INFO("  {:08X} was taken off by the AI - put back on", c.object->formID);
				}
			}
			return true;
		}
		if (g_prepID) {
			Cleanup();  // switched to another NPC: the old one gets their real gear back first
		}
		if (npc->formID == g_lastCleanID && std::chrono::steady_clock::now() - g_lastCleanTime < std::chrono::seconds(5)) {
			return true;  // this NPC's AI may still be re-checking its gear after the last cleanup
		}

		auto changes = ChangesOf(npc);
		if (!changes) {
			return true;
		}
		std::vector<RE::TESBoundObject*> worn;
		for (auto change : *changes->list) {
			if (!change || !change->object || !change->extraData) {
				continue;
			}
			bool isWorn = false;
			for (auto xList : *change->extraData) {
				if (IsWornList(xList)) {
					isWorn = true;
					break;
				}
			}
			if (!isWorn || change->object->Is(RE::FormType::Ammo)) {
				continue;  // ammo stacks make "was the copy taken?" ambiguous
			}
			if (skipProtected && IsProtected(change->object)) {
				continue;
			}
			worn.push_back(change->object);
		}

		g_prepID = npc->formID;
		g_prepTime = std::chrono::steady_clock::now();
		g_copies.clear();
		for (auto object : worn) {
			RememberCopy(npc->formID, object->formID, npc->GetItemCountInContainer(object));
			Trace(std::format("add copy {:08X} to {:08X}", object->formID, npc->formID));
			npc->AddObjectToContainer(object, nullptr, 1);
			Trace("  done");
			g_copies.push_back({ object, npc->GetItemCountInContainer(object) });
		}
		WriteWornFile(g_copies);
		REX::INFO("PPPrepareTarget: {:08X}: {} worn item copies added", g_prepID, g_copies.size());
		a_result = static_cast<double>(g_copies.size());
		return true;
	}

	bool ExecuteCleanup(
		const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject,
		RE::TESObjectREFR* a_container, RE::Script* a_script, RE::ScriptLocals* a_scriptLocals, double& a_result, std::uint32_t& a_offset)
	{
		Trace("cmd PPCleanupTarget start");
		struct TraceEnd { ~TraceEnd() { Trace("cmd PPCleanupTarget end"); } } traceEnd;
		a_result = 0.0;
		std::int32_t forgetOnly = 0;
		RE::Script::ParseParameters(a_parameters, a_compiledParams, a_offset, a_refObject, a_container, a_script, a_scriptLocals, &forgetOnly);
		if (forgetOnly) {
			// 1 = after a load: forget the old state; 2 = also repair saves that captured copies
			if (g_prepID) {
				REX::INFO("PPCleanupTarget: forgetting {:08X} after a load (inventory untouched)", g_prepID);
			}
			g_copies.clear();
			g_prepID = 0;
			WriteWornFile(g_copies);
			if (forgetOnly == 2) {
				RepairAfterLoad();
			}
			return true;
		}
		if (g_prepID) {
			Cleanup();
		}
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

		take("PPAddSneakXP", "ppxp", "PickpocketPlus: adds Sneak XP (PPAddSneakXP <amount>)", g_xpParams, &ExecuteAddXP);
		take("PPPrepareTarget", "ppprep", "PickpocketPlus: copies the aimed NPC's worn items (PPPrepareTarget <maxDistance> <skipProtected>)", g_prepParams, &ExecutePrepare);
		take("PPCleanupTarget", "ppclean", "PickpocketPlus: removes worn-item copies (PPCleanupTarget <0 normal | 1 forget | 2 forget + repair after load>)", g_cleanupParams, &ExecuteCleanup);
		WriteWornFile(g_copies);  // start with an empty file
	}
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	REX::INFO("PickpocketPlus 0.20 loaded");
	PPXP::Install();
	return true;
}
