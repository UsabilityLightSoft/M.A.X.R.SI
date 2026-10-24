/***************************************************************************
 * ai_order_validator.cpp — предпроверка легальности приказов.
 ***************************************************************************/

#include "ai_order_validator.h"

#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/upgradecalculator.h"

#include <algorithm>

namespace ai
{

namespace
{

sValidationResult fail (sValidationResult& r, std::string why)
{
	r.ok = false;
	r.reason = std::move (why);
	return r;
}

const cPlayer* playerOf (const cModel& model, const cOrder& o)
{
	return model.getPlayer (o.playerId);
}

// вернуть shared_ptr<const cPlayer> для cMapView (туман войны)
std::shared_ptr<const cPlayer> sharedPlayer (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id)
			return p;
	return nullptr;
}

} // namespace

sValidationResult validateOrder (const cModel& model, const cOrder& o)
{
	sValidationResult r;
	const auto& map = model.getMap();
	auto* player = playerOf (model, o);

	if (o.type == eOrderType::EndTurn)
	{
		if (player == nullptr) return fail (r, "EndTurn: нет такого игрока");
		if (player->isDefeated) return fail (r, "EndTurn: игрок побеждён");
		if (player->getHasFinishedTurn()) return fail (r, "EndTurn: ход уже завершён");
		return r;
	}

	if (o.type == eOrderType::ChangeResearch)
	{
		if (player == nullptr) return fail (r, "ChangeResearch: нет такого игрока");
		int sum = 0;
		for (int n : o.researchAreas)
		{
			if (n < 0) return fail (r, "ChangeResearch: отрицательное число центров");
			sum += n;
		}
		if (sum > player->getResearchCentersWorkingTotal())
			return fail (r, "ChangeResearch: больше центров, чем работающих");
		return r;
	}

	// --- приказы, адресованные юниту ---
	cUnit* unit = model.getUnitFromID (o.unitId);
	if (unit == nullptr) return fail (r, std::string (toString (o.type)) + ": юнит не найден");
	if (unit->getOwner() == nullptr || unit->getOwner()->getId() != o.playerId)
		return fail (r, std::string (toString (o.type)) + ": юнит не принадлежит игроку");
	if (unit->isDisabled() && o.type != eOrderType::StopWork)
		return fail (r, std::string (toString (o.type)) + ": юнит выведен из строя");

	switch (o.type)
	{
		case eOrderType::Move:
		{
			auto* v = dynamic_cast<cVehicle*> (unit);
			if (v == nullptr) return fail (r, "Move: юнит — не техника");
			if (o.path.empty()) return fail (r, "Move: пустой путь");
			if (v->isUnitMoving()) return fail (r, "Move: уже движется");
			for (const auto& p : o.path)
				if (!map->isValidPosition (p))
					return fail (r, "Move: клетка вне карты");
			return r;
		}
		case eOrderType::Attack:
		{
			if (!map->isValidPosition (o.position))
				return fail (r, "Attack: клетка вне карты");
			cMapView view (map, sharedPlayer (model, o.playerId));
			if (!unit->canAttackObjectAt (o.position, view, false))
				return fail (r, "Attack: цель вне досягаемости");
			return r;
		}
		case eOrderType::Build:
		case eOrderType::BuildPath:
		{
			auto* v = dynamic_cast<cVehicle*> (unit);
			if (v == nullptr) return fail (r, "Build: конструктор — не техника");
			if (!map->isValidPosition (o.position))
				return fail (r, "Build: клетка вне карты");
			const sID bid (o.buildingType.firstPart, o.buildingType.secondPart);
			if (!model.getUnitsData()->isValidId (bid))
				return fail (r, "Build: неизвестный тип здания");
			const auto& bdata = model.getUnitsData()->getStaticUnitData (bid);
			if (!bdata.ID.isABuilding())
				return fail (r, "Build: тип — не здание");
			// правила так не приказывают; для LLM — понятная причина вместо молчаливого отказа сервера
			if (v->getStaticUnitData().canBuild != bdata.buildAs)
				return fail (r, "Build: этот юнит не строит здания такого класса (" + bdata.buildAs + ")");
			if (v->isUnitBuildingABuilding())
				return fail (r, "Build: конструктор уже строит");
			return r;
		}
		case eOrderType::ChangeBuildList:
		{
			auto* b = dynamic_cast<cBuilding*> (unit);
			if (b == nullptr) return fail (r, "ChangeBuildList: юнит — не здание");
			for (const auto& t : o.buildList)
			{
				const sID id (t.firstPart, t.secondPart);
				if (!model.getUnitsData()->isValidId (id))
					return fail (r, "ChangeBuildList: неизвестный тип в очереди");
			}
			return r;
		}
		case eOrderType::Repair:
		case eOrderType::Rearm:
		{
			cUnit* target = model.getUnitFromID (o.unitId2);
			if (target == nullptr) return fail (r, "Repair/Rearm: цель не найдена");
			if (target->getOwner() == nullptr || target->getOwner()->getId() != o.playerId)
				return fail (r, "Repair/Rearm: цель не своя");
			return r;
		}
		case eOrderType::Transfer:
		{
			if (o.transferValue <= 0) return fail (r, "Transfer: нечего передавать");
			cUnit* target = model.getUnitFromID (o.unitId2);
			if (target == nullptr) return fail (r, "Transfer: цель не найдена");
			if (target->getOwner() == nullptr || target->getOwner()->getId() != o.playerId)
				return fail (r, "Transfer: цель не своя");
			return r;
		}
		case eOrderType::StartWork:
		{
			auto* b = dynamic_cast<cBuilding*> (unit);
			if (b == nullptr) return fail (r, "StartWork: юнит — не здание");
			if (!b->getStaticData().canWork) return fail (r, "StartWork: здание не запускается (нет рабочего цикла)");
			if (b->isUnitWorking()) return fail (r, "StartWork: здание уже работает");
			if (!b->buildingCanBeStarted()) return fail (r, "StartWork: у фабрики нет недостроенного задания");
			return r;
		}
		case eOrderType::StopWork:
		case eOrderType::ToggleSentry:
		case eOrderType::ToggleManualFire:
		case eOrderType::ToggleLayMines:
		case eOrderType::ToggleCollectMines:
		case eOrderType::SelfDestroy:
		case eOrderType::FinishBuild:
			return r; // тривиальные приказы на свой юнит — допустимы
		case eOrderType::Load:
		{
			cUnit* target = model.getUnitFromID (o.unitId2);
			if (target == nullptr) return fail (r, "Load: грузимый юнит не найден");
			if (target->getOwner() == nullptr || target->getOwner()->getId() != o.playerId)
				return fail (r, "Load: грузимый юнит не свой");
			return r;
		}
		case eOrderType::ActivateUnit:
		{
			if (!map->isValidPosition (o.position))
				return fail (r, "ActivateUnit: клетка вне карты");
			return r;
		}
		default:
			return fail (r, "неизвестный тип приказа");
	}
}

} // namespace ai
