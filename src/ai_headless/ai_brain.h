/***************************************************************************
 * ai_brain.h — интерфейс «мозга» ИИ (PLAN_AI).
 *
 * Движок (cAiEngine) не зависит от конкретного мозга: этап 2 — заглушка,
 * этап 3 — микро, этап 4 — оперативка+стратегия (один cRuleBrain с тремя
 * слоями). Мозг получает модель и возвращает список приказов на ход.
 ***************************************************************************/

#ifndef ai_headless_ai_brainH
#define ai_headless_ai_brainH

#include "ai_types.h"

#include <vector>

class cModel;

namespace ai
{

class cAiBrain
{
public:
	virtual ~cAiBrain() = default;

	// Человекочитаемое имя (для журнала решений).
	virtual const char* name() const = 0;

	// Решить ход: вернуть приказы. Движок валидирует, исполняет, завершает ход.
	virtual std::vector<cOrder> decideTurn (int playerId, const cModel& model) = 0;
};

// Заглушка этапа 2: ничего не делает, ход просто завершается движком.
class cPassiveBrain : public cAiBrain
{
public:
	const char* name() const override { return "passive"; }
	std::vector<cOrder> decideTurn (int, const cModel&) override { return {}; }
};

} // namespace ai

#endif // ai_headless_ai_brainH
