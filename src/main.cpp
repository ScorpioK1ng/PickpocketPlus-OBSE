// PickpocketPlus (OBSE64 plugin) — version 0.2
// v0.2: logs Sneak XP before/after; if the pickpocket-action call changes nothing, falls back to the
//       raw-usage UseSkill call. "PPAddSneakXP 0" just logs the current Sneak XP.
//
// Takes over one unused debug console command and turns it into:
//     PPAddSneakXP <amount>        (short name: ppxp)
// which adds <amount> Sneak XP, in the same units as the game's own pickpocket XP.
// The PickpocketPlus UE4SS mod calls it after every successful steal.

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
		auto& skill = data->skill[slot];
		const auto avIndex = static_cast<RE::ActorValue::Index>(kSneak);

		const float before = player->skillUsage[slot];
		REX::INFO("Sneak XP now {:.3f} (advancement uses {:.3f}, skill level count {})",
			before, player->skillAdvancementUses[slot], player->skillLevels[slot]);
		if (amount <= 0.0f) {
			return true;
		}

		// try 1: the same path the game uses for pickpocket XP (Sneak, action 1, x modifier)
		float perUse = skill.data.actionValue[static_cast<std::int32_t>(RE::SKILL_ACTION::kPickPocket)];
		if (perUse <= 0.0f) {
			perUse = 1.0f;
		}
		const float modifier = amount / perUse;
		player->UseSkill(avIndex, RE::SKILL_ACTION::kPickPocket, modifier);
		const float after1 = player->skillUsage[slot];
		REX::INFO("try 1 (action call, use value {:.2f}, modifier {:.3f}): Sneak XP {:.3f} -> {:.3f}", perUse, modifier, before, after1);

		// try 2: raw usage call, only if try 1 changed nothing
		if (after1 == before) {
			player->UseSkill(avIndex, amount, std::addressof(skill));
			const float after2 = player->skillUsage[slot];
			REX::INFO("try 2 (raw usage call): Sneak XP {:.3f} -> {:.3f}", after1, after2);
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
	REX::INFO("PickpocketPlus 0.2 loaded");
	PPXP::Install();
	return true;
}
