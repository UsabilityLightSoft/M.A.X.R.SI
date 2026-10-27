/***************************************************************************
 * ai_metrics_collector.cpp — метрики партии для D-score и исход партии.
 ***************************************************************************/

#include "ai_metrics_collector.h"

#include "game/data/base/base.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/vehicle.h"
#include "game/logic/turncounter.h"

#include <algorithm>
#include <fstream>

namespace ai
{

namespace
{

int unitCount (const cPlayer& player)
{
	return static_cast<int> (player.getVehicles().size() + player.getBuildings().size());
}

// Сколько выработки шахт реально собрано: makeTurnStart подбазы кладёт prod в
// склад, тут же отдаёт needed потребителям и срезает склад до maxStored — всё,
// что не влезло, пропадает. Собрано = min(prod, место в складе + расход).
int collected (int prod, int stored, int maxStored, int needed)
{
	return std::clamp (std::min (prod, maxStored - stored + needed), 0, prod);
}

} // namespace

//------------------------------------------------------------------------------
int materialValue (const cPlayer& player)
{
	int value = 0;
	for (const auto& vehicle : player.getVehicles())
		value += vehicle->data.getBuildCost();
	for (const auto& building : player.getBuildings())
		value += building->data.getBuildCost();
	return value;
}

//------------------------------------------------------------------------------
sGameResult decideGameResult (const cModel& model, eGameEnd end, int endTurn)
{
	sGameResult result;
	result.endTurn = endTurn;
	result.endReason = end == eGameEnd::Elimination ? "elimination" : end == eGameEnd::TurnLimit ? "turnLimit" : "watchdog";

	const auto& players = model.getPlayerList();
	if (end == eGameEnd::Elimination)
	{
		std::vector<int> alive;
		for (const auto& player : players)
			if (!player->isDefeated) alive.push_back (player->getId());
		if (alive.size() == 1)
		{
			result.winner = alive.front();
			result.winReason = "elimination";
		}
		else
			result.winReason = "draw"; // уничтожены все сразу
		return result;
	}

	// лимит ходов: по материалу
	int best = -1;
	int bestValue = -1;
	int secondValue = -1;
	for (const auto& player : players)
	{
		const int value = materialValue (*player);
		if (value > bestValue)
		{
			secondValue = bestValue;
			bestValue = value;
			best = player->getId();
		}
		else if (value > secondValue)
			secondValue = value;
	}
	if (best >= 0 && bestValue > 0 && bestValue >= kMaterialWinRatio * std::max (secondValue, 0))
	{
		result.winner = best;
		result.winReason = "material";
	}
	else
		result.winReason = "draw";
	return result;
}

//------------------------------------------------------------------------------
void cMetricsCollector::attach (const cModel& model)
{
	connections.connect (model.gameTimeChanged, [this, &model]() { onTick (model); });
	connections.connect (model.newTurnStarted, [this, &model] (const sNewTurnReport&) { onTurnStart (model); });
}

//------------------------------------------------------------------------------
void cMetricsCollector::sampleStart (const cModel& model)
{
	playerIds.clear();
	for (const auto& player : model.getPlayerList())
	{
		playerIds.push_back (player->getId());
		auto& total = totals[player->getId()];
		total.unitsPeak = unitCount (*player);
		total.lostVehiclesAtRoundStart = player->getGameOverStat().lostVehiclesCount;
		total.lostBuildingsAtRoundStart = player->getGameOverStat().lostBuildingsCount;
	}
	openRound = model.getTurnCounter()->getTurn();
}

//------------------------------------------------------------------------------
void cMetricsCollector::onTick (const cModel& model)
{
	// Состояние подбаз на начало тика. В тике начала хода до makeTurnStart идут
	// только движение (активных нет) и атаки (разрушение — позже, в helperJobs),
	// склады и добыча до неё не меняются: снимок точно «до добычи».
	for (const auto& player : model.getPlayerList())
	{
		auto& list = subbasesBeforeTick[player->getId()];
		list.clear();
		for (const auto& subbase : player->base.SubBases)
		{
			const auto& s = subbase->getResourcesStored();
			const auto& m = subbase->getMaxResourcesStored();
			const auto& n = subbase->getResourcesNeeded();
			const auto& p = subbase->getProd();
			list.push_back ({{s.metal, s.oil, s.gold}, {m.metal, m.oil, m.gold}, {n.metal, n.oil, n.gold}, {p.metal, p.oil, p.gold}});
		}
	}
}

//------------------------------------------------------------------------------
void cMetricsCollector::sampleUnits (const cModel& model)
{
	for (const auto& player : model.getPlayerList())
	{
		auto& total = totals[player->getId()];
		total.unitsPeak = std::max (total.unitsPeak, unitCount (*player));
	}
}

//------------------------------------------------------------------------------
void cMetricsCollector::onTurnStart (const cModel& model)
{
	const cPlayer* active = model.getActiveTurnPlayer();
	if (active == nullptr || openRound < 1) return;
	const int turn = model.getTurnCounter()->getTurn();

	sampleUnits (model);

	// начало круга (ход первого игрока): закрыть прошлый круг — юниты на его
	// конец и потери за него из дельты GameOverStat
	if (active == model.getPlayerList().front().get() && turn > openRound)
	{
		for (const auto& player : model.getPlayerList())
		{
			auto& row = turns[openRound][player->getId()];
			auto& total = totals[player->getId()];
			const auto& stat = player->getGameOverStat();
			row.units = unitCount (*player);
			row.lostVehicles = static_cast<int> (stat.lostVehiclesCount - total.lostVehiclesAtRoundStart);
			row.lostBuildings = static_cast<int> (stat.lostBuildingsCount - total.lostBuildingsAtRoundStart);
			total.lostVehiclesAtRoundStart = stat.lostVehiclesCount;
			total.lostBuildingsAtRoundStart = stat.lostBuildingsCount;
		}
		openRound = turn;
	}

	// добыча на начало хода активного игрока (в ходе 1 makeTurnStart не вызывается)
	if (turn > 1)
	{
		auto& row = turns[turn][active->getId()];
		for (const auto& sb : subbasesBeforeTick[active->getId()])
		{
			row.resources.metal += collected (sb.prod.metal, sb.stored.metal, sb.maxStored.metal, sb.needed.metal);
			row.resources.oil += collected (sb.prod.oil, sb.stored.oil, sb.maxStored.oil, sb.needed.oil);
			row.resources.gold += collected (sb.prod.gold, sb.stored.gold, sb.maxStored.gold, sb.needed.gold);
			row.prod += sb.prod.sum();
		}
	}
}

//------------------------------------------------------------------------------
void cMetricsCollector::finish (const cModel& model, int endTurn_)
{
	endTurn = endTurn_;
	sampleUnits (model);
}

//------------------------------------------------------------------------------
nlohmann::json cMetricsCollector::playerSummary (int playerId) const
{
	std::vector<int> losses;
	int resourcesTotal = 0;
	int resourcesMaxTurn = 0;
	for (const auto& [turn, players] : turns)
	{
		if (turn < 1 || turn >= endTurn || turn >= openRound) continue; // круг не сыгран целиком
		const auto it = players.find (playerId);
		const sPlayerTurn row = it == players.end() ? sPlayerTurn{} : it->second;
		losses.push_back (row.lostVehicles + row.lostBuildings);
		resourcesTotal += row.resources.sum();
		resourcesMaxTurn = std::max (resourcesMaxTurn, row.resources.sum());
	}

	// максимум потерь за окно из k ходов подряд (партия короче окна — вся партия)
	const auto maxWindow = [&] (std::size_t k) {
		int best = 0;
		for (std::size_t i = 0; i < losses.size(); ++i)
		{
			int sum = 0;
			for (std::size_t j = i; j < std::min (losses.size(), i + k); ++j)
				sum += losses[j];
			best = std::max (best, sum);
		}
		return best;
	};

	const auto total = totals.find (playerId);
	nlohmann::json j;
	j["unitsPeak"] = total == totals.end() ? 0 : total->second.unitsPeak;
	j["lossesMax1"] = maxWindow (1);
	j["lossesMax3"] = maxWindow (3);
	j["lossesMax5"] = maxWindow (5);
	j["resourcesTotal"] = resourcesTotal;
	j["resourcesMaxTurn"] = resourcesMaxTurn;
	return j;
}

//------------------------------------------------------------------------------
bool cMetricsCollector::writeTurns (const std::filesystem::path& path) const
{
	std::ofstream out (path);
	if (!out.is_open()) return false;
	for (const auto& [turn, players] : turns)
	{
		if (turn < 1 || turn >= endTurn || turn >= openRound) continue;
		nlohmann::json line;
		line["turn"] = turn;
		nlohmann::json list = nlohmann::json::array();
		for (const int id : playerIds)
		{
			const auto it = players.find (id);
			const sPlayerTurn row = it == players.end() ? sPlayerTurn{} : it->second;
			list.push_back ({{"id", id},
				{"units", row.units},
				{"lost", row.lostVehicles + row.lostBuildings},
				{"lostVehicles", row.lostVehicles},
				{"lostBuildings", row.lostBuildings},
				{"resources", row.resources.sum()},
				{"metal", row.resources.metal},
				{"oil", row.resources.oil},
				{"gold", row.resources.gold},
				{"prod", row.prod}});
		}
		line["players"] = std::move (list);
		out << line.dump() << "\n";
	}
	return true;
}

} // namespace ai
