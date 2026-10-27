/***************************************************************************
 * ai_roles.cpp — роли машин и состав армии (шаг 9 PLAN.md). Описание — в заголовке.
 ***************************************************************************/

#include "ai_roles.h"

#include "ai_world_model.h"

#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"

#include <algorithm>
#include <string>

namespace ai
{

namespace
{

// --- константы (подобраны турнирами против base: отчёт шага 9, §4) ---------
// веса ролей строя — ‰ металла армии
constexpr int kWeightTank = 650;
constexpr int kWeightAssault = 100;
constexpr int kWeightArtillery = 150;
constexpr int kWeightMissile = 100;
constexpr int kSiegeDefense = 40;      // вооружённых зданий врага (металл) — стратегия «осада»
constexpr int kLongRangeMin = 48;      // дальнобойных машин врага (металл) — «ответ на дальность»
constexpr int kScouts = 2;             // разведчиков (+1, когда артиллерии и ракет ≥ 2)
constexpr int kRepairMinArmy = 6;      // ремонтники — при армии от стольких машин
constexpr int kRepairPer = 10;         // ремонтник на столько машин армии
constexpr int kRepairMax = 3;
constexpr int kSupplyMinArmy = 8;      // снабженцы — при армии от стольких машин
constexpr int kSupplyPer = 12;
constexpr int kSupplyMax = 2;
constexpr int kAirDecay = 980;         // пик стоимости авиации врага тает за ход до 98% (‰)
constexpr int kAntiAirPct = 75;        // ПВО — на 75% стоимости авиации врага
constexpr int kAntiAirCapPct = 35;     // но не больше 35% металла армии
constexpr int kAntiAirMin = 4;         // и не меньше 4 машин, когда угроза есть

} // namespace

//------------------------------------------------------------------------------
const char* roleName (eRole r)
{
	switch (r)
	{
		case eRole::Tank: return "tank";
		case eRole::Assault: return "assault";
		case eRole::Artillery: return "artillery";
		case eRole::Missile: return "missile";
		case eRole::AntiAir: return "antiair";
		case eRole::Scout: return "scout";
		case eRole::Repair: return "repair";
		case eRole::Supply: return "supply";
		case eRole::Constructor: return "constructor";
		case eRole::Surveyor: return "surveyor";
		case eRole::Guard: return "guard";
		case eRole::Commando: return "commando";
		case eRole::MineLayer: return "minelayer";
		case eRole::Scanner: return "scanner";
		case eRole::Corvette: return "corvette";
		case eRole::NavalGun: return "navalgun";
		case eRole::Submarine: return "submarine";
		case eRole::SeaTransport: return "seatransport";
		case eRole::NavalOther: return "navalother";
		case eRole::Awac: return "awac";
		case eRole::Bomber: return "bomber";
		case eRole::Fighter: return "fighter";
		case eRole::AirTransport: return "airtransport";
		case eRole::AirOther: return "airother";
		case eRole::Apc: return "apc";
		case eRole::Other: return "other";
		default: return "none";
	}
}

//------------------------------------------------------------------------------
eRole cRoleBook::classify (const cStaticUnitData& sd, int range, int scan, int speedMax, int shotsMax, int damage) const
{
	if (!sd.canBuild.empty()) return eRole::Constructor;
	// шаг 13, часть 2: корабли (только вода) — по способностям
	if (sd.factorSea > 0 && sd.factorGround <= 0 && sd.factorAir <= 0)
	{
		const bool shipArmed = sd.canAttack != 0 && damage > 0 && range > 0 && shotsMax > 0;
		if (sd.storageUnitsMax > 0) return eRole::SeaTransport;
		if (shipArmed && (sd.canDetectStealthOn & eTerrainFlag::Sea) != 0) return eRole::Corvette;
		if (shipArmed && (sd.isStealthOn & eTerrainFlag::Sea) != 0) return eRole::Submarine;
		if (shipArmed && (sd.canAttack & eTerrainFlag::Ground) != 0) return eRole::NavalGun;
		return eRole::NavalOther;
	}
	// шаг 13, часть 3: самолёты — по способностям (П15–П17, П10)
	if (sd.factorAir > 0)
	{
		const bool planeArmed = sd.canAttack != 0 && damage > 0 && range > 0 && shotsMax > 0;
		if (sd.storageUnitsMax > 0) return eRole::AirTransport;
		if (planeArmed && (sd.canAttack & (eTerrainFlag::Ground | eTerrainFlag::Sea | eTerrainFlag::AreaSub)) != 0) return eRole::Bomber;
		if (planeArmed && (sd.canAttack & eTerrainFlag::Air) != 0) return eRole::Fighter;
		if (!planeArmed && scan >= 2 * refScan_ + 2) return eRole::Awac;
		return eRole::AirOther;
	}
	if (sd.vehicleData.canSurvey) return eRole::Surveyor;
	// шаг 13: охрана базы — по способностям (данные), не по номерам типов
	if (sd.vehicleData.canDisable || sd.vehicleData.canCapture) return eRole::Commando;
	if (sd.vehicleData.isHuman) return eRole::Guard;
	if (sd.vehicleData.canPlaceMines) return eRole::MineLayer;
	const bool armed = sd.canAttack != 0 && damage > 0 && range > 0 && shotsMax > 0;
	// шаг 13, часть 3 (П2): БМП — наземная машина без оружия, что возит пехоту (и коммандос)
	if (!armed && sd.factorGround > 0 && sd.storageUnitsMax > 0
		&& std::find (sd.storeUnitsTypes.begin(), sd.storeUnitsTypes.end(), "Human") != sd.storeUnitsTypes.end())
		return eRole::Apc;
	if (!armed && !sd.canRepair && !sd.canRearm && sd.factorAir == 0 && sd.factorGround > 0 && sd.storageUnitsMax == 0 && scan >= 2 * refScan_ + 4)
		return eRole::Scanner; // мобильный радар: зоркий, без оружия и груза
	if (!armed) return sd.canRepair ? eRole::Repair : (sd.canRearm ? eRole::Supply : eRole::Other);
	if (sd.factorAir > 0 || sd.factorGround <= 0) return eRole::Other; // авиация и флот — не наша армия
	if ((sd.canAttack & eTerrainFlag::Ground) == 0)
		return (sd.canAttack & eTerrainFlag::Air) != 0 ? eRole::AntiAir : eRole::Other;
	// зоркий и быстрый — разведчик
	if (scan >= 2 * refRange_ && speedMax * 2 >= refSpeed_ * 3) return eRole::Scout;
	if (range <= refRange_) return eRole::Tank;
	if (range <= refRange_ + 2) return eRole::Assault;
	if (range <= refRange_ + 4) return eRole::Artillery;
	return eRole::Missile;
}

//------------------------------------------------------------------------------
void cRoleBook::build (const cModel& model, const cPlayer& player)
{
	model_ = &model;
	roles_.clear();
	cost_.clear();
	order_.clear();
	const auto& ud = *model.getUnitsData();
	// эталон строя: вооружённая по земле машина большой фабрики с наибольшей бронёй.
	// Шаг 10: роль — свойство типа, а не его версии: классификация по исходным данным
	// (с кланом), иначе купленная дальность танка сделала бы его «штурмовым орудием»
	int bestArmor = -1;
	for (const auto& sd : ud.getStaticUnitsData())
	{
		if (!sd.ID.isAVehicle() || sd.isAlien) continue;
		if (player.getLastUnitData (sd.ID) == nullptr) continue;
		const cDynamicUnitData* dd = &ud.getDynamicUnitData (sd.ID, player.getClan());
		if (sd.buildAs != "BigGroundVehicle" || (sd.canAttack & eTerrainFlag::Ground) == 0 || sd.factorAir > 0 || sd.factorGround <= 0) continue;
		if (dd->getDamage() <= 0 || dd->getRange() <= 0 || dd->getShotsMax() <= 0) continue;
		if (dd->getArmor() > bestArmor)
		{
			bestArmor = dd->getArmor();
			refRange_ = dd->getRange();
			refScan_ = dd->getScan();
			refSpeed_ = dd->getSpeedMax();
		}
	}
	for (const auto& sd : ud.getStaticUnitsData())
	{
		if (!sd.ID.isAVehicle()) continue;
		const cDynamicUnitData* last = player.getLastUnitData (sd.ID);
		if (last == nullptr) continue;
		const cDynamicUnitData* dd = &ud.getDynamicUnitData (sd.ID, player.getClan());
		eRole r = classify (sd, dd->getRange(), dd->getScan(), dd->getSpeedMax(), dd->getShotsMax(), dd->getDamage());
		// шаг 13, часть 3 (П2): техника чужих — только захваченная коммандос (своей фабрики у неё нет, typeFor её не
		// выберет); вооружённая наземная — в строй как танк: броня 14 и самопочинка держат удар лучше любого нашего танка
		if (sd.isAlien && isSupportRole (r)) r = eRole::Tank;
		roles_[sd.ID] = r;
		cost_[sd.ID] = last->getBuildCost(); // цена — текущая (исследование «стоимость»)
		order_.push_back (sd.ID);
	}
}

//------------------------------------------------------------------------------
eRole cRoleBook::roleOfType (const sID& id) const
{
	const auto it = roles_.find (id);
	return it == roles_.end() ? eRole::None : it->second;
}

//------------------------------------------------------------------------------
eRole cRoleBook::roleOf (const cVehicle& v) const
{
	return roleOfType (v.getStaticUnitData().ID);
}

//------------------------------------------------------------------------------
eRole cRoleBook::roleOfMemory (const cModel& model, const sEnemyMemory& m) const
{
	if (m.building) return eRole::None;
	const sID id (m.typeFirst, m.typeSecond);
	const auto& ud = *model.getUnitsData();
	if (!ud.isValidId (id)) return eRole::None;
	const auto& sd = ud.getStaticUnitData (id);
	return classify (sd, m.range, m.scan, m.speed * 4, m.shotsMax, m.damage);
}

//------------------------------------------------------------------------------
sID cRoleBook::typeFor (eRole r, const std::string& buildAs) const
{
	if (model_ == nullptr) return sID();
	const auto& ud = *model_->getUnitsData();
	for (const sID& id : order_)
	{
		if (roleOfType (id) != r) continue;
		if (ud.getStaticUnitData (id).buildAs != buildAs) continue;
		return id;
	}
	return sID();
}

//------------------------------------------------------------------------------
int cRoleBook::costOf (const sID& id) const
{
	const auto it = cost_.find (id);
	return it == cost_.end() ? 0 : it->second;
}

//------------------------------------------------------------------------------
int cComposition::armyCount() const
{
	return count (eRole::Tank) + count (eRole::Assault) + count (eRole::Artillery) + count (eRole::Missile);
}

//------------------------------------------------------------------------------
void cComposition::noteQueued (const cRoleBook& book, const sID& id)
{
	const int r = static_cast<int> (book.roleOfType (id));
	++count_[r];
	metal_[r] += book.costOf (id);
	++typeCount_[id];
}

//------------------------------------------------------------------------------
void cComposition::setDefenseNeeds (int pioneers, int minelayers, int surveyors, int scanners)
{
	needs_.pioneers = pioneers;
	needs_.minelayers = minelayers;
	needs_.surveyors = surveyors;
	needs_.scanners = scanners;
}

//------------------------------------------------------------------------------
int cComposition::countType (const sID& id) const
{
	const auto it = typeCount_.find (id);
	return it == typeCount_.end() ? 0 : it->second;
}

//------------------------------------------------------------------------------
void cComposition::update (const cModel& model, const cPlayer& player, const cRoleBook& book, const cWorldModel& world, int)
{
	count_.fill (0);
	metal_.fill (0);
	typeCount_.clear();
	for (const auto& v : player.getVehicles())
	{
		const int r = static_cast<int> (book.roleOf (*v));
		++count_[r];
		metal_[r] += book.costOf (v->getStaticUnitData().ID);
		++typeCount_[v->getStaticUnitData().ID];
	}
	// очереди фабрик — как уже построенные (иначе одна роль набирается в несколько фабрик сразу)
	for (const auto& b : player.getBuildings())
		for (size_t i = 0; i < b->getBuildListSize(); ++i)
			noteQueued (book, b->getBuildListItem (i).getType());

	// что видно у врага
	enemy_ = {};
	const auto& ud = *model.getUnitsData();
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		const sID id (m.typeFirst, m.typeSecond);
		if (!ud.isValidId (id)) continue;
		const auto& sd = ud.getStaticUnitData (id);
		const int cost = ud.getDynamicUnitData (id).getBuildCost();
		if (m.building)
		{
			if (sd.canBuild == "Plane") ++enemy_.airFactories;
			if (m.hitsGround()) enemy_.staticDef += cost;
			continue;
		}
		if (m.plane)
		{
			// угроза нам — самолёты, что бьют по земле или морю (истребители бьют только воздух)
			if (m.armed() && (m.canAttack & (eTerrainFlag::Ground | eTerrainFlag::Sea)) != 0) enemy_.air += cost;
			continue;
		}
		if (!m.hitsGround() || m.ship) continue;
		enemy_.ground += cost;
		if (m.range > book.refRange()) enemy_.longRange += cost;
	}
	airPeak_ = std::max (static_cast<double> (enemy_.air), airPeak_ * kAirDecay / 1000.0);

