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

#include <algorithm>
#include <filesystem>
#include <forward_list>
#include <initializer_list>

namespace ai
{

namespace
{

std::string typeIdText (const sTypeId& t)
{
	return std::to_string (t.firstPart) + "." + std::to_string (t.secondPart);
}

// Приказ целиком: поля, нужные его типу, всегда; прочие — если не по умолчанию.
// По журналу ход можно восстановить без мозга (в т. ч. очереди фабрик и исследования).
nlohmann::json orderToJson (const cOrder& o)
{
	const auto is = [&o] (std::initializer_list<eOrderType> types) {
		return std::find (types.begin(), types.end(), o.type) != types.end();
	};
	nlohmann::json j;
	j["type"] = toString (o.type);
	j["player"] = o.playerId;
	j["level"] = o.level;
	j["reason"] = o.reason;
	j["unitId"] = o.unitId;
	if (o.unitId2) j["unitId2"] = o.unitId2;
	if (is ({eOrderType::Move, eOrderType::Attack, eOrderType::Build, eOrderType::BuildPath, eOrderType::ActivateUnit, eOrderType::FinishBuild})
		|| o.position != cPosition (0, 0))
	{
		j["pos"] = {{"x", o.position.x()}, {"y", o.position.y()}};
	}
	if (o.type == eOrderType::BuildPath || o.position2 != cPosition (0, 0))
		j["pos2"] = {{"x", o.position2.x()}, {"y", o.position2.y()}};
	if (is ({eOrderType::Build, eOrderType::BuildPath}) || o.buildingType != sTypeId())
		j["buildingType"] = typeIdText (o.buildingType);
	if (o.type == eOrderType::Move || !o.path.empty())
	{
		nlohmann::json path = nlohmann::json::array();
		for (const auto& p : o.path) path.push_back ({{"x", p.x()}, {"y", p.y()}});
		j["path"] = std::move (path);
	}
	if (o.type == eOrderType::ChangeBuildList || !o.buildList.empty())
	{
		nlohmann::json list = nlohmann::json::array();
		for (const auto& t : o.buildList) list.push_back (typeIdText (t));
		j["buildList"] = std::move (list);
	}
	if (is ({eOrderType::Build, eOrderType::BuildPath, eOrderType::ChangeBuildList}) || o.buildSpeed != 0)
		j["buildSpeed"] = o.buildSpeed;
	if (o.type == eOrderType::ChangeBuildList || o.repeat)
		j["repeat"] = o.repeat;
	if (o.type == eOrderType::ChangeResearch || std::any_of (o.researchAreas.begin(), o.researchAreas.end(), [] (int n) { return n != 0; }))
		j["researchAreas"] = o.researchAreas; // центров на область, порядок cResearch::eResearchArea
	if (o.type == eOrderType::Transfer || o.transferValue != 0 || o.resType != eResourceType::None)
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
	const std::size_t index = players_.size();
	players_.push_back ({playerId, client, std::move (brain)});
	if (index == 0) metrics_.attach (client->getModel()); // все модели синхронны — хватит одной

	// cActionEndTurn не сдаёт ход, если base.checkTurnEnd() что-то выключил
	// (нехватка энергии/топлива/металла/золота/людей) — как в GUI, где игрок
	// жмёт «конец хода» ещё раз. Отказ виден по сигналам нехватки базы в нашей
	// клиентской модели: она исполняет тот же EndTurn, что и сервер.
	const cPlayer* player = client->getModel().getPlayer (playerId);
	if (player == nullptr) return;
	const auto onRefused = [this, index]() {
		auto& ctx = players_[index];
		if (ctx.endTurnSent) ctx.endTurnRefused = true;
	};
	signalConnections_.connect (player->base.metalLow, onRefused);
	signalConnections_.connect (player->base.teamLow, onRefused);
	signalConnections_.connect (player->base.goldLow, onRefused);
	signalConnections_.connect (player->base.fuelLow, onRefused);
	signalConnections_.connect (player->base.energyLow, onRefused);
}

void cAiEngine::run()
{
	// драйвер зовёт run() после высадки всех игроков: снимок до первого хода
	if (!metricsStarted_ && !players_.empty())
	{
		metrics_.sampleStart (players_.front().client->getModel());
		metricsStarted_ = true;
	}

	for (auto& ctx : players_)
	{
		const cModel& model = ctx.client->getModel();
		const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
		if (turn != ctx.lastTurn)
		{
			ctx.lastTurn = turn;
			ctx.acted = false;
			ctx.endTurnRetries = 0;
		}

		const cPlayer* active = model.getActiveTurnPlayer();
		// activeTurnPlayer выставляется оптимистично в setPlayerList ещё до
		// высадки (initNewGame); landingPos ставится только при её применении.
		// Ждём, пока модель реально получила высадку — иначе дайджест пустой.
		const bool myTurn = active != nullptr && active->getId() == ctx.playerId
			&& active->getLandingPos() != cPosition (-1, -1) && !active->getHasFinishedTurn();
		if (!myTurn)
		{
			ctx.endTurnSent = false;
			ctx.endTurnRefused = false;
			continue;
		}

		if (!ctx.acted)
		{
			act (ctx);
			ctx.acted = true;
		}
		else if (ctx.endTurnRefused)
			retryEndTurn (ctx, turn);
	}
}

bool cAiEngine::isAnyPlayerDefeated() const
{
	if (players_.empty()) return false;
	const auto& list = players_.front().client->getModel().getPlayerList();
	return std::any_of (list.begin(), list.end(), [] (const auto& player) { return player->isDefeated; });
}

void cAiEngine::sendEndTurn (sCtx& ctx)
{
	ctx.endTurnSent = true;
	ctx.endTurnRefused = false;
	// замороженный клиент молча не шлёт EndTurn (cClient::endTurn) — повторим позже
	if (ctx.client->getFreezeModes().isFrozen())
	{
		ctx.endTurnRefused = true;
		return;
	}
	ctx.client->endTurn();
}

void cAiEngine::retryEndTurn (sCtx& ctx, int turn)
{
	constexpr int kMaxEndTurnRetries = 20;

	if (ctx.client->getFreezeModes().isFrozen()) return;
	if (ctx.endTurnRetries >= kMaxEndTurnRetries)
	{
		if (ctx.endTurnRetries == kMaxEndTurnRetries)
		{
			++ctx.endTurnRetries;
			Log.error ("ai_engine: EndTurn of player " + std::to_string (ctx.playerId) + " refused "
				+ std::to_string (kMaxEndTurnRetries) + " times at turn " + std::to_string (turn));
		}
		ctx.endTurnRefused = false;
		return;
	}
	++ctx.endTurnRetries;
	journal_.log ({{"event", "end_turn_retry"}, {"player", ctx.playerId}, {"turn", turn},
		{"retry", ctx.endTurnRetries}});
	sendEndTurn (ctx);
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

	// решения мозга, которые не приказы (операции армии, шаг 8), — до приказов хода
	for (auto& ev : ctx.brain->takeJournalEvents())
	{
		if (!ev.contains ("event")) ev["event"] = "decision";
		ev["player"] = ctx.playerId;
		ev["turn"] = turn;
		journal_.log (ev);
	}

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
			entry["orderReason"] = order.reason; // reason приказа — под причиной отказа
			entry["reason"] = v.reason;
			journal_.log (std::move (entry));
			Log.info ("ai_engine: rejected " + std::string (toString (order.type)) + ": " + v.reason);
		}
	}

