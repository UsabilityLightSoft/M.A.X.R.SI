/***************************************************************************
 * ai_base3_upgrades.cpp — замороженная копия ai_upgrades.cpp (мозг «base3»).
 *
 * ЗАМОРОЖЕН 26.10.2026, НЕ ПРАВИТЬ — чемпион с шага 13, часть 2 (PLAN.md §6, мерило — последний
 * чемпион): итог шага 13, часть 1 (rules/13_1_defense.md: оборона, высадка из файла карты,
 * скорость; full D = 0.565 против base2) на движке с правками 09–12. Копия сделана скриптом
 * tmp/step13/tools/make_base3.py; отличия от ai_upgrades.cpp — только пространство имён ai::base3,
 * стражи заголовков, include копий и name() = "base3".
 ***************************************************************************/

/***************************************************************************
 * ai_upgrades.cpp — улучшения за золото и обновление юнитов (шаг 10). Описание — в заголовке.
 ***************************************************************************/

#include "ai_base3_upgrades.h"

#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"

namespace ai::base3
{

//------------------------------------------------------------------------------
const char* upgradeKindName (int kind)
{
	static constexpr const char* names[8] = {"damage", "shots", "range", "ammo", "armor", "hits", "scan", "speed"};
	return kind >= 0 && kind < 8 ? names[kind] : "none";
}

//------------------------------------------------------------------------------
bool makeUnitUpgrade (const cModel& model, const cPlayer& player, const sID& id, const std::array<int, 8>& steps,
	cUnitUpgrade& out, int& cost, std::string* why)
{
	auto fail = [&] (const std::string& w) {
		if (why) *why = w;
		cost = 0;
		return false;
	};
	const auto& ud = *model.getUnitsData();
	if (!ud.isValidId (id)) return fail ("неизвестный тип юнита");
	const cDynamicUnitData* cur = player.getLastUnitData (id);
	if (cur == nullptr) return fail ("нет текущей версии типа у игрока");
	const cDynamicUnitData& orig = ud.getDynamicUnitData (id, player.getClan());
	const cStaticUnitData& sd = ud.getStaticUnitData (id);
	out = cUnitUpgrade();
	out.init (orig, *cur, sd, player.getResearchState());
	int total = 0;
	for (size_t k = 0; k < kUpgradeKinds.size(); ++k)
	{
		if (steps[k] < 0) return fail ("отрицательное число шагов");
		if (steps[k] == 0) continue;
		sUnitUpgrade* u = out.getUpgrade (kUpgradeKinds[k]);
		if (u == nullptr) return fail (std::string ("у типа нет улучшения ") + upgradeKindName (static_cast<int> (k)));
		for (int s = 0; s < steps[k]; ++s)
		{
			if (!u->getNextPrice() || *u->getNextPrice() <= 0)
				return fail (std::string ("улучшение ") + upgradeKindName (static_cast<int> (k)) + " — предел таблицы цен");
			u->purchase (player.getResearchState());
			++total;
		}
	}
	if (total == 0) return fail ("нечего покупать");
	cost = out.calcTotalCosts (orig, *cur, player.getResearchState());
	if (cost <= 0) return fail ("движок не считает цену");
	return true;
}

//------------------------------------------------------------------------------
int refitMetalCost (const cPlayer& player, const sID& id)
{
	const cDynamicUnitData* last = player.getLastUnitData (id);
	if (last == nullptr) return 0;
	return cUpgradeCalculator::instance().getMaterialCostForUpgrading (last->getBuildCost());
}

//------------------------------------------------------------------------------
bool outdated (const cUnit& unit)
{
	const cPlayer* owner = unit.getOwner();
	if (owner == nullptr) return false;
	const cDynamicUnitData* last = owner->getLastUnitData (unit.data.getId());
	return last != nullptr && unit.data.canBeUpgradedTo (*last);
}

} // namespace ai::base3