	// стратегия — множители весов ролей строя
	weight_.fill (0);
	weight_[static_cast<int> (eRole::Tank)] = kWeightTank;
	weight_[static_cast<int> (eRole::Assault)] = kWeightAssault;
	weight_[static_cast<int> (eRole::Artillery)] = kWeightArtillery;
	weight_[static_cast<int> (eRole::Missile)] = kWeightMissile;
	strategy_ = "balanced";
	if (enemy_.staticDef >= kSiegeDefense)
	{
		// оборона из зданий (турели держат дальность танка) — осада из-за строя
		strategy_ = "siege";
		weight_[static_cast<int> (eRole::Artillery)] *= 3;
		weight_[static_cast<int> (eRole::Missile)] *= 3;
	}
	else if (enemy_.longRange * 4 >= enemy_.ground && enemy_.longRange >= kLongRangeMin)
	{
		// дальнобойные машины врага — ответ ракетами (дальше всех) и быстрым сближением строя
		strategy_ = "counter-range";
		weight_[static_cast<int> (eRole::Missile)] *= 2;
		weight_[static_cast<int> (eRole::Tank)] = weight_[static_cast<int> (eRole::Tank)] * 3 / 2;
		weight_[static_cast<int> (eRole::Assault)] = weight_[static_cast<int> (eRole::Assault)] * 3 / 2;
	}

