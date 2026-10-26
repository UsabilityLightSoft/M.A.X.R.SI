/***************************************************************************
 * ai_engine.cpp — оркестрация ИИ (PLAN_AI этап 2).
 ***************************************************************************/

#include "ai_engine.h"

#include "ai_game_state.h"
#include "ai_order_validator.h"
#include "ai_types.h"
#include "ai_upgrades.h"

#include "game/data/map/map.h"
#include "game/data/miningresource.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/id.h"
#include "game/data/units/unit.h"
#include "game/data/units/vehicle.h"
#include "game/data/map/mapview.h"
#include "game/logic/attackjob.h"
#include "game/logic/client.h"
#include "game/logic/endmoveaction.h"
#include "game/logic/movejob.h"
#include "game/logic/turncounter.h"
#include "game/logic/upgradecalculator.h"
#include "utility/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <forward_list>
#include <initializer_list>

namespace ai
{

namespace
{

std::shared_ptr<const cPlayer> sharedPlayerOf (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id) return p;
	return nullptr;
}

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
	// шаг 13: unitId2 пишется, когда задан (kNoUnit — нет; 0 — настоящая машина, пишется)
	if (o.unitId2 != kNoUnit) j["unitId2"] = o.unitId2;
	// шаг 11: Move с unitId2 — что сделать с ним в конце пути (выстрел — по умолчанию, не пишется)
	if (o.moveEnd != eMoveEnd::Attack) j["moveEnd"] = toString (o.moveEnd);
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
	// шаг 10: улучшения за золото, обновление юнитов, распределение добычи
	if (o.type == eOrderType::BuyUpgrades || o.unitType != sTypeId())
		j["unitType"] = typeIdText (o.unitType);
	if (o.type == eOrderType::BuyUpgrades || std::any_of (o.upgradeSteps.begin(), o.upgradeSteps.end(), [] (int n) { return n != 0; }))
	{
		nlohmann::json steps = nlohmann::json::object(); // вид улучшения → шагов (только ненулевые)
		for (size_t k = 0; k < o.upgradeSteps.size(); ++k)
			if (o.upgradeSteps[k] != 0) steps[upgradeKindName (static_cast<int> (k))] = o.upgradeSteps[k];
		j["upgradeSteps"] = std::move (steps);
		j["upgradeCost"] = o.upgradeCost;
	}
	if (o.type == eOrderType::UpgradeBuilding || o.allUnits)
		j["allUnits"] = o.allUnits;
	if (o.type == eOrderType::SetMining || std::any_of (o.mining.begin(), o.mining.end(), [] (int n) { return n != 0; }))
		j["mining"] = {{"metal", o.mining[0]}, {"oil", o.mining[1]}, {"gold", o.mining[2]}};
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

	// шаг 11: предел заходов за ход
	maxPasses_ = kDefaultMaxPasses;
	if (const char* env = std::getenv ("MAXR_PASSES"); env != nullptr && *env != 0)
	{
		const int n = std::atoi (env);
		if (n >= 1 && n <= 50)
			maxPasses_ = n;
		else
			Log.warn ("ai_engine: MAXR_PASSES=" + std::string (env) + " ignored (allowed 1..50)");
	}
	Log.info ("ai_engine: passes per turn at most " + std::to_string (maxPasses_));
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
	// шаг 13: своё выключено коммандос врага — мозгу перед его ходом (сигнал модели этого игрока)
	signalConnections_.connect (client->getModel().unitDisabled, [this, index] (const cUnit&, const cUnit& target) {
		if (target.getOwner() != nullptr && target.getOwner()->getId() == players_[index].playerId)
			players_[index].disabledLog.push_back (target.getId());
	});
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
			ctx.passWaiting = false;
			ctx.passSendPending = false;
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
			ctx.passWaiting = false;
			ctx.passSendPending = false;
			continue;
		}

		if (!ctx.acted)
		{
			// шаг 11: решать, когда начало хода доиграно — ответный огонь, вызванный началом
			// хода (cPlayer::makeTurnStart), попал; иначе обстрелянные машины не двинутся и не
			// выстрелят (в headless модель до этого идёт, в сетевой игре — идёт всегда)
			if (!model.isTurnStartPlayedOut()) continue;
			ctx.acted = true;
			act (ctx);
		}
		else if (ctx.passWaiting)
			continuePass (ctx, turn);
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

	// дайджест состояния (точка зрения игрока); шаг 11 — в файл и видимость клеток (для шага 16,
	// в запрос советника она не идёт: там дайджест без неё)
	auto digest = buildGameStateDigest (model, ctx.playerId);
	digest["visibility"] = buildVisibility (model, ctx.playerId);
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

	// заход 1 — решение мозга на начало хода
	ctx.pass = 1;
	ctx.turnOrders = 0;
	ctx.passWaiting = false;
	auto& stats = passStats_[ctx.playerId];
	++stats.turns;
	++stats.passes;
	if (!ctx.disabledLog.empty())
	{
		journal_.log ({{"event", "disabled"}, {"player", ctx.playerId}, {"turn", turn}, {"units", ctx.disabledLog}});
		ctx.brain->noteDisabled (ctx.disabledLog);
		ctx.disabledLog.clear();
	}
	const auto brainStart = std::chrono::steady_clock::now();
	const auto orders = ctx.brain->decideTurn (ctx.playerId, model);
	brainMs_[ctx.playerId] += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - brainStart).count();
	logBrainEvents (ctx, turn);
	const std::size_t executed = executeOrders (ctx, orders, turn);
	endPass (ctx, turn, executed);
}

