/***************************************************************************
 * ai_order_validator.cpp — предпроверка легальности приказов.
 ***************************************************************************/

#include "ai_order_validator.h"

#include "ai_types.h"
#include "ai_upgrades.h"

#include "game/data/base/base.h"
#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/commandodata.h"
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

	// шаг 10: улучшения типа за кредиты — приказ игрока, не юнита
	if (o.type == eOrderType::BuyUpgrades)
	{
		if (player == nullptr) return fail (r, "BuyUpgrades: нет такого игрока");
		if (player->isDefeated) return fail (r, "BuyUpgrades: игрок побеждён");
		const sID id (o.unitType.firstPart, o.unitType.secondPart);
		cUnitUpgrade upgrade;
		int cost = 0;
		std::string why;
		if (!makeUnitUpgrade (model, *player, id, o.upgradeSteps, upgrade, cost, &why))
			return fail (r, "BuyUpgrades: " + why);
		if (cost > player->getCredits())
			return fail (r, "BuyUpgrades: не хватает кредитов (нужно " + std::to_string (cost) + ", есть "
				+ std::to_string (player->getCredits()) + ")");
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
			// шаг 11: погрузить в конце пути или войти — цель своя
			if (o.moveEnd != eMoveEnd::Attack)
			{
				const cUnit* target = model.getUnitFromID (o.unitId2);
				if (target == nullptr) return fail (r, "Move: цель погрузки не найдена");
				if (target->getOwner() != unit->getOwner()) return fail (r, "Move: цель погрузки не своя");
				if (o.moveEnd == eMoveEnd::Load && v->getStaticUnitData().storageUnitsMax <= 0)
					return fail (r, "Move: машина не транспорт — грузить некуда");
				if (o.moveEnd == eMoveEnd::GetIn && target->getStaticUnitData().storageUnitsMax <= 0)
					return fail (r, "Move: цель — не транспорт и не хранилище");
			}
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
			// Р6 (правка 22, PLAN.md §7 п. 18): только соседу, как проверяет сервер (cActionTransfer):
			// здание — через подбазу (её здание рядом с машиной, два здания — одна подбаза), машина — вплотную
			if (target == unit) return fail (r, "Transfer: сам себе");
			{
				const auto* fromBuilding = dynamic_cast<const cBuilding*> (unit);
				const auto* toBuilding = dynamic_cast<const cBuilding*> (target);
				if ((fromBuilding && !fromBuilding->subBase) || (toBuilding && !toBuilding->subBase))
					return fail (r, "Transfer: здание без подбазы");
				bool near = false;
				if (fromBuilding && toBuilding)
					near = fromBuilding->subBase == toBuilding->subBase;
				else if (fromBuilding)
					near = std::any_of (fromBuilding->subBase->getBuildings().begin(), fromBuilding->subBase->getBuildings().end(),
						[&] (const auto& b) { return b->isNextTo (target->getPosition()); });
				else
					near = target->isNextTo (unit->getPosition());
				if (!near) return fail (r, "Transfer: цель не рядом (нужно: соседняя клетка или здание той же подбазы рядом)");
			}
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
			// шаг 11: та же проверка, что у сервера (cActionLoad) — с понятной причиной
			auto* loaded = dynamic_cast<cVehicle*> (target);
			if (loaded == nullptr) return fail (r, "Load: грузить можно только машину");
			if (!unit->canLoad (loaded))
				return fail (r, "Load: не грузится (нужно: рядом, авиатранспорт — над машиной; место; тип груза; машина стоит и не под огнём)");
			return r;
		}
		case eOrderType::ActivateUnit:
		{
			if (!map->isValidPosition (o.position))
				return fail (r, "ActivateUnit: клетка вне карты");
			// шаг 11: машина внутри и может выйти на клетку (как cActionActivate)
			const cUnit* stored = model.getUnitFromID (o.unitId2);
			if (stored == nullptr || std::find (unit->storedUnits.begin(), unit->storedUnits.end(), stored) == unit->storedUnits.end())
				return fail (r, "ActivateUnit: машины нет внутри");
			if (!unit->isNextTo (o.position) && unit->getPosition() != o.position)
				return fail (r, "ActivateUnit: клетка не рядом с хранилищем");
			// по видимому игроку (скрытую чужую машину на клетке сервер отодвинет сам)
			const cMapView view (map, sharedPlayer (model, o.playerId));
			if (!unit->canExitTo (o.position, view, stored->getStaticUnitData()))
				return fail (r, "ActivateUnit: на клетку не выйти (занята или не та местность)");
			return r;
		}
		// --- шаг 10 ---
		case eOrderType::UpgradeVehicle:
		{
			auto* b = dynamic_cast<cBuilding*> (unit);
			if (b == nullptr) return fail (r, "UpgradeVehicle: хранилище — не здание");
			if (b->getStaticUnitData().storageUnitsMax <= 0) return fail (r, "UpgradeVehicle: здание не хранит машин");
			if (b->subBase == nullptr) return fail (r, "UpgradeVehicle: здание без подбазы (нет металла)");
			const int metal = b->subBase->getResourcesStored().metal;
			int cheapest = -1;
			for (const cVehicle* v : b->storedUnits)
			{
				if (o.unitId2 != kNoUnit && v->getId() != o.unitId2) continue; // шаг 13: kNoUnit — все устаревшие
				if (!outdated (*v)) continue;
				const int c = refitMetalCost (*unit->getOwner(), v->data.getId());
				if (cheapest < 0 || c < cheapest) cheapest = c;
			}
			if (cheapest < 0)
				return fail (r, o.unitId2 != kNoUnit ? "UpgradeVehicle: машины нет в хранилище или она уже текущей версии"
												: "UpgradeVehicle: в хранилище нет устаревших машин");
			if (cheapest > metal)
				return fail (r, "UpgradeVehicle: не хватает металла (нужно " + std::to_string (cheapest) + ", есть " + std::to_string (metal) + ")");
			return r;
		}
		case eOrderType::UpgradeBuilding:
		{
			auto* b = dynamic_cast<cBuilding*> (unit);
			if (b == nullptr) return fail (r, "UpgradeBuilding: юнит — не здание");
			if (!outdated (*b)) return fail (r, "UpgradeBuilding: здание уже текущей версии");
			if (b->subBase == nullptr) return fail (r, "UpgradeBuilding: здание без подбазы (нет металла)");
			const int cost = refitMetalCost (*unit->getOwner(), b->data.getId());
			if (cost > b->subBase->getResourcesStored().metal)
				return fail (r, "UpgradeBuilding: не хватает металла (нужно " + std::to_string (cost) + ")");
			return r;
		}
		case eOrderType::SetMining:
		{
			auto* b = dynamic_cast<cBuilding*> (unit);
			if (b == nullptr) return fail (r, "SetMining: юнит — не здание");
			if (b->subBase == nullptr) return fail (r, "SetMining: здание без подбазы");
			for (int n : o.mining)
				if (n < 0) return fail (r, "SetMining: отрицательная добыча");
			return r;
		}
		// --- шаг 11: коммандос выключает или захватывает соседнюю цель (cActionStealDisable) ---
		case eOrderType::Disable:
		case eOrderType::Steal:
		{
			const bool steal = o.type == eOrderType::Steal;
			const char* what = steal ? "Steal" : "Disable";
			auto* commando = dynamic_cast<cVehicle*> (unit);
			if (commando == nullptr) return fail (r, std::string (what) + ": юнит — не машина");
			if (steal ? !commando->getStaticData().canCapture : !commando->getStaticData().canDisable)
				return fail (r, std::string (what) + ": машина этого не умеет (только коммандос)");
			const cUnit* target = model.getUnitFromID (o.unitId2);
			if (target == nullptr) return fail (r, std::string (what) + ": цель не найдена");
			const auto viewer = sharedPlayer (model, o.playerId);
			if (viewer == nullptr || !viewer->canSeeUnit (*target, *map)) return fail (r, std::string (what) + ": цель не видна");
			if (!cCommandoData::canDoAction (*commando, target, steal))
				return fail (r, std::string (what) + (steal
					? ": нельзя (нужно: соседняя клетка, выстрел, цель — чужая машина без груза, не в воздухе)"
					: ": нельзя (нужно: соседняя клетка, выстрел, цель чужая, не выключена, не в воздухе)"));
			return r;
		}
		default:
			return fail (r, "неизвестный тип приказа");
	}
}

} // namespace ai
