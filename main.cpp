// PickpocketPlus (OBSE64 plugin) — version 0.30
// Companion DLL for the PickpocketPlus UE4SS mod.
//   Console commands (used only a few times per robbery, at calm moments):
//     PPPrepareTarget <maxDistance> <skipProtected> adds unequipped copies of the worn items of the NPC in front
//                                                   of you (worn gear shows in their steal menu, they stay dressed)
//     PPCleanupTarget <0|1|3>                       0 = after a steal menu: remove the real pieces you stole,
//                                                   1 = after a load / area change, 3 = dead-NPC sweep only
//     PPAddSneakXP <amount>                         (kept for manual testing)
//   Without console (frequent things): bonus Sneak XP and the worn penalty come from PickpocketPlus_bridge.txt,
//   written by the UE4SS mod; a background reader applies the penalty, the XP is added on the game thread.
//
// v0.30: copies are added ONCE per NPC and never taken back (no more add/remove churn): only the real piece of a
//        stolen copy is removed; dead NPCs lose their copies. Held weapons: the copy gets its own condition (1 point
//        below new) so it never merges with the held weapon's hidden stack and always shows in the steal menu.
// v0.29: hybrid: adding/removing items must run through the game's script context (a console command) - from an
//        Unreal task it crashed at once (0.28). XP and penalty stay console-free. Held weapons are copied again.
// History: 0.1-0.4 Sneak XP; 0.5-0.8 undress method; 0.9-0.23 copies via console; 0.24-0.28 console-free attempt.

namespace PPXP
{
	constexpr std::int32_t kSneak = 31;       // actor value index of Sneak
	constexpr std::int32_t kFirstSkill = 12;  // TESDataHandler::skill[0] is Armorer (12)

	// ---------------- Sneak XP ----------------
	RE::SCRIPT_PARAMETER g_xpParams[] = {
		{ "amount", RE::SCRIPT_PARAM_TYPE::kFloat, false }
	};

