/***************************************************************************
 * ai_game_state.cpp — сборка GameState digest в JSON (PLAN_AI этап 2).
 ***************************************************************************/

#include "ai_game_state.h"

#include "game/data/base/base.h"
#include "game/data/map/map.h"
#include "game/data/miningresource.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unit.h"
#include "game/data/units/commandodata.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/movejob.h"
#include "game/logic/turncounter.h"
#include "game/logic/upgradecalculator.h"
#include "utility/string/utf-8.h"

#include <algorithm>

namespace ai
{

namespace
{

const char* researchAreaName (cResearch::eResearchArea a)
{
	switch (a)
	{
		case cResearch::eResearchArea::AttackResearch: return "attack";
		case cResearch::eResearchArea::ShotsResearch: return "shots";
		case cResearch::eResearchArea::RangeResearch: return "range";
		case cResearch::eResearchArea::ArmorResearch: return "armor";
		case cResearch::eResearchArea::HitpointsResearch: return "hitpoints";
		case cResearch::eResearchArea::SpeedResearch: return "speed";
		case cResearch::eResearchArea::ScanResearch: return "scan";
		case cResearch::eResearchArea::CostResearch: return "cost";
	}
	return "?";
}

const char* resTypeName (eResourceType t)
{
	switch (t)
	{
		case eResourceType::Metal: return "metal";
		case eResourceType::Oil: return "oil";
		case eResourceType::Gold: return "gold";
		default: return "none";
	}
}

nlohmann::json posJson (const cPosition& p)
{
	return nlohmann::json{{"x", p.x()}, {"y", p.y()}};
}

nlohmann::json resJson (const sMiningResource& r)
{
	return nlohmann::json{{"metal", r.metal}, {"oil", r.oil}, {"gold", r.gold}};
}

// имя типа по экземпляру юнита (надёжнее, чем lookup по id)
std::string unitTypeName (const cUnit& u)
{
	return u.getStaticUnitData().getDefaultName();
}

std::string unitTypeIdText (const cUnit& u)
{
	return u.getStaticUnitData().ID.getText();
}

// имя типа по sID (для очередей строительства)
std::string typeNameById (const cModel& model, const sID& id)
{
	const auto& unitsData = model.getUnitsData();
	if (unitsData && unitsData->isValidId (id))
		return unitsData->getStaticUnitData (id).getDefaultName();
	return id.getText();
}

// шаг 11: груз хранилища (транспорт, депо, ангар, док, казарма) — что внутри и сколько мест
void addCargo (nlohmann::json& j, const cUnit& u)
{
	const int places = u.getStaticUnitData().storageUnitsMax;
	if (places <= 0) return;
	nlohmann::json cargo = nlohmann::json::array();
	for (const cVehicle* v : u.storedUnits)
		cargo.push_back ({{"id", v->getId()}, {"type", unitTypeName (*v)}});
	j["cargo"] = std::move (cargo);
	j["cargoMax"] = places;
}

nlohmann::json buildingJson (const cBuilding& b, const cModel& model)
{
	nlohmann::json j;
	j["id"] = b.getId();
	j["type"] = unitTypeName (b);
	j["typeId"] = unitTypeIdText (b);
	j["owner"] = b.getOwner() ? b.getOwner()->getId() : -1;
	j["pos"] = posJson (b.getPosition());
	j["hp"] = b.data.getHitpoints();
	j["hpMax"] = b.data.getHitpointsMax();
	j["armor"] = b.data.getArmor();
	j["scan"] = b.data.getScan();
	j["disabled"] = b.isDisabled();
	if (b.isDisabled()) j["disabledTurns"] = b.getDisabledTurns(); // шаг 11: сколько ходов ещё выключено
	j["working"] = b.isUnitWorking();
	j["rubble"] = b.isRubble();
	j["storedRes"] = b.getStoredResources();
	if (b.getStaticData().canResearch)
		j["researchArea"] = researchAreaName (b.getResearchArea());
	if (!b.isBuildListEmpty())
	{
		nlohmann::json list = nlohmann::json::array();
		for (std::size_t i = 0; i < b.getBuildListSize(); ++i)
		{
			const auto& item = b.getBuildListItem (i);
			list.push_back ({{"type", typeNameById (model, item.getType())},
				{"typeId", item.getType().getText()},
				{"remainingMetal", item.getRemainingMetal()}});
		}
		j["buildList"] = std::move (list);
	}
	j["buildSpeed"] = b.getBuildSpeed();
	j["repeat"] = b.getRepeatBuild();
	j["prod"] = resJson (b.prod);
	j["maxProd"] = resJson (b.getMaxProd());
	addCargo (j, b);
	return j;
}

nlohmann::json vehicleJson (const cVehicle& v)
{
	nlohmann::json j;
	j["id"] = v.getId();
	j["type"] = unitTypeName (v);
	j["typeId"] = unitTypeIdText (v);
	j["owner"] = v.getOwner() ? v.getOwner()->getId() : -1;
	j["pos"] = posJson (v.getPosition());
	j["hp"] = v.data.getHitpoints();
	j["hpMax"] = v.data.getHitpointsMax();
	j["speed"] = v.data.getSpeed();
	j["speedMax"] = v.data.getSpeedMax();
	j["shots"] = v.data.getShots();
	j["shotsMax"] = v.data.getShotsMax();
	j["ammo"] = v.data.getAmmo();
	j["ammoMax"] = v.data.getAmmoMax();
	j["damage"] = v.data.getDamage();
	j["armor"] = v.data.getArmor();
	j["range"] = v.data.getRange();
	j["scan"] = v.data.getScan();
	j["disabled"] = v.isDisabled();
	if (v.isDisabled()) j["disabledTurns"] = v.getDisabledTurns(); // шаг 11: сколько ходов ещё выключена
	j["sentry"] = v.isSentryActive();
	if (v.isManualFireActive()) j["manualFire"] = true;
	j["attacking"] = v.isAttacking();
	j["beingAttacked"] = v.isBeingAttacked();
	j["moving"] = v.isUnitMoving();
	j["building"] = v.isUnitBuildingABuilding();
	j["storedRes"] = v.getStoredResources();
	j["storedUnits"] = v.storedUnits.size();
	if (v.isUnitLoaded()) j["loaded"] = true; // внутри транспорта или здания
	// шаг 11 (для шага 16): задание движения — куда едет и сколько клеток пути осталось; без поля — стоит
	if (const cMoveJob* job = v.getMoveJob(); job != nullptr && !job->isFinished() && !job->getPath().empty())
	{
		cPosition dest;
		int left = 0;
		for (const auto& p : job->getPath())
		{
			dest = p;
			++left;
		}
		j["moveDest"] = posJson (dest);
		j["pathLeft"] = left;
	}
	addCargo (j, v);
	return j;
}

// шаг 11: скрытность — скрыта ли машина на своей местности и кто из игроков её обнаружил
void addStealth (nlohmann::json& j, const cVehicle& v, const cModel& model)
{
	if (v.getStaticUnitData().isStealthOn == 0 || v.isUnitLoaded()) return;
	const auto& map = *model.getMap();
	const cPosition& p = v.getPosition();
	nlohmann::json detected = nlohmann::json::array();
	for (const auto& player : model.getPlayerList())
		if (player.get() != v.getOwner() && v.isDetectedByPlayer (player.get())) detected.push_back (player->getId());
	j["stealth"] = {{"hidden", v.isStealthOnCurrentTerrain (map.getField (p), map.staticMap->getTerrain (p))},
		{"detectedBy", std::move (detected)}};
}

// шаг 11: коммандос — удачи, звание и шанс выключить или захватить соседние видимые цели
// по формуле движка (cCommandoData::computeChance; цель «можно» — cCommandoData::canDoAction без
// учёта выстрела: выстрел есть — поле shots машины)
void addCommando (nlohmann::json& j, const cVehicle& v, const cModel& model, const cPlayer* viewpoint)
{
	const auto& sd = v.getStaticData();
	if (!sd.canDisable && !sd.canCapture) return;
	const auto& data = v.getCommandoData();
	nlohmann::json c;
	c["successes"] = data.getSuccessCount();
	c["rank"] = cCommandoData::getLevel (data.getSuccessCount());
	nlohmann::json targets = nlohmann::json::array();
	if (!v.isUnitLoaded())
	{
		const auto& map = *model.getMap();
		std::vector<const cUnit*> seen;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition p = v.getPosition() + cPosition (dx, dy);
				if ((dx == 0 && dy == 0) || !map.isValidPosition (p)) continue;
				for (const cUnit* u : map.getField (p).getUnits())
				{
					if (u == nullptr || u->getOwner() == v.getOwner()) continue;
					if (std::find (seen.begin(), seen.end(), u) != seen.end()) continue; // большое здание — одно
					seen.push_back (u);
					if (viewpoint != nullptr && !viewpoint->canSeeUnit (*u, map)) continue;
					if (u->isABuilding() && static_cast<const cBuilding*> (u)->isRubble()) continue;
					const auto& usd = u->getStaticUnitData();
					const bool flying = u->isAVehicle() && usd.factorAir > 0 && static_cast<const cVehicle*> (u)->getFlightHeight() > 0;
					const bool canDisable = sd.canDisable && usd.canBeDisabled && !u->isDisabled() && !flying;
					const bool canSteal = sd.canCapture && usd.canBeCaptured && u->storedUnits.empty() && !flying;
					if (!canDisable && !canSteal) continue;
					nlohmann::json t;
					t["id"] = u->getId();
					t["type"] = unitTypeName (*u);
					t["owner"] = u->getOwner() ? u->getOwner()->getId() : -1; // −1 — ничья (чужие): неудача без тревоги
					t["disable"] = canDisable ? nlohmann::json (data.computeChance (u, false)) : nlohmann::json();
					t["steal"] = canSteal ? nlohmann::json (data.computeChance (u, true)) : nlohmann::json();
					targets.push_back (std::move (t));
				}
			}
	}
	c["targets"] = std::move (targets);
	j["commando"] = std::move (c);
}

// машина в дайджесте: общие поля + поля шага 11 (скрытность, коммандос)
nlohmann::json vehicleFull (const cVehicle& v, const cModel& model, const cPlayer* viewpoint)
{
	nlohmann::json j = vehicleJson (v);
	addStealth (j, v, model);
	addCommando (j, v, model, viewpoint);
	return j;
}

} // namespace

