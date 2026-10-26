/***************************************************************************
 * ai_base2_rule_brain.h — замороженная копия ai_rule_brain.h (мозг «base2»).
 *
 * ЗАМОРОЖЕН 26.10.2026, НЕ ПРАВИТЬ — эталон турниров с шага 11 (PLAN.md §6, §7 п. 71):
 * итог шага 10 (exe ai_headless_s10final, full D = 0.909 против base). Копия сделана
 * скриптом tmp/step11/tools/make_base2.py; отличия от ai_rule_brain.h — только пространство имён
 * ai::base2, стражи заголовков, include копий и name() = "base2".
 ***************************************************************************/

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
 * разведка 1–2 машинами, резерв у базы; шаг 9 — роли армии (ai_roles):
 * состав по весам ролей и стратегии, малая фабрика (разведчики, ремонтники,
 * ПВО по угрозе с воздуха), бой по ролям — поддержка за строем бьёт с
 * предельной дальности, ремонтники при армии, ПВО при армии и базе;
 * «подъехал — выстрелил» (выстрел в конце пути); шаг 10 — исследования и
 * золото (ai_tech): центры исследований по доходу, тема по пользе против
 * видимого врага, переработка золота и покупка улучшений, обновление машин в
 * депо. Детерминированный, тестируемый, fallback для LLM-советника.
 ***************************************************************************/

#ifndef ai_headless_ai_base2_rule_brainH
#define ai_headless_ai_base2_rule_brainH

#include "ai_base2_army_ops.h"
#include "ai_brain.h"
#include "ai_base2_roles.h"
#include "ai_base2_tech.h"
#include "ai_base2_world_model.h"

#include "utility/position.h"

#include <map>
#include <vector>

class cModel;

namespace ai::base2
{

// struct cOrder — ai::cOrder (ai_types.h)

class cRuleBrain : public cAiBrain
{
public:
	const char* name() const override { return "base2"; }

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
	// шаг 10: машины, отправленные в депо на обновление (id → ход выбора)
	std::map<unsigned int, int> refit_;
	// память о враге и карта угроз (шаг 7): обновляются в начале каждого хода
	cWorldModel world_;
	// операции армии (шаг 8): живут между ходами
	cArmyOps ops_;
	// роли машин и состав армии (шаг 9): обновляются в начале каждого хода
	cRoleBook roles_;
	cComposition comp_;
	// польза исследований и улучшений за золото (шаг 10): обновляется в начале каждого хода
	cTechAdvisor tech_;
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
		int moveFire = 0;       // шаг 9: приказов «подъехал — выстрелил» (выстрел в конце пути)
		int kites = 0;          // шаг 9: машин поддержки, ушедших из зоны огня врага
		int repairs = 0;        // шаг 9: приказов ремонта
		int rearms = 0;         // шаг 9: приказов пополнить боезапас
		int escorts = 0;        // шаг 9: приказов ПВО и ремонтникам держаться у армии или базы
		int bought = 0;         // шаг 10: кредитов на улучшения (по расчёту мозга)
		int refitLoads = 0;     // шаг 10: машин отправлено в депо на обновление
		int refits = 0;         // шаг 10: приказов обновить машины в депо
	};
	sTurnStats stats_;
};

} // namespace ai::base2

#endif // ai_headless_ai_base2_rule_brainH
