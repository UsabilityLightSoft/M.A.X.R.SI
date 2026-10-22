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
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/turncounter.h"
#include "game/logic/upgradecalculator.h"

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
	j["sentry"] = v.isSentryActive();
	j["attacking"] = v.isAttacking();
	j["beingAttacked"] = v.isBeingAttacked();
	j["moving"] = v.isUnitMoving();
	j["building"] = v.isUnitBuildingABuilding();
	j["storedRes"] = v.getStoredResources();
	j["storedUnits"] = v.storedUnits.size();
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
			units.push_back (vehicleJson (*v));
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

} // namespace ai
