/***************************************************************************
 * ai_version.h — говорящее имя версии ИИИ (решение Владыки 27.10.2026, PLAN.md §6
 * «Имена версий ИИИ»): SI-<тип><поколение>.<сборка>-<обучение>[+<советник>].
 *
 *   тип        R — правила (алгоритм); N — своя сеть на ПК; H — гибрид (сеть решает,
 *              правила исполняют); L — языковая модель на ПК; C — модель онлайн;
 *              X — не версия ИИИ: налётчик, сценарий проверки, внешняя программа;
 *   поколение  номер чемпиона (обыграл прежнего, D >= 0.53); сборка — улучшения после него;
 *   обучение   T — настроен турнирами (сам не учился); дальше — I…, S…, P…, F (сети, LLM);
 *   советник   +L<модель> (LM Studio на ПК) / +C<модель> (онлайн).
 *
 * Примеры: rule — SI-R4.0-T; замороженные чемпионы base4 … base — SI-R4 … SI-R1;
 * llm — SI-R4.0-T+Lgemma4-12b (модель из MAXR_LLM_MODEL). Выключатели опытов правил
 * (MAXR_NAVY=0 и другие) дают «~x» в конце: это уже не та версия, что в паспорте.
 * Расшифровка — si/AI_PASSPORT.md. Одна функция для ai_headless и клиента ИИИ:
 * имя в лобби, приветствие в чате, журнал (событие aiVersion) и metrics.json.
 ***************************************************************************/

#ifndef ai_headless_ai_versionH
#define ai_headless_ai_versionH

#include <nlohmann/json.hpp>

#include <string>

namespace ai
{

// Поколение и сборка мозга rule. SI-R4.8-T — base4 и пункты 1–8 части 4б (Р17, 28.10.2026).
// Изменил поведение rule — kRuleBuild + 1; заморозил новым чемпионом (base5) — kRuleGeneration = 5, kRuleBuild = 0.
constexpr int kRuleGeneration = 4;
constexpr int kRuleBuild = 8;

// паспорт ИИИ в открытом форке
constexpr const char* kAiPassportUrl = "https://github.com/UsabilityLightSoft/M.A.X.R.SI/blob/main/si/AI_PASSPORT.md";

struct sAiVersion
{
	std::string brain;    // мозг: rule, base4, llm, red1, …
	std::string name;     // SI-R4.0-T, SI-R4, SI-R4.0-T+Lgemma4-12b, SI-X-red1, …
	std::string type;     // R, L, X (см. шапку)
	int generation = 0;   // 0 — нет (X)
	int build = -1;       // -1 — нет (замороженный чемпион, X)
	std::string training; // T; пусто — нет
	std::string advisor;  // +L<модель>; пусто — нет
	nlohmann::json settings = nlohmann::json::object(); // выключатели опытов, заданные в окружении (дают «~x»)

	// для журнала (событие aiVersion): всё, кроме пустого
	nlohmann::json toJson() const;
};

// имя версии по мозгу и окружению (MAXR_LLM_MODEL, выключатели опытов)
sAiVersion aiVersion (const std::string& brain);

// модель советника коротко, для имени: «google/gemma-4-12b-qat» → «gemma4-12b»; пусто — «local»
std::string shortModelName (const std::string& model);

} // namespace ai

#endif // ai_headless_ai_versionH
