/***************************************************************************
 * ai_client_observer.cpp — журнал действий других игроков в формате ИИИ (шаг 15).
 ***************************************************************************/

#include "ai_client_observer.h"

#include "ai_game_state.h"
#include "ai_upgrades.h"

#include "game/data/gamesettings.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/vehicle.h"
#include "game/logic/action/action.h"
#include "game/logic/turncounter.h"
#include "game/protocol/netmessage.h"
#include "utility/log.h"
#include "utility/serialization/jsonarchive.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>

namespace ai_client
{

namespace
{

int num (const nlohmann::json& j, const char* key, int def = 0)
{
	if (!j.is_object() || !j.contains (key)) return def;
	const auto& v = j[key];
	if (v.is_number_integer()) return v.get<int>();
	if (v.is_number_unsigned()) return static_cast<int> (v.get<unsigned int>());
	if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
	if (v.is_number_float()) return static_cast<int> (v.get<double>());
	return def;
}

bool flag (const nlohmann::json& j, const char* key)
{
	return num (j, key, 0) != 0;
}

cPosition posOf (const nlohmann::json& j)
{
	if (!j.is_object()) return cPosition (0, 0);
	return cPosition (num (j, "X"), num (j, "Y"));
}

ai::sTypeId typeOf (const nlohmann::json& j)
{
	return ai::sTypeId (num (j, "firstPart"), num (j, "secondPart"));
}

std::string typeIdText (const ai::sTypeId& t)
{
	return std::to_string (t.firstPart) + "." + std::to_string (t.secondPart);
}

std::string actionName (const nlohmann::json& a)
{
	return a.contains ("action") && a["action"].is_string() ? a["action"].get<std::string>() : std::string ("?");
}

// приказы Order API из одного действия (BuyUpgrades — по приказу на тип юнита)
std::vector<ai::cOrder> actionToOrders (const nlohmann::json& a, const cModel& model)
{
	using ai::eOrderType;
	const std::string name = actionName (a);
	const int player = num (a, "playerNr", -1);
	ai::cOrder o;
	o.playerId = player;
	o.level = "human";

	if (name == "StartMove")
	{
		o.type = eOrderType::Move;
		o.unitId = num (a, "unitId");
		if (a.contains ("path") && a["path"].is_array())
			for (const auto& p : a["path"]) o.path.push_back (posOf (p));
		// цель пути — последняя клетка (у приказа ИИИ pos — цель, путь ведёт к ней)
		if (!o.path.empty()) o.position = o.path.back();
		const auto& end = a.contains ("endMoveAction") ? a["endMoveAction"] : nlohmann::json();
		// cEndMoveAction: None, Load, GetIn, Attack; destID −1 — нет
		const int dest = num (end, "destID", -1);
		switch (num (end, "endMoveAction"))
		{
			case 1: o.moveEnd = ai::eMoveEnd::Load; break;
			case 2: o.moveEnd = ai::eMoveEnd::GetIn; break;
			case 3: o.moveEnd = ai::eMoveEnd::Attack; break;
			default: break;
		}
		if (num (end, "endMoveAction") != 0 && dest >= 0) o.unitId2 = static_cast<unsigned int> (dest);
		return {o};
	}
	if (name == "Attack")
	{
		o.type = eOrderType::Attack;
		o.unitId = num (a, "agressorId");
		o.position = posOf (a.value ("targetPosition", nlohmann::json()));
		// cActionAttack: targetId 0 — «без цели» (движок так и исполняет)
		if (num (a, "targetId") != 0) o.unitId2 = static_cast<unsigned int> (num (a, "targetId"));
		return {o};
	}
	if (name == "StartBuild")
	{
		const bool path = a.contains ("pathEndPosition") && a["pathEndPosition"].is_object();
		o.type = path ? eOrderType::BuildPath : eOrderType::Build;
		o.unitId = num (a, "vehicleID");
		o.buildingType = typeOf (a.value ("buildingTypeID", nlohmann::json()));
		o.buildSpeed = num (a, "buildSpeed");
		o.position = posOf (a.value ("buildPosition", nlohmann::json()));
		if (path) o.position2 = posOf (a["pathEndPosition"]);
		return {o};
	}
	if (name == "ChangeBuildlist")
	{
		o.type = eOrderType::ChangeBuildList;
		o.unitId = num (a, "buildingId");
		if (a.contains ("buildList") && a["buildList"].is_array())
			for (const auto& t : a["buildList"]) o.buildList.push_back (typeOf (t));
		o.buildSpeed = num (a, "buildSpeed");
		o.repeat = flag (a, "repeat");
		return {o};
	}
	if (name == "ChangeResearch")
	{
		o.type = eOrderType::ChangeResearch;
		if (a.contains ("researchAreas") && a["researchAreas"].is_array())
			for (std::size_t i = 0; i < a["researchAreas"].size() && i < o.researchAreas.size(); ++i)
				o.researchAreas[i] = a["researchAreas"][i].is_number() ? a["researchAreas"][i].get<int>() : 0;
		return {o};
	}
	if (name == "RepairReload")
	{
		// eSupplyType: REARM, REPAIR
		o.type = num (a, "supplyType") == 1 ? eOrderType::Repair : eOrderType::Rearm;
		o.unitId = num (a, "sourceUnitId");
		o.unitId2 = num (a, "destUnitId");
		return {o};
	}
	if (name == "Transfer")
	{
		o.type = eOrderType::Transfer;
		o.unitId = num (a, "sourceUnitId");
		o.unitId2 = num (a, "destinationUnitId");
		o.transferValue = num (a, "transferValue");
		o.resType = static_cast<eResourceType> (num (a, "resourceType"));
		return {o};
	}
	if (name == "StartWork" || name == "Stop" || name == "ChangeSentry" || name == "ChangeManualFire" || name == "SelfDestroy")
	{
		o.type = name == "StartWork" ? eOrderType::StartWork
			: name == "Stop" ? eOrderType::StopWork
			: name == "ChangeSentry" ? eOrderType::ToggleSentry
			: name == "ChangeManualFire" ? eOrderType::ToggleManualFire
			: eOrderType::SelfDestroy;
		o.unitId = num (a, "unitId");
		return {o};
	}
	if (name == "MinelayerStatus")
	{
		// действие ставит флаги, приказ ИИИ — переключатель (cClient::toggleLayMines / toggleCollectMines)
		o.unitId = num (a, "vehicleId");
		if (flag (a, "layMines"))
			o.type = eOrderType::ToggleLayMines;
		else if (flag (a, "clearMines"))
			o.type = eOrderType::ToggleCollectMines;
		else
		{
			const cVehicle* v = model.getVehicleFromID (o.unitId);
			o.type = (v != nullptr && v->isUnitClearingMines()) ? eOrderType::ToggleCollectMines : eOrderType::ToggleLayMines;
		}
		return {o};
	}
	if (name == "Load")
	{
		o.type = eOrderType::Load;
		o.unitId = num (a, "loadingUnitId");
		o.unitId2 = num (a, "loadedVehicleId");
		return {o};
	}
	if (name == "Activate")
	{
		o.type = eOrderType::ActivateUnit;
		o.unitId = num (a, "containingUnitId");
		o.unitId2 = num (a, "activatedVehicleId");
		o.position = posOf (a.value ("position", nlohmann::json()));
		return {o};
	}
	if (name == "FinishBuild")
	{
		o.type = eOrderType::FinishBuild;
		o.unitId = num (a, "unitId");
		o.position = posOf (a.value ("escapePosition", nlohmann::json()));
		return {o};
	}
	if (name == "BuyUpgrades")
	{
		std::vector<ai::cOrder> out;
		if (!a.contains ("unitUpgrades") || !a["unitUpgrades"].is_array()) return out;
		const cPlayer* owner = model.getPlayer (player);
		for (const auto& entry : a["unitUpgrades"])
		{
			ai::cOrder u = o;
			u.type = eOrderType::BuyUpgrades;
			u.unitType = typeOf (entry.value ("first", nlohmann::json()));
			const auto& upgrades = entry.contains ("second") ? entry["second"].value ("upgrades", nlohmann::json()) : nlohmann::json();
			if (upgrades.is_array())
				for (const auto& up : upgrades)
				{
					const int kind = num (up, "type", -1);
					const auto it = std::find_if (ai::kUpgradeKinds.begin(), ai::kUpgradeKinds.end(), [&] (const auto& k) { return static_cast<int> (k) == kind; });
					if (it != ai::kUpgradeKinds.end()) u.upgradeSteps[it - ai::kUpgradeKinds.begin()] = num (up, "purchased");
				}
			// цена — тем же расчётом, что у мозга (модель ещё до покупки)
			if (owner != nullptr)
			{
				cUnitUpgrade dummy;
				int cost = 0;
				if (ai::makeUnitUpgrade (model, *owner, sID (u.unitType.firstPart, u.unitType.secondPart), u.upgradeSteps, dummy, cost))
					u.upgradeCost = cost;
				else
					u.upgradeCost = -1;
			}
			out.push_back (std::move (u));
		}
		return out;
	}
	if (name == "UpgradeVehicle")
	{
		o.type = eOrderType::UpgradeVehicle;
		o.unitId = num (a, "buildingId");
		// cActionUpgradeVehicle: vehicleId 0 — все устаревшие (у приказа ИИИ — kNoUnit)
		if (num (a, "vehicleId") != 0) o.unitId2 = static_cast<unsigned int> (num (a, "vehicleId"));
		return {o};
	}
	if (name == "UpgradeBuilding")
	{
		o.type = eOrderType::UpgradeBuilding;
		o.unitId = num (a, "buildingId");
		o.allUnits = flag (a, "allBuildings");
		return {o};
	}
	if (name == "ResourceDistribution")
	{
		o.type = eOrderType::SetMining;
		o.unitId = num (a, "buildingId");
		o.mining = {num (a, "metal"), num (a, "oil"), num (a, "gold")};
		return {o};
	}
	if (name == "StealDisable")
	{
		o.type = flag (a, "steal") ? eOrderType::Steal : eOrderType::Disable;
		o.unitId = num (a, "infiltratorId");
		o.unitId2 = num (a, "targetId");
		return {o};
	}
	return {};
}

} // namespace

std::optional<ai::cOrder> actionToOrder (const nlohmann::json& action, const cModel& model)
{
	auto orders = actionToOrders (action, model);
	if (orders.empty()) return std::nullopt;
	return orders.front();
}

// Копия orderToJson из ai_engine.cpp на 26.10.2026 20:50 (с kNoUnit шага 13; там — в безымянном
// пространстве имён, файл правят шаги 13 и 16). Поменяли там — поменять здесь: сверка — tmp/step15/cmp_observed.py.
// Приказ целиком — поля, нужные его типу, всегда; прочие — если не по умолчанию.
nlohmann::json orderToJson (const ai::cOrder& o)
{
	using ai::eOrderType;
	const auto is = [&o] (std::initializer_list<eOrderType> types) {
		return std::find (types.begin(), types.end(), o.type) != types.end();
	};
	nlohmann::json j;
	j["type"] = ai::toString (o.type);
	j["player"] = o.playerId;
	j["level"] = o.level;
	j["reason"] = o.reason;
	j["unitId"] = o.unitId;
	// шаг 13: unitId2 пишется, когда задан (kNoUnit — нет; 0 — настоящая машина, пишется)
	if (o.unitId2 != ai::kNoUnit) j["unitId2"] = o.unitId2;
	if (o.moveEnd != ai::eMoveEnd::Attack) j["moveEnd"] = ai::toString (o.moveEnd);
	if (is ({eOrderType::Move, eOrderType::Attack, eOrderType::Build, eOrderType::BuildPath, eOrderType::ActivateUnit, eOrderType::FinishBuild})
		|| o.position != cPosition (0, 0))
	{
		j["pos"] = {{"x", o.position.x()}, {"y", o.position.y()}};
	}
	if (o.type == eOrderType::BuildPath || o.position2 != cPosition (0, 0))
		j["pos2"] = {{"x", o.position2.x()}, {"y", o.position2.y()}};
	if (is ({eOrderType::Build, eOrderType::BuildPath}) || o.buildingType != ai::sTypeId())
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
		j["researchAreas"] = o.researchAreas;
	if (o.type == eOrderType::Transfer || o.transferValue != 0 || o.resType != eResourceType::None)
	{
		j["transferValue"] = o.transferValue;
		j["resType"] = static_cast<int> (o.resType);
	}
	if (o.type == eOrderType::BuyUpgrades || o.unitType != ai::sTypeId())
		j["unitType"] = typeIdText (o.unitType);
	if (o.type == eOrderType::BuyUpgrades || std::any_of (o.upgradeSteps.begin(), o.upgradeSteps.end(), [] (int n) { return n != 0; }))
	{
		nlohmann::json steps = nlohmann::json::object();
		for (size_t k = 0; k < o.upgradeSteps.size(); ++k)
			if (o.upgradeSteps[k] != 0) steps[ai::upgradeKindName (static_cast<int> (k))] = o.upgradeSteps[k];
		j["upgradeSteps"] = std::move (steps);
		j["upgradeCost"] = o.upgradeCost;
	}
	if (o.type == eOrderType::UpgradeBuilding || o.allUnits)
		j["allUnits"] = o.allUnits;
	if (o.type == eOrderType::SetMining || std::any_of (o.mining.begin(), o.mining.end(), [] (int n) { return n != 0; }))
		j["mining"] = {{"metal", o.mining[0]}, {"oil", o.mining[1]}, {"gold", o.mining[2]}};
	return j;
}

//------------------------------------------------------------------------------
cObserver::cObserver (std::string outDir_, int aiPlayerId_) :
	outDir (std::move (outDir_)),
	aiPlayerId (aiPlayerId_),
	journal (outDir + "/journal_human.jsonl")
{
	std::error_code ec;
	std::filesystem::create_directories (outDir + "/digest", ec);
	if (!journal.open())
		Log.error ("ai_client: cannot open " + journal.getPath());
}

cObserver::~cObserver()
{
	close();
}

void cObserver::close()
{
	journal.close();
}

void cObserver::onMessage (cNetMessage& message, const cModel& model)
{
	if (message.getType() != eNetMessageType::ACTION) return;

	nlohmann::json a;
	{
		cJsonArchiveOut archive (a);
		archive << message;
	}
	const int player = message.playerNr;
	const std::string name = actionName (a);
	if (player == aiPlayerId)
	{
		++ownEchoes[name];
		return;
	}
	if (model.getPlayer (player) == nullptr) return; // как cClient: действие неизвестного игрока не исполняется

	const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	auto& st = players[player];
	++st.byType[name];

	if (name == "EndTurn")
	{
		journal.log ({{"event", "turn_end"}, {"player", player}, {"turn", turn}, {"orders", st.turnOrders}, {"passes", 1},
			{"source", "human"}});
		st.turnOrders = 0;
		return;
	}
	if (name == "InitNewGame")
	{
		nlohmann::json e = {{"event", "landing"}, {"player", player}, {"turn", turn}, {"source", "human"}, {"clan", num (a, "clan", -1)}};
		const cPosition p = posOf (a.value ("landingPosition", nlohmann::json()));
		e["pos"] = {{"x", p.x()}, {"y", p.y()}};
		nlohmann::json units = nlohmann::json::array();
		if (a.contains ("landingUnits") && a["landingUnits"].is_array())
			for (const auto& u : a["landingUnits"])
				units.push_back ({{"type", typeIdText (typeOf (u.value ("unitID", nlohmann::json())))}, {"cargo", num (u, "cargo")}});
		e["units"] = std::move (units);
		journal.log (e);
		return;
	}
	logOrder (player, turn, a, model);
}

void cObserver::logOrder (int player, int turn, const nlohmann::json& action, const cModel& model)
{
	auto& st = players[player];
	const auto orders = actionToOrders (action, model);
	if (orders.empty())
	{
		// действие без приказа Order API (ResumeMove, SetAutoMove, Clear, ChangeUnitName, StartTurn, AiPass) — как есть
		++st.otherActions;
		nlohmann::json e = {{"event", "action"}, {"player", player}, {"turn", turn}, {"source", "human"}, {"action", action}};
		e["action"].erase ("type");
		journal.log (e);
		return;
	}
	for (const auto& order : orders)
	{
		nlohmann::json entry = orderToJson (order);
		entry["event"] = "order";
		entry["seq"] = ++seq;
		entry["turn"] = turn;
		entry["pass"] = 1;
		entry["source"] = "human";
		journal.log (entry);
		++st.turnOrders;
		++st.orders;
	}
}

bool cObserver::mustWait (const cNetMessage& message, const cModel& model) const
{
	if (message.getType() != eNetMessageType::ACTION || message.playerNr == aiPlayerId) return false;
	const auto settings = model.getGameSettings();
	if (!settings || settings->gameType != eGameSettingsGameType::Turns) return false;
	if (static_cast<const cAction&> (message).getType() == cAction::eActiontype::InitNewGame) return false;
	if (model.getPlayer (message.playerNr) == nullptr) return false;
	// сервер исполнил действие в ход этого игрока: модель клиента дойдёт туда по сообщениям, что уже отданы
	const cPlayer* active = model.getActiveTurnPlayer();
	return active == nullptr || active->getId() != message.playerNr;
}

void cObserver::tick (const cModel& model, int forcePlayer)
{
	const cPlayer* active = model.getActiveTurnPlayer();
	if (active == nullptr || active->getId() == aiPlayerId) return;
	if (active->getLandingPos() == cPosition (-1, -1) || active->getHasFinishedTurn() || active->isDefeated) return;
	if (!model.getTurnCounter()) return;
	if (!model.isTurnStartPlayedOut() && active->getId() != forcePlayer) return;
	const int turn = model.getTurnCounter()->getTurn();
	auto& st = players[active->getId()];
	if (st.digestTurn == turn) return;
	st.digestTurn = turn;
	st.turnOrders = 0;

	auto digest = ai::buildGameStateDigest (model, active->getId());
	digest["visibility"] = ai::buildVisibility (model, active->getId());
	const std::string path = outDir + "/digest/turn" + std::to_string (turn) + "_p" + std::to_string (active->getId()) + ".json";
	std::ofstream file (path);
	if (file.is_open())
	{
		file << digest.dump (2) << "\n";
		++digests;
	}
	else
		Log.error ("ai_client: cannot write digest " + path);
	journal.log ({{"event", "turn_start"}, {"player", active->getId()}, {"turn", turn}, {"brain", "human"},
		{"name", active->getName()}, {"source", "human"}});
}

nlohmann::json cObserver::summary() const
{
	nlohmann::json s;
	nlohmann::json ps = nlohmann::json::object();
	for (const auto& [id, st] : players)
		ps[std::to_string (id)] = {{"orders", st.orders}, {"otherActions", st.otherActions}, {"actions", st.byType}};
	s["players"] = std::move (ps);
	s["humanDigests"] = digests;
	s["ownActionsEchoed"] = ownEchoes;
	return s;
}

} // namespace ai_client
