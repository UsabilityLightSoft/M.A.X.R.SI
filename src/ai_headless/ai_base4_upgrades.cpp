/***************************************************************************
 * ai_base4_upgrades.cpp — замороженная копия ai_upgrades.cpp (мозг «base4»).
 *
 * ЗАМОРОЖЕН 27.10.2026, НЕ ПРАВИТЬ — чемпион с шага 13, часть 4а (Р7; PLAN.md §6, мерило — последний
 * чемпион): ИИИ-3.2 + коммандос, эко-сферы и победа по очкам, умный геолог, снабжение флота и ранняя
 * верфь, мины у верфи, AWAC в укрытии, разведка при ненайденном враге (rules/13_4a_economy.md). Копия
 * сделана скриптом tmp/R7/tools/make_base4.py; отличия от ai_upgrades.cpp — только пространство имён ai::base4,
 * стражи заголовков, include копий, name() = "base4" и выключатели опытов (в копии не читаются).
 ***************************************************************************/

/***************************************************************************
 * ai_upgrades.cpp — улучшения за золото и обновление юнитов (шаг 10). Описание — в заголовке.
 ***************************************************************************/

#include "ai_base4_upgrades.h"

#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"

namespace ai::base4
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

} // namespace ai::base4
