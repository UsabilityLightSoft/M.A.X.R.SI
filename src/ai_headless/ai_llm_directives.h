/***************************************************************************
 * ai_llm_directives.h — директивы LLM-советника (PLAN_AI этап 5, шаг 2).
 *
 * Модель решает немного крупного (что производить, что исследовать, куда
 * вести армию), советник переводит это в обычные приказы cOrder. Здесь же —
 * то, что модель видит кроме digest: каталог техники наших фабрик и краткая
 * сводка, и JSON-схема плана (response_format).
 *
 * Правила (cRuleBrain) не меняются: директивы только заменяют часть их
 * приказов по ключу конфликта (см. llmConflictKey).
 ***************************************************************************/

#ifndef ai_headless_ai_llm_directivesH
#define ai_headless_ai_llm_directivesH

#include "ai_types.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

class cModel;

namespace ai
{

// Выключатели советника (MAXR_LLM_DIRECTIVES, MAXR_LLM_ORDERS,
// MAXR_LLM_PROD_BASE_SHARE). Выключенная директива / orders убираются из
// промпта и схемы; пришедшие всё же — игнорируются (событие в llm.jsonl),
// эта область остаётся правилам.
// Умолчания — по опыту 08_directive_ablation.md и 10_duels_new_rules.md: army и
// orders выключены (дробят армию и перебивают микро), production — с долей
// базовой машины 0.8 по металлу очереди.
struct sLlmSwitches
{
	bool production = true;
	bool research = true;
	bool army = false;
	bool orders = false;         // поюнитные orders (умолчание выкл.: Move модели перебивали боевое микро правил)
	double prodBaseShare = 0.8;  // доля базовой машины правил в очереди фабрики (0 — модель решает всё)
	bool prodShareByMetal = true; // MAXR_LLM_PROD_SHARE_BY=metal (умолчание): доля по металлу (цене) очереди; count — по числу машин

	bool anyDirective() const { return production || research || army; }
	bool anything() const { return anyDirective() || orders; }
	std::string directivesText() const; // «production,research,army» | «none»
};

// базовая машина правил («produce tanks»): id «a.b» — для промпта
std::string llmBaseVehicleId();

// ключ конфликта: приказ правил с тем же ключом заменяется приказом LLM
std::string llmConflictKey (const cOrder& o);

// «a.b» → sTypeId; форму digest «01 22» (sID::getText) тоже принимаем
bool llmParseTypeId (const std::string& s, sTypeId& out);

// Каталог: техника, которую умеют строить наши фабрики (если фабрик нет —
// что строила бы большая фабрика 1.11, первая постройка правил).
// [{"id":"0.32","name":"tank","cost":..,"hp":..,"armor":..,"damage":..,
//   "range":..,"shots":..,"speed":..,"scan":..,"combat":bool}, …]
nlohmann::json buildLlmCatalog (const cModel& model, int playerId);

// Сводка: база, фабрики с очередями, армия, видимые враги, центры исследований.
nlohmann::json buildLlmSummary (const cModel& model, int playerId);

// JSON-схема плана v2 (для response_format json_schema, strict). Ключи
// production — id из каталога; приказы — anyOf по типам. ordered_json: порядок
// полей в схеме задаёт порядок вывода (strategy первой). Выключенных
// директив и orders (sw) в схеме нет.
nlohmann::ordered_json buildLlmPlanSchema (const nlohmann::json& catalog, const sLlmSwitches& sw);

// Итог перевода директив в приказы.
struct sDirectivesResult
{
	std::vector<cOrder> orders;          // приказы, сделанные из директив
	std::vector<nlohmann::json> events;  // события «directive» для llm.jsonl
	int accepted = 0;                    // директив принято (в т.ч. без приказов)
	int rejected = 0;                    // директив отклонено целиком
	int ignored = 0;                     // выключенных директив, пришедших в плане
	std::vector<std::string> problems;   // причины отказов (директив и их частей) — для повтора
};

// directives — объект «directives» из плана. takenKeys — ключи, уже занятые
// явными приказами плана (их директива не трогает). ruleOrders — приказы
// правил этого хода (по ним видно, что правила делают с юнитом/фабрикой).
// sw — какие директивы включены и доля базовой машины в production.
sDirectivesResult translateLlmDirectives (const nlohmann::json& directives, int playerId, const cModel& model,
                                          const std::vector<cOrder>& ruleOrders, const std::set<std::string>& takenKeys,
                                          const sLlmSwitches& sw);

} // namespace ai

#endif // ai_headless_ai_llm_directivesH