	// нужды малой фабрики
	const int army = armyCount();
	needs_ = {};
	needs_.scouts = kScouts + ((count (eRole::Artillery) + count (eRole::Missile)) >= 2 ? 1 : 0);
	if (army >= kRepairMinArmy)
		needs_.repair = std::clamp (army / kRepairPer, 1, kRepairMax);
	// снабженцы — по армии: поддержка тратит боезапас быстро (ракеты — 6 выстрелов, артиллерия — 16)
	if (army >= kSupplyMinArmy)
		needs_.supply = std::clamp (army / kSupplyPer, 1, kSupplyMax);
	const sID aa = book.typeFor (eRole::AntiAir, "SmallGroundVehicle");
	const int aaCost = std::max (1, book.costOf (aa));
	if (aa != sID() && (airPeak_ > 0 || enemy_.airFactories > 0))
	{
		int armyMetal = 0;
		for (eRole r : {eRole::Tank, eRole::Assault, eRole::Artillery, eRole::Missile})
			armyMetal += metal (r);
		// доля ПВО — 0.75 стоимости авиации врага, не больше 35% армии, но угроза есть — не меньше
		// четырёх машин (половина при армии, половина у базы): опыт с бомбардировщиками — при
		// потолке от малой ранней армии ПВО было 2–3 на 11–13 бомбардировщиков
		int want = static_cast<int> (airPeak_ * kAntiAirPct / 100.0);
		want = std::min (want, armyMetal * kAntiAirCapPct / 100);
		want = std::max (want, kAntiAirMin * aaCost);
		needs_.antiAir = std::max (1, (want + aaCost - 1) / aaCost);
	}
}