nlohmann::json buildGameStateDigest (const cModel& model, int viewpointPlayerId)
{
	nlohmann::json j;

	j["gameId"] = model.getGameId();
	j["turn"] = model.getTurnCounter()->getTurn();
	j["gameTime"] = model.getGameTime();
	j["viewpoint"] = viewpointPlayerId;

	const auto& map = model.getMap();
	j["mapSize"] = posJson (map->getSize());
	// шаг 11 (для шага 16): файл карты и его контрольная сумма — местность берётся из .wrl
	j["map"] = utf8::to_string (map->getFilename().filename());
	j["mapChecksum"] = map->staticMap->getChecksum (0);

	const cPlayer* viewpoint = (viewpointPlayerId >= 0) ? model.getPlayer (viewpointPlayerId) : nullptr;

	// --- игроки ---
	nlohmann::json players = nlohmann::json::array();
	for (const auto& player : model.getPlayerList())
	{
		nlohmann::json p;
		p["id"] = player->getId();
		p["name"] = player->getName();
		p["clan"] = player->getClan();
		p["credits"] = player->getCredits();
		p["score"] = player->getScore();
		p["defeated"] = player->isDefeated;
		p["finishedTurn"] = player->getHasFinishedTurn();
		// шаг 11: кредиты, исследования и подбазы чужого игрока — сверх тумана войны: только своему
		// (очки — общие, их видят все игроки)
		if (viewpoint != nullptr && player.get() != viewpoint)
		{
			p.erase ("credits");
			players.push_back (std::move (p));
			continue;
		}

		nlohmann::json research;
		for (int a = 0; a < static_cast<int> (cResearch::kNrResearchAreas); ++a)
		{
			const auto area = static_cast<cResearch::eResearchArea> (a);
			research[researchAreaName (area)] = player->getResearchState().getCurResearchLevel (area);
		}
		p["research"] = std::move (research);
		p["researchCentersTotal"] = player->getResearchCentersWorkingTotal();

		// база: суббазы (ресурсы/энергия/люди)
		nlohmann::json subbases = nlohmann::json::array();
		for (const auto& sb : player->base.SubBases)
		{
			subbases.push_back ({
				{"stored", resJson (sb->getResourcesStored())},
				{"maxStored", resJson (sb->getMaxResourcesStored())},
				{"needed", resJson (sb->getResourcesNeeded())},
				{"prod", resJson (sb->getProd())},
				{"maxProd", resJson (sb->getMaxProd())},
				{"energyProd", sb->getEnergyProd()},
				{"energyNeed", sb->getEnergyNeed()},
				{"maxEnergyProd", sb->getMaxEnergyProd()},
				{"humanProd", sb->getHumanProd()},
				{"humanNeed", sb->getHumanNeed()},
			});
		}
		p["subbases"] = std::move (subbases);
		players.push_back (std::move (p));
	}
	j["players"] = std::move (players);

	// --- юниты/здания (с фильтром тумана войны для viewpoint) ---
	nlohmann::json units = nlohmann::json::array();
	nlohmann::json buildings = nlohmann::json::array();
	for (const auto& player : model.getPlayerList())
	{
		const bool isViewpoint = (viewpoint && player.get() == viewpoint);

		for (const auto& v : player->getVehicles())
		{
			bool visible = isViewpoint || (viewpoint == nullptr);
			if (!visible)
				visible = viewpoint->canSeeUnit (*v, *map);
			if (!visible) continue;
			units.push_back (vehicleFull (*v, model, viewpoint));
		}
		for (const auto& b : player->getBuildings())
		{
			bool visible = isViewpoint || (viewpoint == nullptr);
			if (!visible)
				visible = viewpoint->canSeeUnit (*b, *map);
			if (!visible) continue;
			buildings.push_back (buildingJson (*b, model));
		}
	}
	// шаг 11: ничейные юниты (чужие, owner −1), видимые игроку: их выключают и захватывают без тревоги
	for (const auto& v : model.getNeutralVehicles())
		if (viewpoint == nullptr || viewpoint->canSeeUnit (*v, *map)) units.push_back (vehicleFull (*v, model, viewpoint));
	for (const auto& b : model.getNeutralBuildings()) // кроме обломков: они тоже ничьи, но не техника чужих
		if (!b->isRubble() && (viewpoint == nullptr || viewpoint->canSeeUnit (*b, *map))) buildings.push_back (buildingJson (*b, model));
	j["units"] = std::move (units);
	j["buildings"] = std::move (buildings);

	// --- разведанные ресурсы viewpoint-игрока ---
	if (viewpoint)
	{
		nlohmann::json resources = nlohmann::json::array();
		const cPosition size = map->getSize();
		for (int x = 0; x < size.x(); ++x)
		{
			for (int y = 0; y < size.y(); ++y)
			{
				const cPosition pos (x, y);
				if (!viewpoint->hasResourceExplored (pos)) continue;
				const auto& r = map->getResource (pos);
				if (r.typ == eResourceType::None) continue;
				resources.push_back ({
					{"x", x}, {"y", y}, {"type", resTypeName (r.typ)}, {"value", r.value}});
			}
		}
		j["resources"] = std::move (resources);
	}

	return j;
}

