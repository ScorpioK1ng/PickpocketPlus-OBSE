// PickpocketPlus (OBSE64 plugin) — version 0.27
// Companion DLL for the PickpocketPlus UE4SS mod - NO CONSOLE COMMANDS anymore (the game's console crashed
// now and then when a command landed at a bad moment). Instead:
//   - the UE4SS mod writes PickpocketPlus_bridge.txt (sneaking, steal menu open, loads, area changes, bonus XP
//     total, worn penalty);
//   - a small background thread reads it every 50 ms and applies the worn penalty (a game setting) at once;
//   - everything else (copies of worn gear, cleanup, save repair, bonus XP) runs inside the player's own
//     per-frame update, i.e. at a moment the game is in a stable state by definition.
// Files: PickpocketPlus_worn.txt (worn copies, for the UE4SS mod), PickpocketPlus_ledger.txt (save repair).
//
// v0.27: never touches NPCs while the game is loading or changing areas (the UE4SS mod sets "wait" in the
//        status file the moment a load/fade starts); a failed task hand-off can no longer stall the work.
// v0.26: startup fix: nothing is sent to the game thread until the UE4SS mod has written a FRESH status file
//        in this session (+3 s) - 0.25 queued work while the engine was still starting (game wouldn't launch,
//        an old status file from the last session was already on disk). Player update hook removed.
// v0.25: the per-frame work is now queued onto the game thread through Unreal's own task system (the player
//        update hook never ran in 0.24); skips all item work while any game menu is open; logs once when the
//        status file is found and when the game-thread work starts.
// v0.24: console-free rewrite (status file + player update hook); held weapons are copied again.
// History: 0.1-0.4 Sneak XP; 0.5-0.8 worn gear by undressing; 0.9-0.23 worn gear via copies (console based).

namespace PPXP
{
	using Clock = std::chrono::steady_clock;