//------------------------------------------------------------------------------
sID cComposition::nextBig (const cRoleBook& book, const std::string& buildAs)
{
	// шаг 13: мобильный радар (депо: выйдет, когда база слепнет) — раньше армии, он один
	if (const sID sc = book.typeFor (eRole::Scanner, buildAs); sc != sID() && count (eRole::Scanner) < needs_.scanners) return sc;
	long long total = 0;
	for (eRole r : {eRole::Tank, eRole::Assault, eRole::Artillery, eRole::Missile})
		total += metal (r);
	int wsum = 0;
	for (eRole r : {eRole::Tank, eRole::Assault, eRole::Artillery, eRole::Missile})
		if (book.typeFor (r, buildAs) != sID()) wsum += weight_[static_cast<int> (r)];
	if (wsum <= 0) return sID();
	sID best;
	long long bestDeficit = 0;
	bool any = false;
	for (eRole r : {eRole::Tank, eRole::Assault, eRole::Artillery, eRole::Missile})
	{
		const sID t = book.typeFor (r, buildAs);
		const int w = weight_[static_cast<int> (r)];
		if (t == sID() || w <= 0) continue;
		// нехватка доли металла после этой машины (×wsum, чтобы считать в целых)
		const long long deficit = static_cast<long long> (w) * (total + book.costOf (t)) - static_cast<long long> (metal (r)) * wsum;
		if (!any || deficit > bestDeficit)
		{
			any = true;
			bestDeficit = deficit;
			best = t;
		}
	}
	return best;
}

