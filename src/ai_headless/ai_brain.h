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

#include <nlohmann/json.hpp>

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
	// Это первый заход хода (шаг 11: ход в несколько заходов, см. ниже).
	virtual std::vector<cOrder> decideTurn (int playerId, const cModel& model) = 0;

	// Ход в несколько заходов (шаг 11). После decideTurn (заход 1) и после каждого
	// decidePass движок спрашивает wantsAnotherPass(): true — исполнить приказы захода
	// до конца (движение, выстрелы, погрузка доиграны, ход остаётся у игрока) и позвать
	// decidePass со следующим номером захода (2, 3, …) на новом поле; false — сдать ход.
	// Предел заходов — у движка (MAXR_PASSES); заход без исполненных приказов — последний.
	// По умолчанию — один заход, как до шага 11 (base, base2).
	virtual bool wantsAnotherPass() const { return false; }
	virtual std::vector<cOrder> decidePass (int /*playerId*/, const cModel& /*model*/, int /*pass*/) { return {}; }

	// Решения мозга за последний decideTurn, которые не приказы (шаг 8: операции
	// армии — создана, состояние, цель, сила/нужно, отход): движок пишет их в
	// journal.jsonl до приказов хода. По умолчанию — нет.
	virtual std::vector<nlohmann::json> takeJournalEvents() { return {}; }

	// Шаг 13: «нет цели» в unitId2 — kNoUnit (ai_types.h). Мозг, у которого 0 в unitId2 приказов Move с
	// выстрелом в конце пути, Attack и UpgradeVehicle значит «нет» (так было до шага 13: base, base2,
	// сторонние мозги), отвечает true — движок переводит такой 0 в kNoUnit, и его поведение не меняется.
	// Мозг, знающий kNoUnit (правила, советник, сценарий), отвечает false: 0 у него — машина с id 0.
	virtual bool zeroMeansNoUnit() const { return true; }

	// Шаг 13: свои юниты, выключенные коммандос врага с прошлого хода (id) — движок зовёт перед decideTurn.
	// Выключение на 1 ход снимается в начале нашего хода, и к решению в модели его уже не видно (человек
	// в окне видит сообщение о выключении). По умолчанию — не нужно.
	virtual void noteDisabled (const std::vector<unsigned int>& /*ids*/) {}
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
