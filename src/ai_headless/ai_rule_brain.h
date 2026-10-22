/***************************************************************************
 * ai_rule_brain.h — алгоритмический «мозг» ИИ (правила + эвристики).
 *
 * Три слоя (PLAN_AI): стратегия → оперативка → микро. Этап 3 реализует
 * микро (бой/движение/отход/ремонт/разведка); этап 4 — оперативку и
 * стратегию. Детерминированный, тестируемый, fallback для LLM-советника.
 ***************************************************************************/

#ifndef ai_headless_ai_rule_brainH
#define ai_headless_ai_rule_brainH

#include "ai_brain.h"

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

private:
	void strategicDecisions (int playerId, const cModel& model, std::vector<cOrder>& out);
	void operationalDecisions (int playerId, const cModel& model, std::vector<cOrder>& out);
	void microDecisions (int playerId, const cModel& model, std::vector<cOrder>& out);
};

} // namespace ai

#endif // ai_headless_ai_rule_brainH