	bool ExecuteAddXP(
		const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject,
		RE::TESObjectREFR* a_container, RE::Script* a_script, RE::ScriptLocals* a_scriptLocals, double& a_result, std::uint32_t& a_offset)
	{
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

	// ---------------- worn-gear copies: added ONCE per NPC, never taken back ----------------
	// Each worn item of an NPC gets one unequipped copy (that's what shows in the steal menu). Untaken copies
	// simply stay in the NPC's pack. Only when you steal a copy, the NPC's real worn piece is removed.
	// The ledger remembers, per NPC and item, how many they had BEFORE the copy ("baseline"):
	//   count >  baseline -> the copy is there        -> nothing to add
	//   count <= baseline -> no copy (or it was stolen) -> add one / remove the real piece after a steal

	RE::SCRIPT_PARAMETER g_prepParams[] = {
		{ "maxDistance", RE::SCRIPT_PARAM_TYPE::kFloat, true },
		{ "skipProtected", RE::SCRIPT_PARAM_TYPE::kInt, true }
	};
	RE::SCRIPT_PARAMETER g_cleanupParams[] = {
		{ "mode", RE::SCRIPT_PARAM_TYPE::kInt, true }
	};

	using ItemKey = std::pair<RE::TESFormID, RE::TESFormID>;  // NPC, item
	RE::TESFormID     g_targetID = 0;  // the NPC the last prepare looked at
	std::set<ItemKey> g_redressed;     // AI-removed pieces already put back once
	std::set<ItemKey> g_hadCopy;       // copies confirmed present this session (a steal can only take those)

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

	// ---------------- ledger (PickpocketPlus_ledger.txt) ----------------
	struct LedgerEntry
	{
		RE::TESFormID npc;
		RE::TESFormID object;
		std::int32_t  baseline;  // the NPC's count of the item before the copy was added
	};
	constexpr std::size_t kLedgerMax = 5000;  // never dropped in practice: a dropped entry could cause a 2nd copy

	std::vector<LedgerEntry> g_ledger;
	bool                     g_ledgerLoaded = false;

	std::vector<LedgerEntry>& Ledger()
	{
		if (!g_ledgerLoaded) {
			g_ledgerLoaded = true;
			std::ifstream file("PickpocketPlus_ledger.txt");
			LedgerEntry   e{};
			while (file >> std::hex >> e.npc >> e.object >> std::dec >> e.baseline) {
				g_ledger.push_back(e);
			}
		}
		return g_ledger;
	}

	void SaveLedger()
	{
		auto& list = Ledger();
		if (list.size() > kLedgerMax) {
			list.erase(list.begin(), list.begin() + (list.size() - kLedgerMax));
		}
		std::ofstream file("PickpocketPlus_ledger.txt", std::ios::trunc);
		for (auto& e : list) {
			file << std::format("{:08X} {:08X} {}\n", e.npc, e.object, e.baseline);
		}
	}

	LedgerEntry* FindEntry(RE::TESFormID a_npc, RE::TESFormID a_object)
	{
		for (auto& e : Ledger()) {
			if (e.npc == a_npc && e.object == a_object) {
				return &e;
			}
		}
		return nullptr;
	}

	// ---------------- weapons: the copy gets its own condition so it never merges with the held one ----------------
	// (a held weapon's stack is hidden in the steal menu; a copy with a different condition gets its own row)
	RE::ExtraDataList* MakeConditionList(RE::TESBoundObject* a_object)
	{
		auto healthForm = RE::oblivion_cast<RE::TESHealthForm*>(a_object);
		if (!healthForm || healthForm->health < 2) {
			return nullptr;
		}
		auto xList = static_cast<RE::ExtraDataList*>(RE::malloc(sizeof(RE::ExtraDataList)));
		auto extra = static_cast<std::uint8_t*>(RE::malloc(0x20));
		if (!xList || !extra) {
			return nullptr;
		}
		std::memset(xList, 0, sizeof(RE::ExtraDataList));
		std::memset(extra, 0, 0x20);
		REL::Relocation<std::uintptr_t> listVtbl{ RE::VTABLE::ExtraDataList[0] };
		REL::Relocation<std::uintptr_t> healthVtbl{ RE::VTABLE::ExtraHealth[0] };
		*reinterpret_cast<std::uintptr_t*>(xList) = listVtbl.address();
		*reinterpret_cast<std::uintptr_t*>(extra) = healthVtbl.address();
		extra[0x08] = static_cast<std::uint8_t>(RE::EXTRA_DATA_TYPE::Health);       // BSExtraData::type
		*reinterpret_cast<float*>(extra + 0x18) = static_cast<float>(healthForm->health - 1);  // 1 point below new
		xList->AddExtra(reinterpret_cast<RE::BSExtraData*>(extra));
		return xList;
	}

	// worn items of an NPC that qualify for a copy (no ammo, optionally no quest/scripted items)
	std::vector<RE::TESBoundObject*> WornItems(RE::Actor* a_npc, bool a_skipProtected)
	{
		std::vector<RE::TESBoundObject*> worn;
		auto changes = ChangesOf(a_npc);
		if (!changes) {
			return worn;
		}
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
			if (a_skipProtected && IsProtected(change->object)) {
				continue;
			}
			worn.push_back(change->object);
		}
		return worn;
	}

	// names (+ value, weight) of the items that currently have a copy, for the UE4SS mod's marks
	void WriteWornFile(RE::Actor* a_npc)
	{
		std::ofstream file("PickpocketPlus_worn.txt", std::ios::trunc);
		if (!file || !a_npc) {
			return;
		}
		for (auto& e : Ledger()) {
			if (e.npc != a_npc->formID) {
				continue;
			}
			auto object = static_cast<RE::TESBoundObject*>(RE::TESForm::LookupByID(e.object));
			if (!object || a_npc->GetItemCountInContainer(object) <= e.baseline) {
				continue;  // no copy right now
			}
			std::uint32_t value = 0;
			float         weight = 0.0f;
			if (auto v = RE::oblivion_cast<RE::TESValueForm*>(object)) {
				value = v->value;
			}
			if (auto w = RE::oblivion_cast<RE::TESWeightForm*>(object)) {
				weight = w->weight;
			}
			file << std::format("{}|{}|{:.2f}\n", NameOf(object), value, weight);
		}
	}

	void ClearWornFile()
	{
		std::ofstream file("PickpocketPlus_worn.txt", std::ios::trunc);
	}

	// ---------------- PPPrepareTarget: copies for the NPC in front of you (only what's missing) ----------------
	bool ExecutePrepare(
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
			return true;
		}
		g_targetID = npc->formID;