nlohmann::json buildVisibility (const cModel& model, int playerId)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr) return {};
	const cPosition size = model.getMap()->getSize();
	// по строкам — отрезки [y, x, длина] клеток, где признак истинен
	const auto runs = [&] (auto has) {
		nlohmann::json out = nlohmann::json::array();
		for (int y = 0; y < size.y(); ++y)
		{
			int start = -1;
			for (int x = 0; x <= size.x(); ++x)
			{
				const bool on = x < size.x() && has (cPosition (x, y));
				if (on && start < 0) start = x;
				if (!on && start >= 0)
				{
					out.push_back ({y, start, x - start});
					start = -1;
				}
			}
		}
		return out;
	};
	return {{"scan", runs ([&] (const cPosition& p) { return player->canSeeAt (p); })},
		{"detectLand", runs ([&] (const cPosition& p) { return player->hasLandDetection (p); })},
		{"detectSea", runs ([&] (const cPosition& p) { return player->hasSeaDetection (p); })}};
}

nlohmann::json buildPassDigest (const cModel& model, int viewpointPlayerId, int pass)
{
	nlohmann::json j;
	j["turn"] = model.getTurnCounter()->getTurn();
	j["pass"] = pass;
	j["gameTime"] = model.getGameTime();
	j["viewpoint"] = viewpointPlayerId;
	const auto& map = model.getMap();
	const cPlayer* viewpoint = (viewpointPlayerId >= 0) ? model.getPlayer (viewpointPlayerId) : nullptr;
	nlohmann::json units = nlohmann::json::array();
	nlohmann::json buildings = nlohmann::json::array();
	const auto add = [&] (const cPlayer* owner, const cVehicle* v, const cBuilding* b) {
		const cUnit& u = v ? static_cast<const cUnit&> (*v) : static_cast<const cUnit&> (*b);
		const bool own = viewpoint != nullptr && owner == viewpoint;
		if (!own && viewpoint != nullptr && !viewpoint->canSeeUnit (u, *map)) return;
		if (v)
			units.push_back (vehicleFull (*v, model, viewpoint));
		// свои здания — только повреждённые или выключенные: остальные с первого захода не менялись
		else if (!own || b->data.getHitpoints() < b->data.getHitpointsMax() || b->isDisabled())
			buildings.push_back (buildingJson (*b, model));
	};
	for (const auto& player : model.getPlayerList())
	{
		for (const auto& v : player->getVehicles()) add (player.get(), v.get(), nullptr);
		for (const auto& b : player->getBuildings()) add (player.get(), nullptr, b.get());
	}
	for (const auto& v : model.getNeutralVehicles()) add (nullptr, v.get(), nullptr);
	for (const auto& b : model.getNeutralBuildings())
		if (!b->isRubble()) add (nullptr, nullptr, b.get());
	j["units"] = std::move (units);
	j["buildings"] = std::move (buildings);
	return j;
}

} // namespace ai
