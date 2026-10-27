/***************************************************************************
 * ai_base4_upgrades.h — замороженная копия ai_upgrades.h (мозг «base4»).
 *
 * ЗАМОРОЖЕН 27.10.2026, НЕ ПРАВИТЬ — чемпион с шага 13, часть 4а (Р7; PLAN.md §6, мерило — последний
 * чемпион): ИИИ-3.2 + коммандос, эко-сферы и победа по очкам, умный геолог, снабжение флота и ранняя
 * верфь, мины у верфи, AWAC в укрытии, разведка при ненайденном враге (rules/13_4a_economy.md). Копия
 * сделана скриптом tmp/R7/tools/make_base4.py; отличия от ai_upgrades.h — только пространство имён ai::base4,
 * стражи заголовков, include копий, name() = "base4" и выключатели опытов (в копии не читаются).
 ***************************************************************************/

/***************************************************************************
 * ai_upgrades.h — улучшения юнитов за золото и обновление до текущей версии
 * (шаг 10 PLAN.md): общие расчёты для движка ИИИ, валидатора и мозга.
 *
 * Как в движке (cActionBuyUpgrades, cActionUpgradeVehicle/Building): улучшения
 * типа покупаются за кредиты (их даёт золото через переработку), действуют на
 * новые машины этого типа; уже стоящие обновляются до текущей версии за металл
 * (четверть цены постройки) — машина в хранилище (депо), здание на месте.
 * Исследования действуют так же: меняют текущую версию типа, а не стоящие юниты.
 ***************************************************************************/

#ifndef ai_headless_ai_base4_upgradesH
#define ai_headless_ai_base4_upgradesH

#include "game/data/units/id.h"
#include "game/logic/upgradecalculator.h"

#include <array>
#include <optional>
#include <string>

class cBuilding;
class cModel;
class cPlayer;
class cUnit;

namespace ai::base4
{

// виды улучшений за золото — порядок cOrder::upgradeSteps
inline constexpr std::array<sUnitUpgrade::eUpgradeType, 8> kUpgradeKinds = {
	sUnitUpgrade::eUpgradeType::Damage, sUnitUpgrade::eUpgradeType::Shots, sUnitUpgrade::eUpgradeType::Range,
	sUnitUpgrade::eUpgradeType::Ammo, sUnitUpgrade::eUpgradeType::Armor, sUnitUpgrade::eUpgradeType::Hits,
	sUnitUpgrade::eUpgradeType::Scan, sUnitUpgrade::eUpgradeType::Speed};
const char* upgradeKindName (int kind);

// Улучшение типа id у игрока на steps шагов (по видам kUpgradeKinds) — как его
// собирает окно улучшений игры (cUnitUpgrade::init + purchase). cost — цена в
// кредитах по расчёту движка (cUnitUpgrade::calcTotalCosts). false — нельзя
// (why — почему: тип, вид без улучшения, предел таблицы цен).
bool makeUnitUpgrade (const cModel& model, const cPlayer& player, const sID& id, const std::array<int, 8>& steps,
	cUnitUpgrade& out, int& cost, std::string* why = nullptr);

// металл на обновление юнита типа id до текущей версии (как cActionUpgradeVehicle)
int refitMetalCost (const cPlayer& player, const sID& id);
// юнит отстал от текущей версии своего типа (исследования или купленные улучшения)
bool outdated (const cUnit& unit);

} // namespace ai::base4

#endif // ai_headless_ai_base4_upgradesH