		int  added = 0;
		bool ledgerChanged = false;
		for (auto object : WornItems(npc, skipProtected != 0)) {
			const std::int32_t now = npc->GetItemCountInContainer(object);
			auto               entry = FindEntry(npc->formID, object->formID);
			if (!entry) {
				Ledger().push_back({ npc->formID, object->formID, now });
				entry = &Ledger().back();
				ledgerChanged = true;
			}
			if (now > entry->baseline) {
				g_hadCopy.insert({ npc->formID, object->formID });
				continue;  // the copy is already there
			}
			if (now < entry->baseline) {
				entry->baseline = now;  // they have fewer than before (e.g. an older save): adjust
				ledgerChanged = true;
			}
			RE::ExtraDataList* condition = object->Is(RE::FormType::Weapon) ? MakeConditionList(object) : nullptr;
			npc->AddObjectToContainer(object, condition, 1);
			g_hadCopy.insert({ npc->formID, object->formID });
			++added;
		}
		if (ledgerChanged || added) {
			SaveLedger();
		}

		// the AI sometimes takes a real piece off after a copy arrives (e.g. the shield): put it back once
		for (auto& e : Ledger()) {
			if (e.npc != npc->formID || g_redressed.contains({ e.npc, e.object })) {
				continue;
			}
			auto object = static_cast<RE::TESBoundObject*>(RE::TESForm::LookupByID(e.object));
			if (object && npc->GetItemCountInContainer(object) > e.baseline && !WornListOf(npc, object)) {
				g_redressed.insert({ e.npc, e.object });
				npc->EquipObject(object, 1, nullptr);
				REX::INFO("  {:08X} was taken off by the AI - put back on", e.object);
			}
		}