//------------------------------------------------------------------------------
sID cComposition::nextSmall (const cRoleBook& book, const std::string& buildAs)
{
	const sID scout = book.typeFor (eRole::Scout, buildAs);
	const sID aa = book.typeFor (eRole::AntiAir, buildAs);
	const sID repair = book.typeFor (eRole::Repair, buildAs);
	// угроза с воздуха — ПВО раньше всего; иначе первый разведчик, потом остальное
	if (aa != sID() && count (eRole::AntiAir) < needs_.antiAir) return aa;
	if (scout != sID() && count (eRole::Scout) < std::min (1, needs_.scouts)) return scout;
	// шаг 13: оборона — второй пионер (радары, ПВО-пушки, стены), минёр, геолог при армии
	if (const sID pio = book.typeFor (eRole::Constructor, buildAs); pio != sID() && countType (pio) < needs_.pioneers) return pio;
	if (const sID ml = book.typeFor (eRole::MineLayer, buildAs); ml != sID() && count (eRole::MineLayer) < needs_.minelayers) return ml;
	if (const sID sv = book.typeFor (eRole::Surveyor, buildAs); sv != sID() && count (eRole::Surveyor) < 1 + needs_.surveyors) return sv;
	if (const sID supply = book.typeFor (eRole::Supply, buildAs); supply != sID() && count (eRole::Supply) < needs_.supply) return supply;
	if (repair != sID() && count (eRole::Repair) < needs_.repair) return repair;
	if (scout != sID() && count (eRole::Scout) < needs_.scouts) return scout;
	return sID();
}

} // namespace ai