	constexpr std::int32_t kSneak = 31;       // actor value index of Sneak
	constexpr std::int32_t kFirstSkill = 12;  // TESDataHandler::skill[0] is Armorer (12)

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
		std::int32_t baseline = a_baseline;
		for (auto& e : list) {
			if (e.npc == a_npc && e.object == a_object && e.baseline < baseline) {
				baseline = e.baseline;  // never raise it: a loaded save may already hold a copy
			}
		}
		std::erase_if(list, [&](const LedgerEntry& e) { return e.npc == a_npc && e.object == a_object; });
		list.push_back({ a_npc, a_object, baseline });
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
				npc->RemoveItem(object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
				++repaired;
			}
			if (now > e.baseline && !WornListOf(npc, object) && npc->GetItemCountInContainer(object) > 0) {
				npc->EquipObject(object, 1, nullptr);
			}
		}
		REX::INFO("cleanup: repair after load - {} saved copies removed", repaired);
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
					npc->RemoveItem(c.object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
					++removedCopies;
					if (!WornListOf(npc, c.object) && npc->GetItemCountInContainer(c.object) > 0) {
						npc->EquipObject(c.object, 1, nullptr);
					}
				} else {
					// the copy was stolen: the real worn piece goes too
					if (auto worn = WornListOf(npc, c.object)) {
						npc->UnequipObject(c.object, 1, worn, true);
					}
					if (npc->GetItemCountInContainer(c.object) > 0) {
						npc->RemoveItem(c.object, nullptr, 1, false, false, nullptr, nullptr, nullptr, true, false);
					}
					++takenWorn;
					REX::INFO("  {:08X} was stolen - real worn piece removed", c.object->formID);
				}
			}
			REX::INFO("cleanup: {:08X}: {} copies removed, {} worn pieces taken", g_prepID, removedCopies, takenWorn);
		}
		g_lastCleanID = g_prepID;
		g_lastCleanTime = std::chrono::steady_clock::now();
		g_copies.clear();
		g_prepID = 0;
		WriteWornFile(g_copies);
	}


	// ---------------- copies: prepare (game thread only) ----------------
	void Prepare(float a_maxDist, bool a_skipProtected)
	{
		auto im = RE::InterfaceManager::GetInstance(false, false);
		auto npc = im ? AsNPC(im->crosshairRef) : nullptr;
		if (npc && DistanceToPlayer(npc) > a_maxDist) {
			npc = nullptr;
		}
		if (!npc) {
			npc = FindTargetNPC(a_maxDist);
		}
		if (!npc) {
			npc = FindNearestNPC(a_maxDist);
		}
		if (!npc) {
			// nobody in reach: drop the prepared NPC once we've walked away from them
			if (g_prepID) {
				auto prepared = LookupNPC(g_prepID);
				if (!prepared || DistanceToPlayer(prepared) > a_maxDist * 1.5f) {
					Cleanup();
				}
			}
			return;
		}
		if (npc->formID == g_prepID) {
			// already prepared: make sure the AI didn't take a real piece off (e.g. the shield)
			if (Clock::now() - g_prepTime < std::chrono::seconds(2)) {
				return;
			}
			for (auto& c : g_copies) {
				if (c.redressTries < 1 && !WornListOf(npc, c.object) && npc->GetItemCountInContainer(c.object) > 0) {
					++c.redressTries;
					npc->EquipObject(c.object, 1, nullptr);
					REX::INFO("  {:08X} was taken off by the AI - put back on", c.object->formID);
				}
			}
			return;
		}
		if (g_prepID) {
			Cleanup();  // switched to another NPC: the old one gets their real gear back first
		}
		if (npc->formID == g_lastCleanID && Clock::now() - g_lastCleanTime < std::chrono::seconds(5)) {
			return;  // this NPC's AI may still be re-checking its gear after the last cleanup
		}

		auto changes = ChangesOf(npc);
		if (!changes) {
			return;
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
			if (a_skipProtected && IsProtected(change->object)) {
				continue;
			}
			worn.push_back(change->object);
		}

		g_prepID = npc->formID;
		g_prepTime = Clock::now();
		g_copies.clear();
		for (auto object : worn) {
			RememberCopy(npc->formID, object->formID, npc->GetItemCountInContainer(object));
			npc->AddObjectToContainer(object, nullptr, 1);
			g_copies.push_back({ object, npc->GetItemCountInContainer(object) });
		}
		WriteWornFile(g_copies);
		REX::INFO("prepare: {:08X}: {} worn item copies added", g_prepID, g_copies.size());
	}

	void Forget()
	{
		if (g_prepID) {
			REX::INFO("forgetting {:08X} after a load (inventory untouched)", g_prepID);
		}
		g_copies.clear();
		g_prepID = 0;
		WriteWornFile(g_copies);
	}

	// ---------------- bridge: what the UE4SS mod tells us ----------------
	struct Bridge
	{
		bool        valid = false;
		bool        on = true;      // worn gear feature on
		bool        sneak = false;  // player crouched
		bool        menu = false;   // steal menu open
		std::int64_t load = 0;      // +1 after every save load
		std::int64_t area = 0;      // +1 after every area change (door / fast travel)
		double      xp = 0.0;       // bonus XP total sent this session
		float       penalty = 0.0f; // fPickPocketAmountBase to use right now
		float       dist = 300.0f;
		bool        skip = true;
		bool        wait = false;   // loading / changing areas: hands off
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
			if (key == "on") b.on = v != 0.0;
			else if (key == "sneak") b.sneak = v != 0.0;
			else if (key == "menu") b.menu = v != 0.0;
			else if (key == "load") b.load = static_cast<std::int64_t>(v);
			else if (key == "area") b.area = static_cast<std::int64_t>(v);
			else if (key == "xp") b.xp = v;
			else if (key == "pen") b.penalty = static_cast<float>(v);
			else if (key == "dist") b.dist = static_cast<float>(v);
			else if (key == "skip") b.skip = v != 0.0;
			else if (key == "wait") b.wait = v != 0.0;
		}
		b.valid = ended;  // only a fully written file counts
		return b;
	}

	Bridge CurrentBridge()
	{
		std::lock_guard lock(g_bridgeLock);
		return g_bridge;
	}

	// background thread: read the bridge, apply the worn penalty right away (works while menus are open)
	void Tick();  // defined further down, runs on the game thread

	std::atomic<bool> g_taskQueued{ false };

	void BridgeThread(std::stop_token a_stop)
	{
		RE::Setting* amountBase = nullptr;
		float        applied = 1e9f;
		bool         reportedFound = false, reportedMissing = false;
		int          loops = 0;
		const auto   started = std::filesystem::file_time_type::clock::now();
		auto         readyAt = Clock::time_point::max();   // game-thread work only once the game is up
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
			if (!b.valid && !reportedMissing && !reportedFound) {
				reportedMissing = true;
				std::error_code ec;
				REX::INFO("waiting for {} (written by the UE4SS mod)", std::filesystem::absolute("PickpocketPlus_bridge.txt", ec).string());
			}
			if (b.valid && !reportedFound) {
				reportedFound = true;
				readyAt = Clock::now() + std::chrono::seconds(3);
				REX::INFO("status file found - connected to the UE4SS mod");
			}
			// every ~200 ms: run the item work on the game thread (one task at a time)
			if (Clock::now() >= readyAt && ++loops % 4 == 0 && !g_taskQueued.exchange(true)) {
				const bool queued = UE::AsyncTask(UE::ENamedThreads::GameThread, []() {
					Tick();
					g_taskQueued = false;
				});
				if (!queued) {
					g_taskQueued = false;  // try again next round
				}
			}
			if (b.valid) {
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
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	// ---------------- the per-frame work (inside the player's update) ----------------
	std::int64_t      g_seenLoad = -1, g_seenArea = -1;
	double            g_xpApplied = 0.0;
	bool              g_menuWasOpen = false;
	Clock::time_point g_nextTick{}, g_repairAt{}, g_cleanupAt{}, g_prepAllowedAt{}, g_standSince{};
	bool              g_repairPending = false, g_cleanupPending = false, g_standing = false;

	void ApplyXP(double a_total)
	{
		if (a_total < g_xpApplied) {
			g_xpApplied = a_total;  // the UE4SS mod restarted its count
		}
		const double delta = a_total - g_xpApplied;
		if (delta <= 0.0) {
			return;
		}
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			constexpr std::int32_t slot = kSneak - kFirstSkill;
			player->skillUsage[slot] += static_cast<float>(delta);
			REX::INFO("bonus XP +{:.3f} -> Sneak XP {:.3f} / {:.3f}", delta, player->skillUsage[slot], player->skillAdvancementUses[slot]);
		}
		g_xpApplied = a_total;
	}

	bool GameMenuOpen()
	{
		auto im = RE::InterfaceManager::GetInstance(false, false);
		return im && im->menuMode != 0;
	}

	void Tick()
	{
		const auto now = Clock::now();
		if (now < g_nextTick) {
			return;
		}
		g_nextTick = now + std::chrono::milliseconds(200);

		static bool announced = false;
		if (!announced) {
			announced = true;
			REX::INFO("game-thread work is running");
		}

		const auto b = CurrentBridge();
		if (!b.valid) {
			return;
		}

		ApplyXP(b.xp);

		// loading or changing areas: hands off until the UE4SS mod says the world is ready again
		if (b.wait) {
			return;
		}
		if (auto player = RE::PlayerCharacter::GetSingleton(); !player || !player->parentCell) {
			return;
		}

		// a save was loaded: forget (the NPC was reloaded without our copies), repair saved copies a bit later
		if (b.load != g_seenLoad) {
			if (g_seenLoad != -1 || b.load > 0) {
				Forget();
				g_repairPending = true;
				g_repairAt = now + std::chrono::seconds(3);
				g_cleanupPending = false;
			}
			g_seenLoad = b.load;
		}
		// an area change: forget the old NPC (never touch an NPC right after a load/door - the ledger
		// repair below removes any leftover copies safely, whether they still exist or not)
		if (b.area != g_seenArea) {
			if (g_seenArea != -1) {
				Forget();
				g_repairPending = true;
				g_repairAt = now + std::chrono::seconds(2);
			}
			g_seenArea = b.area;
		}
		const bool anyMenu = GameMenuOpen();
		static int lastMenuState = -1;
		if (static_cast<int>(anyMenu) != lastMenuState) {
			lastMenuState = static_cast<int>(anyMenu);
			REX::INFO("game menu mode: {}", anyMenu ? "on" : "off");
		}
		if (g_repairPending && now >= g_repairAt && !b.menu && !anyMenu) {
			g_repairPending = false;
			RepairAfterLoad();
		}

		if (!b.on) {
			if (g_prepID && !b.menu && !anyMenu) {
				Cleanup();
			}
			return;
		}

		// steal menu: leave everything alone while it's open; clean up shortly after it closes
		if (b.menu) {
			g_menuWasOpen = true;
			return;
		}
		if (anyMenu) {
			return;  // pause menu, inventory, console...: never touch NPCs then
		}
		if (g_menuWasOpen) {
			g_menuWasOpen = false;
			g_cleanupPending = true;
			g_cleanupAt = now + std::chrono::milliseconds(500);
			g_prepAllowedAt = now + std::chrono::seconds(2);
		}
		if (g_cleanupPending) {
			if (now < g_cleanupAt) {
				return;
			}
			g_cleanupPending = false;
			if (g_prepID) {
				Cleanup();
			}
		}

		if (b.sneak) {
			g_standing = false;
			if (now >= g_prepAllowedAt) {
				Prepare(b.dist, b.skip);
			}
		} else if (g_prepID) {
			// stood up: keep the copies 10 s (crouching again soon needs no remove + re-add)
			if (!g_standing) {
				g_standing = true;
				g_standSince = now;
			} else if (now - g_standSince > std::chrono::seconds(10)) {
				Cleanup();
				g_standing = false;
			}
		}
	}

	std::jthread g_bridgeThread;

	void Install()
	{
		WriteWornFile(g_copies);  // start with an empty file
		g_bridgeThread = std::jthread(BridgeThread);
	}
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	REX::INFO("PickpocketPlus 0.27 loaded");
	PPXP::Install();
	return true;
}
