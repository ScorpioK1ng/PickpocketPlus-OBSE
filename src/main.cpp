// PickpocketPlus (OBSE64 plugin) — version 0.4
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

	void Install()
	{
		// unused debug commands, tried in order; the first one found is replaced
		constexpr std::array candidates{
			"OutputMemContexts"sv,
			"OutputMemStats"sv,
			"OutputArchiveProfile"sv,
			"TestCode"sv
		};

		for (const auto name : candidates) {
			if (auto cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand(name)) {
				REX::INFO("replacing unused console command '{}' with PPAddSneakXP", name);
				cmd->functionName = "PPAddSneakXP";
				cmd->shortName = "ppxp";
				cmd->helpString = "PickpocketPlus: adds Sneak XP (PPAddSneakXP <amount>)";
				cmd->referenceFunction = false;
				cmd->SetParameters(g_params);
				cmd->executeFunction = &Execute;
				cmd->editorFilter = false;
				return;
			}
		}

		REX::ERROR("no free console command found - listing all of them:");
		for (auto& cmd : RE::SCRIPT_FUNCTION::GetConsoleFunctions()) {
			if (cmd.functionName) {
				REX::INFO("  {}", cmd.functionName);
			}
		}
	}
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	REX::INFO("PickpocketPlus 0.4 loaded");
	PPXP::Install();
	return true;
}
