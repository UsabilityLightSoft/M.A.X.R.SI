/***************************************************************************
 * ai_base_brain.h — замороженная копия ai_rule_brain.h (мозг «base»).
 *
 * ЗАМОРОЖЕН 24.10.2026, НЕ ПРАВИТЬ — эталон для турниров шагов 7–12 (PLAN.md §6):
 * копия ai_rule_brain.{h,cpp} на 24.10.2026 (эталоны rule v12/v13), мозг «base».
 * Правила усиливаются в ai_rule_brain.cpp; эта копия не меняется, чтобы турнир
 * «rule против base» мерил именно изменения шага. Отличия от копии — только
 * имена: класс cBaseRuleBrain, страж заголовка, name() = "base".
 *
 * Описание копии:
 * Три слоя (PLAN_AI): стратегия → оперативка → микро. Этап 3 реализует
 * микро (бой/движение/отход/ремонт/разведка); этап 4 — оперативку и
 * стратегию; шаг 8 этапа 5 — экономику без даровой подпитки движка
 * (бюджет металла, шахты на разведанном металле, соединители, энергия по
 * нефти). Детерминированный, тестируемый, fallback для LLM-советника.
 ***************************************************************************/

#ifndef ai_headless_ai_base_brainH
#define ai_headless_ai_base_brainH

#include "ai_brain.h"

#include "utility/position.h"

#include <map>
#include <vector>

class cModel;

namespace ai
{

struct cOrder;

class cBaseRuleBrain : public cAiBrain
{
public:
	const char* name() const override { return "base"; }

	std::vector<cOrder> decideTurn (int playerId, const cModel& model) override;

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
};

} // namespace ai

#endif // ai_headless_ai_base_brainH
