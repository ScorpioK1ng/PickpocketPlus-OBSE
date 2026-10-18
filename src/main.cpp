// PickpocketPlus (OBSE64 plugin) — version 0.1
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
		if (amount <= 0.0f) {
			return true;
		}

		auto player = RE::PlayerCharacter::GetSingleton();
		auto data = RE::TESDataHandler::GetSingleton();
		if (!player || !data) {
			REX::WARN("PPAddSneakXP: player or game data not ready");
			return true;
		}

		// The game gives pickpocket XP as "Sneak, action 1 (pickpocket) x modifier".
		// Using the same path keeps our XP identical in kind to the game's own.
		const auto& skill = data->skill[kSneak - kFirstSkill];
		float perUse = skill.data.actionValue[static_cast<std::int32_t>(RE::SKILL_ACTION::kPickPocket)];
		if (perUse <= 0.0f) {
			perUse = 1.0f;
		}
		const float modifier = amount / perUse;

		player->UseSkill(static_cast<RE::ActorValue::Index>(kSneak), RE::SKILL_ACTION::kPickPocket, modifier);

		REX::INFO("PPAddSneakXP: +{:.2f} Sneak XP (pickpocket use value {:.2f}, modifier {:.3f})", amount, perUse, modifier);
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
	REX::INFO("PickpocketPlus 0.1 loaded");
	PPXP::Install();
	return true;
}
