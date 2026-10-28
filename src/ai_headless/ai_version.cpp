/***************************************************************************
 * ai_version.cpp — говорящее имя версии ИИИ (см. ai_version.h).
 ***************************************************************************/

#include "ai_version.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace ai
{

namespace
{

// Выключатели опытов правил «вариант против варианта» (читают модули ai_navy, ai_air, ai_commando, ai_eco,
// ai_survey; замороженные base* их не читают). Новый выключатель — добавить сюда.
// switchOff — выключатель «=0» (модуль смотрит только на ведущий «0»); иначе — число: задано — уже опыт.
struct sExperimentKnob
{
	const char* env;
	bool switchOff;
};
constexpr sExperimentKnob kExperimentKnobs[] = {
	{"MAXR_AIR", true},
	{"MAXR_ARMADA", true}, // Р17 (п. 125)
	{"MAXR_AWAC_HIDE", true},
	{"MAXR_CMD_TRAIN", false},
	{"MAXR_CMD_TRAIN_RANK", false},
	{"MAXR_CMD_TURN", false},
	{"MAXR_COMMANDO", true},
	{"MAXR_EARLY_DEFENDER", true}, // Р17 (п. 55)
	{"MAXR_ECO", true},
	{"MAXR_FACTORY_ENERGY", true}, // Р17 (п. 55)
	{"MAXR_LANDLOCK", true},
	{"MAXR_LANDLOCK_FACTORIES", true}, // Р17 (п. 142)
	{"MAXR_NAVY", true},
	{"MAXR_SCOUT", true},
	{"MAXR_SEAMINES", true},
	{"MAXR_SEASUPPLY", true},
	{"MAXR_SURVEY", true},
};

nlohmann::json experimentSettings()
{
	nlohmann::json s = nlohmann::json::object();
	for (const auto& knob : kExperimentKnobs)
	{
		const char* v = std::getenv (knob.env);
		if (v == nullptr) continue;
		if (knob.switchOff && v[0] != '0') continue;
		s[knob.env] = std::string (v);
	}
	return s;
}

bool isQuantizationPart (const std::string& p)
{
	static const char* const known[] = {"qat", "gguf", "ggml", "awq", "gptq", "mlx", "fp16", "bf16", "fp8", "fp32", "int4", "int8", "exl2"};
	for (const char* k : known)
		if (p == k || p.rfind (std::string (k) + "_", 0) == 0) return true;
	// q4_k_m, q8_0, iq3_xs
	if (p.size() >= 2 && p[0] == 'q' && std::isdigit (static_cast<unsigned char> (p[1]))) return true;
	if (p.size() >= 3 && p[0] == 'i' && p[1] == 'q' && std::isdigit (static_cast<unsigned char> (p[2]))) return true;
	return false;
}

} // namespace

//------------------------------------------------------------------------------
std::string shortModelName (const std::string& model)
{
	std::string s = model;
	if (const auto slash = s.find_last_of ("/\\"); slash != std::string::npos) s = s.substr (slash + 1);
	for (auto& c : s)
		c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	if (s.size() > 5 && s.compare (s.size() - 5, 5, ".gguf") == 0) s.resize (s.size() - 5);

	// части через «-»; квантование и формат весов — не имя модели: отрезать с первой такой части
	std::vector<std::string> parts;
	for (std::size_t start = 0; start <= s.size();)
	{
		const auto dash = s.find ('-', start);
		const auto end = dash == std::string::npos ? s.size() : dash;
		parts.push_back (s.substr (start, end - start));
		if (dash == std::string::npos) break;
		start = dash + 1;
	}
	std::size_t keep = 0;
	while (keep < parts.size() && !isQuantizationPart (parts[keep]))
		++keep;
	parts.resize (keep);
	std::erase_if (parts, [] (const std::string& p) { return p.empty(); });

	// «gemma-4-12b» → «gemma4-12b»: имя из букв и номер версии модели — слитно
	if (parts.size() >= 2 && !parts[0].empty() && std::all_of (parts[0].begin(), parts[0].end(), [] (unsigned char c) { return std::isalpha (c); })
		&& std::isdigit (static_cast<unsigned char> (parts[1][0])) && parts[1].find_first_not_of ("0123456789.") == std::string::npos)
	{
		parts[0] += parts[1];
		parts.erase (parts.begin() + 1);
	}

	std::string out;
	for (const auto& p : parts)
		out += (out.empty() ? "" : "-") + p;
	// для лобби и имён файлов: латиница, цифры, «.» и «-»
	for (auto& c : out)
		if (!std::isalnum (static_cast<unsigned char> (c)) && c != '.' && c != '-') c = '-';
	if (out.size() > 24) out.resize (24);
	while (!out.empty() && (out.back() == '-' || out.back() == '.'))
		out.pop_back();
	return out.empty() ? std::string ("local") : out;
}

//------------------------------------------------------------------------------
sAiVersion aiVersion (const std::string& brain)
{
	sAiVersion v;
	v.brain = brain;
	if (brain == "rule" || brain == "llm")
	{
		v.type = "R";
		v.generation = kRuleGeneration;
		v.build = kRuleBuild;
		v.training = "T";
		if (brain == "llm")
		{
			const char* model = std::getenv ("MAXR_LLM_MODEL");
			v.advisor = "+L" + shortModelName (model != nullptr ? model : "");
		}
		v.settings = experimentSettings();
		v.name = "SI-" + v.type + std::to_string (v.generation) + "." + std::to_string (v.build) + "-" + v.training + v.advisor;
		if (!v.settings.empty()) v.name += "~x";
		return v;
	}
	// замороженные чемпионы: base — первое поколение (24.10.2026), base2 — 26.10, base3 — 26.10, base4 — 27.10.2026
	const std::string prefix = "base";
	if (brain.rfind (prefix, 0) == 0)
	{
		const std::string rest = brain.substr (prefix.size());
		const int generation = rest.empty() ? 1 : std::atoi (rest.c_str());
		if (generation >= 1 && generation <= kRuleGeneration && (rest.empty() || rest == std::to_string (generation)))
		{
			v.type = "R";
			v.generation = generation;
			v.name = "SI-R" + std::to_string (generation);
			return v;
		}
	}
	// налётчики red1/red2, сценарий проверки, внешняя программа: не версии ИИИ
	v.type = "X";
	v.name = "SI-X-" + brain;
	return v;
}

//------------------------------------------------------------------------------
nlohmann::json sAiVersion::toJson() const
{
	nlohmann::json j;
	j["brain"] = brain;
	j["name"] = name;
	j["type"] = type;
	if (generation > 0) j["generation"] = generation;
	if (build >= 0) j["build"] = build;
	if (!training.empty()) j["training"] = training;
	if (!advisor.empty()) j["advisor"] = advisor;
	if (!settings.empty()) j["settings"] = settings;
	j["passport"] = kAiPassportUrl;
	return j;
}

} // namespace ai