		WriteWornFile(npc);
		if (added) {
			REX::INFO("PPPrepareTarget: {:08X}: {} copies added (only missing ones)", npc->formID, added);
		}
		a_result = added;
		return true;
	}

	// after a steal menu: real pieces whose copy was stolen are removed (nothing else is touched)
	void SettleStolen(RE::Actor* a_npc)
	{
		int  taken = 0;
		bool changed = false;
		for (auto& e : Ledger()) {
			if (e.npc != a_npc->formID) {
				continue;
			}
			auto object = static_cast<RE::TESBoundObject*>(RE::TESForm::LookupByID(e.object));
			if (!object) {
				continue;
			}
			const std::int32_t now = a_npc->GetItemCountInContainer(object);
			if (now > e.baseline) {
				continue;  // copy still there
			}
			auto worn = WornListOf(a_npc, object);
			if (worn && now > 0 && g_hadCopy.erase({ e.npc, e.object })) {
				// the copy was stolen: the real worn piece goes too
				a_npc->UnequipObject(object, 1, worn, true);
				a_npc->RemoveItem(object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
				REX::INFO("  {:08X} was stolen - real worn piece removed", e.object);
				++taken;
			}
			const std::int32_t after = a_npc->GetItemCountInContainer(object);
			if (after != e.baseline) {
				e.baseline = after;  // new normal for this NPC (a later re-equip gets one fresh copy)
				changed = true;
			}
		}
		if (changed) {
			SaveLedger();
		}
		if (taken) {
			REX::INFO("PPCleanupTarget: {:08X}: {} worn pieces taken", a_npc->formID, taken);
		}
	}

	// dead NPCs: their copies go, so a corpse never holds duplicates
	void SweepDead()
	{
		bool changed = false;
		int  removed = 0;
		for (auto& e : Ledger()) {
			auto form = RE::TESForm::LookupByID(e.npc);
			if (!form || !form->Is(RE::FormType::ActorCharacter)) {
				continue;
			}
			auto npc = static_cast<RE::Actor*>(form);
			if (!npc->parentCell || npc->lifeState == RE::ACTOR_LIFE_STATE::kAlive) {
				continue;
			}
			auto object = static_cast<RE::TESBoundObject*>(RE::TESForm::LookupByID(e.object));
			if (!object) {
				continue;
			}
			for (std::int32_t extra = npc->GetItemCountInContainer(object) - e.baseline; extra > 0; --extra) {
				npc->RemoveItem(object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
				++removed;
				changed = true;
			}
		}
		if (changed) {
			SaveLedger();
			REX::INFO("PPCleanupTarget: {} copies removed from dead NPCs", removed);
		}
	}

	// PPCleanupTarget <mode>: 0 = after a steal menu, 1 = after a load / area change, 3 = dead-NPC sweep only
	bool ExecuteCleanup(
		const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject,
		RE::TESObjectREFR* a_container, RE::Script* a_script, RE::ScriptLocals* a_scriptLocals, double& a_result, std::uint32_t& a_offset)
	{
		a_result = 0.0;
		std::int32_t mode = 0;
		RE::Script::ParseParameters(a_parameters, a_compiledParams, a_offset, a_refObject, a_container, a_script, a_scriptLocals, &mode);
		if (mode == 0) {
			if (auto npc = LookupNPC(g_targetID); npc && npc->parentCell) {
				SettleStolen(npc);
			}
		} else if (mode == 1) {
			g_targetID = 0;
			g_redressed.clear();
			g_hadCopy.clear();
			ClearWornFile();
		}
		SweepDead();
		return true;
	}

	// ---------------- bridge: bonus XP + worn penalty without console ----------------
	using Clock = std::chrono::steady_clock;

	struct Bridge
	{
		bool   valid = false;
		double xp = 0.0;        // bonus XP total sent this session
		float  penalty = 0.0f;  // fPickPocketAmountBase to use right now
	};

	std::mutex g_bridgeLock;
	Bridge     g_bridge;

	Bridge ReadBridgeFile()
	{
		Bridge        b;
		std::ifstream file("PickpocketPlus_bridge.txt");
		std::string   key;
		bool          ended = false;
		while (file >> key) {
			if (key == "end") {
				ended = true;
				break;
			}
			double v = 0.0;
			if (!(file >> v)) {
				break;
			}
			if (key == "xp") b.xp = v;
			else if (key == "pen") b.penalty = static_cast<float>(v);
		}
		b.valid = ended;  // only a fully written file counts
		return b;
	}

	double g_xpApplied = 0.0;

	// game thread (Unreal task): plain number change, no items touched - this part worked fine in 0.27/0.28
	void ApplyXP()
	{
		double total;
		{
			std::lock_guard lock(g_bridgeLock);
			if (!g_bridge.valid) {
				return;
			}
			total = g_bridge.xp;
		}
		if (total < g_xpApplied) {
			g_xpApplied = total;  // the UE4SS mod restarted its count
		}
		const double delta = total - g_xpApplied;
		if (delta <= 0.0) {
			return;
		}
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			constexpr std::int32_t slot = kSneak - kFirstSkill;
			player->skillUsage[slot] += static_cast<float>(delta);
			REX::INFO("bonus XP +{:.3f} -> Sneak XP {:.3f} / {:.3f}", delta, player->skillUsage[slot], player->skillAdvancementUses[slot]);
		}
		g_xpApplied = total;
	}

	std::atomic<bool> g_taskQueued{ false };

	// background thread: read the bridge; apply the penalty at once (works while the steal menu is open)
	void BridgeThread(std::stop_token a_stop)
	{
		RE::Setting* amountBase = nullptr;
		float        applied = 1e9f;
		bool         reported = false;
		int          loops = 0;
		const auto   started = std::filesystem::file_time_type::clock::now();
		auto         readyAt = Clock::time_point::max();
		while (!a_stop.stop_requested()) {
			auto b = ReadBridgeFile();
			if (b.valid) {
				// only a status file written in THIS session counts (an old one is still on disk at startup)
				std::error_code ec;
				const auto written = std::filesystem::last_write_time("PickpocketPlus_bridge.txt", ec);
				if (ec || written < started) {
					b.valid = false;
				}
			}
			if (b.valid) {
				if (!reported) {
					reported = true;
					readyAt = Clock::now() + std::chrono::seconds(3);
					REX::INFO("status file found - connected to the UE4SS mod");
				}
				{
					std::lock_guard lock(g_bridgeLock);
					g_bridge = b;
				}
				if (!amountBase) {
					if (auto gs = RE::GameSettingCollection::GetSingleton()) {
						amountBase = gs->GetSetting("fPickPocketAmountBase");
					}
				}
				if (amountBase && b.penalty != applied) {
					amountBase->data.f = b.penalty;
					applied = b.penalty;
				}
			}
			// every ~200 ms once the game is up: add pending bonus XP on the game thread
			if (Clock::now() >= readyAt && ++loops % 4 == 0 && !g_taskQueued.exchange(true)) {
				const bool queued = UE::AsyncTask(UE::ENamedThreads::GameThread, []() {
					ApplyXP();
					g_taskQueued = false;
				});
				if (!queued) {
					g_taskQueued = false;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	std::jthread g_bridgeThread;

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
		ClearWornFile();  // start with an empty file
		g_bridgeThread = std::jthread(BridgeThread);
	}
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	REX::INFO("PickpocketPlus 0.30 loaded");
	PPXP::Install();
	return true;
}