void cAiEngine::logBrainEvents (sCtx& ctx, int turn)
{
	// решения мозга, которые не приказы (операции армии, шаг 8), — до приказов захода
	for (auto& ev : ctx.brain->takeJournalEvents())
	{
		if (!ev.contains ("event")) ev["event"] = "decision";
		ev["player"] = ctx.playerId;
		ev["turn"] = turn;
		if (ctx.pass > 1) ev["pass"] = ctx.pass;
		journal_.log (ev);
	}
}

std::size_t cAiEngine::executeOrders (sCtx& ctx, const std::vector<cOrder>& brainOrders, int turn)
{
	const cModel& model = ctx.client->getModel();
	std::size_t executed = 0;
	// шаг 13: «нет цели» — kNoUnit. У мозга, где 0 значил «нет» (base, base2, сторонние), такой 0 у Move с
	// выстрелом в конце пути, Attack и UpgradeVehicle — перевести: его поведение и журнал прежние
	std::vector<cOrder> orders = brainOrders;
	if (ctx.brain->zeroMeansNoUnit())
		for (auto& o : orders)
			if (o.unitId2 == 0 && (o.type == eOrderType::Attack || o.type == eOrderType::UpgradeVehicle
				|| (o.type == eOrderType::Move && o.moveEnd == eMoveEnd::Attack)))
				o.unitId2 = kNoUnit;
	ctx.turnOrders += orders.size();
	// валидация + исполнение
	for (const auto& order : orders)
	{
		++orderCounter_;
		const auto v = validateOrder (model, order);
		nlohmann::json entry = orderToJson (order);
		entry["seq"] = orderCounter_;
		entry["turn"] = turn;
		entry["pass"] = ctx.pass;
		// шаг 11 (для шага 16): у Attack — id цели, в которую попадёт выстрел (в клетке бывают самолёт и танк)
		if (order.type == eOrderType::Attack && order.unitId2 == kNoUnit && v.ok)
			if (const cUnit* aggressor = model.getUnitFromID (order.unitId); aggressor != nullptr && model.getMap()->isValidPosition (order.position))
			{
				const cMapView view (model.getMap(), sharedPlayerOf (model, ctx.playerId));
				if (const cUnit* target = cAttackJob::selectTarget (order.position, aggressor->getStaticUnitData().canAttack, view, aggressor->getOwner()))
					entry["unitId2"] = target->getId();
			}
		if (v.ok)
		{
			executeOrder (*ctx.client, order);
			++executed;
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
	if (ctx.pass > 1) passStats_[ctx.playerId].laterOrders += static_cast<int> (executed);
	return executed;
}

// Конец захода: ещё заход, если мозг хочет, приказы захода что-то изменили, у машин
// есть ход или выстрелы и предел MAXR_PASSES не исчерпан; иначе — конец хода.
void cAiEngine::endPass (sCtx& ctx, int turn, std::size_t executed)
{
	auto& stats = passStats_[ctx.playerId];
	if (ctx.brain->wantsAnotherPass() && executed > 0 && canStillAct (ctx))
	{
		if (ctx.pass < maxPasses_)
		{
			const cModel& model = ctx.client->getModel();
			ctx.passSerial = model.getAiPassSerial();
			ctx.passStartTime = model.getGameTime();
			ctx.passWaiting = true;
			ctx.passResent = false;
			sendAiPass (ctx);
			return;
		}
		++stats.limitHits;
	}
	stats.maxPasses = std::max (stats.maxPasses, ctx.pass);
	if (ctx.pass > 1) ++stats.multiPassTurns;
	sendEndTurn (ctx);
	journal_.log ({{"event", "turn_end"}, {"player", ctx.playerId}, {"turn", turn},
		{"orders", ctx.turnOrders}, {"passes", ctx.pass}});
}

bool cAiEngine::canStillAct (const sCtx& ctx) const
{
	const cPlayer* player = ctx.client->getModel().getPlayer (ctx.playerId);
	if (player == nullptr || player->isDefeated) return false;
	return std::any_of (player->getVehicles().begin(), player->getVehicles().end(), [] (const auto& v) {
		return !v->isDisabled() && (v->data.getSpeed() > 0 || v->data.getShots() > 0);
	});
}

void cAiEngine::sendAiPass (sCtx& ctx)
{
	ctx.passSentAt = std::chrono::steady_clock::now();
	// замороженный клиент молча не шлёт запрос (cClient::aiPass) — повторим, когда оттает
	if (ctx.client->getFreezeModes().isFrozen())
	{
		ctx.passSendPending = true;
		return;
	}
	ctx.passSendPending = false;
	ctx.client->aiPass();
}

// Заходы 2+: модель доиграла приказы прошлого захода — мозг решает снова на новом поле.
void cAiEngine::continuePass (sCtx& ctx, int turn)
{
	const cModel& model = ctx.client->getModel();
	if (ctx.passSendPending)
	{
		if (!ctx.client->getFreezeModes().isFrozen()) sendAiPass (ctx);
		return;
	}
	// ждём, пока запрос дойдёт до модели (номер сменится) и заход доиграется
	if (model.getAiPassSerial() == ctx.passSerial || model.isAiPassActive())
	{
		// Страховка от потерянного запроса (в норме не срабатывает: без окна заход доигрывается
		// за доли секунды, в окне — за секунды; модель сама кончает заход по пределу тиков)
		const auto waited = std::chrono::steady_clock::now() - ctx.passSentAt;
		if (model.getAiPassSerial() == ctx.passSerial && !ctx.passResent && waited > std::chrono::seconds (30))
		{
			ctx.passResent = true;
			++passStats_[ctx.playerId].lostRequests;
			Log.warn ("ai_engine: pass request of player " + std::to_string (ctx.playerId) + " not answered in 30 s, resent");
			journal_.log ({{"event", "pass_resent"}, {"player", ctx.playerId}, {"turn", turn}, {"pass", ctx.pass + 1}});
			sendAiPass (ctx);
		}
		else if (waited > std::chrono::seconds (120))
		{
			ctx.passWaiting = false;
			++passStats_[ctx.playerId].lostRequests;
			Log.error ("ai_engine: pass of player " + std::to_string (ctx.playerId) + " not finished in 120 s, the turn ends");
			journal_.log ({{"event", "pass_lost"}, {"player", ctx.playerId}, {"turn", turn}, {"pass", ctx.pass + 1}});
			sendEndTurn (ctx);
			journal_.log ({{"event", "turn_end"}, {"player", ctx.playerId}, {"turn", turn},
				{"orders", ctx.turnOrders}, {"passes", ctx.pass}});
		}
		return;
	}

	ctx.passWaiting = false;
	++ctx.pass;
	++passStats_[ctx.playerId].passes;
	journal_.log ({{"event", "pass"}, {"player", ctx.playerId}, {"turn", turn}, {"pass", ctx.pass},
		{"ticks", model.getGameTime() - ctx.passStartTime}});

	// снимок захода — сжатый: машины и не свои или повреждённые здания (полный — в заходе 1)
	const auto digest = buildPassDigest (model, ctx.playerId, ctx.pass);
	const std::string dir = outDir_ + "/digest/passes";
	std::error_code ec;
	std::filesystem::create_directories (dir, ec);
	const std::string path = dir + "/turn" + std::to_string (turn) + "_p" + std::to_string (ctx.playerId)
		+ "_k" + std::to_string (ctx.pass) + ".json";
	std::ofstream file (path);
	if (file.is_open())
		file << digest.dump() << "\n";
	else
		Log.error ("ai_engine: cannot write digest " + path);

	const auto brainStart = std::chrono::steady_clock::now();
	const auto orders = ctx.brain->decidePass (ctx.playerId, model, ctx.pass);
	brainMs_[ctx.playerId] += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - brainStart).count();
	logBrainEvents (ctx, turn);
	const std::size_t executed = executeOrders (ctx, orders, turn);
	endPass (ctx, turn, executed);
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
			// шаг 9: «подъехал — выстрелил» — unitId2 у Move — цель выстрела в конце пути.
			// Движок стреляет сам, когда машина доехала, если цель видна, в дальности и
			// выстрел остался (cEndMoveAction::Attacking — так ходит и GUI по клику на врага).
			// Шаг 11: moveEnd — погрузить unitId2 (транспорт подъехал) или войти в него.
			// шаг 13: kNoUnit — «без цели»; 0 — машина с id 0 (и у выстрела, и у погрузки)
			const bool hasEnd = o.unitId2 != kNoUnit;
			const cUnit* target = hasEnd ? model.getUnitFromID (o.unitId2) : nullptr;
			cEndMoveAction end = cEndMoveAction::None();
			if (target != nullptr)
			{
				switch (o.moveEnd)
				{
					case eMoveEnd::Load: end = cEndMoveAction::Load (*target); break;
					case eMoveEnd::GetIn: end = cEndMoveAction::GetIn (*target); break;
					case eMoveEnd::Attack: end = cEndMoveAction::Attacking (*target); break;
				}
			}
			client.startMove (*v, path, eStart::Immediate, eStopOn::Never, end);
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

		// --- шаг 10 ---
		case eOrderType::BuyUpgrades:
		{
			const cPlayer* player = model.getPlayer (o.playerId);
			if (player == nullptr) return;
			const sID id (o.unitType.firstPart, o.unitType.secondPart);
			cUnitUpgrade upgrade;
			int cost = 0;
			if (!makeUnitUpgrade (model, *player, id, o.upgradeSteps, upgrade, cost)) return;
			client.buyUpgrades ({{id, upgrade}});
			return;
		}

		case eOrderType::UpgradeVehicle:
		{
			auto* b = model.getBuildingFromID (o.unitId);
			if (b == nullptr) return;
			if (o.unitId2 == kNoUnit)
				client.upgradeAllVehicles (*b);
			else if (auto* v = model.getVehicleFromID (o.unitId2))
				client.upgradeVehicle (*b, *v);
			return;
		}

		case eOrderType::UpgradeBuilding:
		{
			auto* b = model.getBuildingFromID (o.unitId);
			if (b == nullptr) return;
			if (o.allUnits) client.upgradeAllBuildings (*b);
			else client.upgradeBuilding (*b);
			return;
		}

		case eOrderType::SetMining:
		{
			auto* b = model.getBuildingFromID (o.unitId);
			if (b == nullptr) return;
			sMiningResource prod;
			prod.metal = o.mining[0];
			prod.oil = o.mining[1];
			prod.gold = o.mining[2];
			client.changeResourceDistribution (*b, prod);
			return;
		}

		// --- шаг 11: коммандос ---
		case eOrderType::Disable:
		case eOrderType::Steal:
		{
			auto* commando = model.getVehicleFromID (o.unitId);
			auto* target = model.getUnitFromID (o.unitId2);
			if (commando == nullptr || target == nullptr) return;
			if (o.type == eOrderType::Steal)
				client.steal (*commando, *target);
			else
				client.disable (*commando, *target);
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
	// шаг 11: заходы хода — предел и заходы, кончившиеся по пределу тиков модели
	m["maxPasses"] = maxPasses_;
	m["passTimeouts"] = model.getAiPassTimeouts();

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
		if (const auto it = passStats_.find (player->getId()); it != passStats_.end())
		{
			const sPassStats& s = it->second;
			p["passes"] = {{"turns", s.turns}, {"passes", s.passes}, {"multiPassTurns", s.multiPassTurns},
				{"maxPasses", s.maxPasses}, {"laterOrders", s.laterOrders}, {"limitHits", s.limitHits},
				{"lostRequests", s.lostRequests}};
		}
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
	// шаг 13: время мозга — только в stdout (по часам: в журналы и метрики не идёт)
	std::string brainTime = "[ai_engine] brain time ms:";
	for (const auto& [id, ms] : brainMs_)
		brainTime += " p" + std::to_string (id) + "=" + std::to_string (static_cast<long long> (ms));
	std::printf ("%s\n", brainTime.c_str());
	std::fflush (stdout);
	return result;
}

} // namespace ai