	sendEndTurn (ctx);
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

void cAiEngine::writeMetrics (const sGameResult& result)
{
	if (players_.empty()) return;

	// берём финальную модель первого клиента (все клиенты синхронны).
	const cModel& model = players_.front().client->getModel();

	nlohmann::json m;
	m["turn"] = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	m["gameTime"] = model.getGameTime();
	// исход партии (см. decideGameResult)
	m["winner"] = result.winner;
	m["winReason"] = result.winReason;
	m["endReason"] = result.endReason;
	m["endTurn"] = result.endTurn;
	m["materialWinRatio"] = kMaterialWinRatio;
	// зависшие концы хода, развязанные движком (cModel::setResolveStalledTurnEnd)
	m["stalledTurnEnds"] = model.getStalledTurnEnds();
	m["forcedTurnEnds"] = model.getForcedTurnEnds();

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
		// для D-score: стоимость живого, пик юнитов, потери за окна, собранные ресурсы
		p["material"] = materialValue (*player);
		p.update (metrics_.playerSummary (player->getId()));
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

sGameResult cAiEngine::finalize (eGameEnd end, int endTurn)
{
	sGameResult result;
	if (!players_.empty())
	{
		const cModel& model = players_.front().client->getModel();
		result = decideGameResult (model, end, endTurn);
		metrics_.finish (model, endTurn);
		writeMetrics (result);
		const std::string turnsPath = outDir_ + "/metrics_turns.jsonl";
		if (!metrics_.writeTurns (turnsPath))
			Log.error ("ai_engine: cannot write " + turnsPath);
	}
	journal_.close();
	return result;
}

} // namespace ai
