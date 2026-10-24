/***************************************************************************
 * ai_rule_brain.h — алгоритмический «мозг» ИИ (правила + эвристики).
 *
 * Три слоя (PLAN_AI): стратегия → оперативка → микро. Этап 3 реализует
 * микро (бой/движение/отход/ремонт/разведка); этап 4 — оперативку и
 * стратегию; шаг 5 PLAN.md — экономику без даровой подпитки движка
 * (бюджет металла, шахты на разведанном металле, соединители, энергия по
 * нефти); шаг 7 — память о враге и карту угроз (ai_world_model): тревога у
 * базы, отход в безопасную клетку, стройщики, геолог и разведка танками —
 * мимо опасных клеток; шаг 8 — операции армии (ai_army_ops): сбор у своего
 * края, поход лидером и ведомыми, атака группой по силе, отход при потерях,
 * разведка 1–2 машинами, резерв у базы. Детерминированный, тестируемый,
 * fallback для LLM-советника.
 ***************************************************************************/

#ifndef ai_headless_ai_rule_brainH
#define ai_headless_ai_rule_brainH

#include "ai_army_ops.h"
#include "ai_brain.h"
#include "ai_world_model.h"

#include "utility/position.h"

#include <map>
#include <vector>

class cModel;

namespace ai
{

struct cOrder;

class cRuleBrain : public cAiBrain
{
public:
	const char* name() const override { return "rule"; }

	std::vector<cOrder> decideTurn (int playerId, const cModel& model) override;
	// решения операций армии за ход (шаг 8) — движок пишет их в журнал (level "operational")
	std::vector<nlohmann::json> takeJournalEvents() override { return ops_.takeEvents(); }
	// вход для директивы советника (шаг 13): цель / собрать / отойти
	void setArmyDirective (const sArmyDirective& d) { ops_.setDirective (d); }
	const cArmyOps& armyOps() const { return ops_; }

	// план стройщика между ходами: что, где (левый верхний угол), скорость, сколько металла нужно
	struct sBuildPlan
	{
		int typeFirst = 0;
		int typeSecond = 0;
		cPosition pos;
		int speed = 0;
		int cost = 0;
	};

private:
	void strategicDecisions (int playerId, const cModel& model, std::vector<cOrder>& out);
	void operationalDecisions (int playerId, const cModel& model, std::vector<cOrder>& out);
	void microDecisions (int playerId, const cModel& model, std::vector<cOrder>& out);

	// планы стройщиков (id машины → план); упорядочено по id — детерминированно
	std::map<unsigned int, sBuildPlan> plans_;
	// машины, которым оперативка уже дала приказ (отогнаны с площадки), — микро их не трогает
	std::vector<unsigned int> opMoved_;
	// ходов подряд, когда стройщику не нашлось пути к площадке (id машины → счётчик)
	std::map<unsigned int, int> stuck_;
	// память о враге и карта угроз (шаг 7): обновляются в начале каждого хода
	cWorldModel world_;
	// операции армии (шаг 8): живут между ходами
	cArmyOps ops_;
	// счётчики хода для строки в журнале движка (поведенческие цифры шага 7)
	struct sTurnStats
	{
		int alarms = 0;         // групп врагов у базы
		int intruders = 0;      // врагов в них
		int defenders = 0;      // назначено защитников
		int reserve = 0;        // бойцов без дела, пошедших к врагам тревоги
		int holds = 0;          // бойцов, ждущих у края зоны огня врага
		int uncovered = 0;      // групп, на которые сил не хватило
		int retreats = 0;       // приказов отхода
		int unsafeRetreats = 0; // из них — в клетку под угрозой («дальность + ход»)
		int fireRetreats = 0;   // из них — в зону огня врага с места
		int evacuations = 0;    // стройщиков и геологов увели из-под угрозы
		int opMoves = 0;        // приказов движения от операций армии (шаг 8)
		int opHolds = 0;        // машин операций, которым велено стоять
	};
	sTurnStats stats_;
};

} // namespace ai

#endif // ai_headless_ai_rule_brainH
