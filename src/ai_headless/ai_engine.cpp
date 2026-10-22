/***************************************************************************
 * ai_engine.cpp — оркестрация ИИ (PLAN_AI этап 2).
 ***************************************************************************/

#include "ai_engine.h"

#include "ai_game_state.h"
#include "ai_order_validator.h"
#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/id.h"
#include "game/data/units/unit.h"
#include "game/data/units/vehicle.h"
#include "game/logic/client.h"
#include "game/logic/endmoveaction.h"
#include "game/logic/movejob.h"
#include "game/logic/turncounter.h"
#include "game/logic/upgradecalculator.h"
#include "utility/log.h"

#include <filesystem>
#include <forward_list>

namespace ai
{

namespace
{

nlohmann::json orderToJson (const cOrder& o)
{
	nlohmann::json j;
	j["type"] = toString (o.type);
	j["player"] = o.playerId;
	j["level"] = o.level;
	j["reason"] = o.reason;
	j["unitId"] = o.unitId;
	if (o.unitId2) j["unitId2"] = o.unitId2;
	if (o.type == eOrderType::Move || o.type == eOrderType::Attack || o.type == eOrderType::Build
		|| o.type == eOrderType::BuildPath || o.type == eOrderType::ActivateUnit)
	{
		j["pos"] = {{"x", o.position.x()}, {"y", o.position.y()}};
	}
	if (o.type == eOrderType::Build || o.type == eOrderType::BuildPath)
		j["buildingType"] = o.buildingType.firstPart + "." + std::to_string (o.buildingType.secondPart);
	if (o.type == eOrderType::Move)
	{
		nlohmann::json path = nlohmann::json::array();
		for (const auto& p : o.path) path.push_back ({{"x", p.x()}, {"y", p.y()}});
		j["path"] = std::move (path);
	}
	if (o.type == eOrderType::Transfer)
	{
		j["transferValue"] = o.transferValue;
		j["resType"] = static_cast<int> (o.resType);
	}
	return j;
}

} // namespace

cAiEngine::cAiEngine (std::string outDir) :
	outDir_ (std::move (outDir)),
	journal_ (outDir_ + "/journal.jsonl")
{
	std::filesystem::create_directories (outDir_ + "/digest");
	if (!journal_.open())
		Log.error ("ai_engine: cannot open journal " + journal_.getPath());
}

cAiEngine::~cAiEngine()
{
	journal_.close();
}

void cAiEngine::addPlayer (int playerId, cClient* client, std::shared_ptr<cAiBrain> brain)
{
	players_.push_back ({playerId, client, std::move (brain), -1, false});
}

void cAiEngine::run()
{
	for (auto& ctx : players_)
	{
		const cModel& model = ctx.client->getModel();
		const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
		if (turn != ctx.lastTurn)
		{
			ctx.lastTurn = turn;
			ctx.acted = false;
		}

		const cPlayer* active = model.getActiveTurnPlayer();
		if (active == nullptr) continue;
		if (active->getId() != ctx.playerId) continue;
		// activeTurnPlayer выставляется оптимистично в setPlayerList ещё до
		// высадки (initNewGame); landingPos ставится только при её применении.
		// Ждём, пока модель реально получила высадку — иначе дайджест пустой.
		if (active->getLandingPos() == cPosition (-1, -1)) continue;
		if (active->getHasFinishedTurn()) continue;
		if (ctx.acted) continue;

		act (ctx);
		ctx.acted = true;
	}
}

void cAiEngine::act (sCtx& ctx)
{
	const cModel& model = ctx.client->getModel();
	const int turn = model.getTurnCounter()->getTurn();

	// дайджест состояния (точка зрения игрока)
	const auto digest = buildGameStateDigest (model, ctx.playerId);
	const std::string digestPath = outDir_ + "/digest/turn" + std::to_string (turn)
		+ "_p" + std::to_string (ctx.playerId) + ".json";
	std::ofstream digestFile (digestPath);
	if (digestFile.is_open())
	{
		digestFile << digest.dump (2) << "\n";
		digestFile.close();
	}
	else
		Log.error ("ai_engine: cannot write digest " + digestPath);

	journal_.log ({{"event", "turn_start"}, {"player", ctx.playerId}, {"turn", turn},
		{"brain", ctx.brain->name()}});

	// решение мозга
	const auto orders = ctx.brain->decideTurn (ctx.playerId, model);

	// валидация + исполнение
	for (const auto& order : orders)
	{
		++orderCounter_;
		const auto v = validateOrder (model, order);
		nlohmann::json entry = orderToJson (order);
		entry["seq"] = orderCounter_;
		entry["turn"] = turn;
		if (v.ok)
		{
			executeOrder (*ctx.client, order);
			entry["event"] = "order";
			journal_.log (std::move (entry));
		}
		else
		{
			entry["event"] = "rejected";
			entry["reason"] = v.reason;
			journal_.log (std::move (entry));
			Log.info ("ai_engine: rejected " + std::string (toString (order.type)) + ": " + v.reason);
		}
	}

	ctx.client->endTurn();
	journal_.log ({{"event", "turn_end"}, {"player", ctx.playerId}, {"turn", turn},
		{"orders", orders.size()}});
}

void cAiEngine::executeOrder (cClient& client, const cOrder& o)
{
	const cModel& model = client.getModel();

	switch (o.type)
	{
		case eOrderType::EndTurn:
			client.endTurn();
			return;

		case eOrderType::Move:
		{
			auto* v = model.getVehicleFromID (o.unitId);
			if (v == nullptr) return;
			std::forward_list<cPosition> path;
			for (auto it = o.path.rbegin(); it != o.path.rend(); ++it)
				path.push_front (*it);
			client.startMove (*v, path, eStart::Immediate, eStopOn::Never, cEndMoveAction::None());
			return;
		}

		case eOrderType::Attack:
		{
			auto* u = model.getUnitFromID (o.unitId);
			if (u == nullptr) return;
			const auto& field = model.getMap()->getField (o.position);
			const cUnit* target = field.getTopBuilding();
			if (target == nullptr) target = field.getVehicle();
			client.attack (*u, o.position, target);
			return;
		}

		case eOrderType::Build:
		case eOrderType::BuildPath:
		{
			auto* v = model.getVehicleFromID (o.unitId);
			if (v == nullptr) return;
			const sID bid (o.buildingType.firstPart, o.buildingType.secondPart);
			if (o.type == eOrderType::BuildPath)
				client.startBuildPath (*v, bid, o.buildSpeed, o.position, o.position2);
			else
				client.startBuild (*v, bid, o.buildSpeed, o.position);
			return;
		}

		case eOrderType::ChangeBuildList:
		{
			auto* b = model.getBuildingFromID (o.unitId);
			if (b == nullptr) return;
			std::vector<sID> list;
			list.reserve (o.buildList.size());
			for (const auto& t : o.buildList)
				list.emplace_back (t.firstPart, t.secondPart);
			client.changeBuildList (*b, list, o.buildSpeed, o.repeat);
			return;
		}

		case eOrderType::ChangeResearch:
		{
			std::array<int, cResearch::kNrResearchAreas> areas{};
			for (std::size_t i = 0; i < areas.size() && i < o.researchAreas.size(); ++i)
				areas[i] = o.researchAreas[i];
			client.changeResearch (areas);
			return;
		}

		case eOrderType::Repair:
		case eOrderType::Rearm:
		{
			auto* src = model.getUnitFromID (o.unitId);
			auto* dst = model.getUnitFromID (o.unitId2);
			if (src == nullptr || dst == nullptr) return;
			if (o.type == eOrderType::Repair) client.repair (*src, *dst);
			else client.rearm (*src, *dst);
			return;
		}

		case eOrderType::Transfer:
		{
			auto* src = model.getUnitFromID (o.unitId);
			auto* dst = model.getUnitFromID (o.unitId2);
			if (src == nullptr || dst == nullptr) return;
			client.transfer (*src, *dst, o.transferValue, o.resType);
			return;
		}

		case eOrderType::StartWork:
		{
			auto* b = model.getBuildingFromID (o.unitId);
			if (b != nullptr) client.startWork (*b);
			return;
		}

		case eOrderType::StopWork:
		{
			auto* u = model.getUnitFromID (o.unitId);
			if (u != nullptr) client.stopWork (*u);
			return;
		}

		case eOrderType::ToggleSentry:
		{
			auto* u = model.getUnitFromID (o.unitId);
			if (u != nullptr) client.changeSentry (*u);
			return;
		}

		case eOrderType::ToggleManualFire:
		{
			auto* u = model.getUnitFromID (o.unitId);
			if (u != nullptr) client.changeManualFire (*u);
			return;
		}

		case eOrderType::ToggleLayMines:
		{
			auto* v = model.getVehicleFromID (o.unitId);
			if (v != nullptr) client.toggleLayMines (*v);
			return;
		}

		case eOrderType::ToggleCollectMines:
		{
			auto* v = model.getVehicleFromID (o.unitId);
			if (v != nullptr) client.toggleCollectMines (*v);
			return;
		}

		case eOrderType::SelfDestroy:
		{
			auto* b = model.getBuildingFromID (o.unitId);
			if (b != nullptr) client.selfDestroy (*b);
			return;
		}

		case eOrderType::Load:
		{
			auto* loader = model.getUnitFromID (o.unitId);
			auto* loaded = model.getVehicleFromID (o.unitId2);
			if (loader != nullptr && loaded != nullptr) client.load (*loader, *loaded);
			return;
		}

		case eOrderType::ActivateUnit:
		{
			auto* container = model.getUnitFromID (o.unitId);
			auto* vehicle = model.getVehicleFromID (o.unitId2);
			if (container != nullptr && vehicle != nullptr)
				client.activateUnit (*container, *vehicle, o.position);
			return;
		}

		case eOrderType::FinishBuild:
		{
			auto* unit = model.getUnitFromID (o.unitId);
			if (unit != nullptr) client.finishBuild (*unit, o.position);
			return;
		}

		default:
			return;
	}
}

void cAiEngine::writeMetrics()
{
	if (players_.empty()) return;

	// берём финальную модель первого клиента (все клиенты синхронны).
	const cModel& model = players_.front().client->getModel();

	nlohmann::json m;
	m["turn"] = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	m["gameTime"] = model.getGameTime();

	nlohmann::json players = nlohmann::json::array();
	for (const auto& player : model.getPlayerList())
	{
		const auto& s = player->getGameOverStat();
		nlohmann::json p;
		p["id"] = player->getId();
		p["name"] = player->getName();
		p["clan"] = player->getClan();
		p["defeated"] = player->isDefeated;
		p["credits"] = player->getCredits();
		p["score"] = player->getScore();
		p["units"] = player->getVehicles().size() + player->getBuildings().size();
		p["builtFactories"] = s.builtFactoriesCount;
		p["builtMineStations"] = s.builtMineStationCount;
		p["builtBuildings"] = s.builtBuildingsCount;
		p["lostBuildings"] = s.lostBuildingsCount;
		p["builtVehicles"] = s.builtVehiclesCount;
		p["lostVehicles"] = s.lostVehiclesCount;
		p["totalUpgradeCost"] = s.totalUpgradeCost;
		players.push_back (std::move (p));
	}
	m["players"] = std::move (players);

	const std::string path = outDir_ + "/metrics.json";
	std::ofstream f (path);
	if (f.is_open())
	{
		f << m.dump (2) << "\n";
		f.close();
		Log.info ("ai_engine: metrics written to " + path);
	}
	else
		Log.error ("ai_engine: cannot write metrics " + path);
}

void cAiEngine::finalize()
{
	writeMetrics();
	journal_.close();
}

} // namespace ai
