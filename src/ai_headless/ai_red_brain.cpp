/***************************************************************************
 * ai_red_brain.cpp — сценарный противник «красная команда-1» (шаг 14, П23).
 * Что и зачем — в ai_red_brain.h; запуск и сводка — TargetFiles/tools/red_run.py.
 ***************************************************************************/

#include "ai_red_brain.h"

#include "ai_base2_rule_brain.h"
#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/commandodata.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"
#include "game/logic/turncounter.h"
#include "utility/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace ai
{

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr int kRingRadius = 11;    // облёт AWAC вокруг базы (обзор AWAC 12)
constexpr int kStagingRadius = 16; // стоянка самолётов: вне обзора зданий (3–4) и вне досягаемости зениток (8)
constexpr int kTankStagingRadius = 11;

std::shared_ptr<const cPlayer> sharedPlayerOf (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id) return p;
	return nullptr;
}

std::vector<cPosition> toVector (const std::forward_list<cPosition>& path)
{
	return {path.begin(), path.end()};
}

// путь до хода: столько клеток, сколько машина пройдёт с оставшимся ходом
void cutToTurn (const cVehicle& v, const cMapView& view, std::vector<cPosition>& path)
{
	int budget = v.data.getSpeed();
	cPosition prev = v.getPosition();
	std::size_t n = 0;
	for (; n < path.size(); ++n)
	{
		const int c = cPathCalculator::calcNextCost (prev, path[n], &v, &view);
		if (c > budget) break;
		budget -= c;
		prev = path[n];
	}
	path.resize (n);
}

int dist2 (const cPosition& a, const cPosition& b)
{
	return (a - b).l2NormSquared();
}

// цена полёта по прямой (4 — шаг, 6 — по диагонали)
int flightCost (const cPosition& a, const cPosition& b)
{
	const int dx = std::abs (a.x() - b.x());
	const int dy = std::abs (a.y() - b.y());
	return 4 * std::max (dx, dy) + 2 * std::min (dx, dy);
}

nlohmann::json posJson (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

// имя по данным — маленькими буквами, пробел и дефис как «_» («Energy_Big», «energy small», «storage-gold»)
std::string norm (std::string s)
{
	for (auto& c : s)
	{
		if (c >= 'A' && c <= 'Z') c = static_cast<char> (c - 'A' + 'a');
		if (c == ' ' || c == '-') c = '_';
	}
	return s;
}

cPosition clampToMap (const cMap& map, cPosition p)
{
	const int n = map.getSize().x();
	return cPosition (std::clamp (p.x(), 1, n - 2), std::clamp (p.y(), 1, n - 2));
}

// точка на направлении d (0..7, по часовой от «восток») на расстоянии r от c
cPosition onDir (const cMap& map, const cPosition& c, int d, int r)
{
	const double a = kPi / 4.0 * ((d % 8 + 8) % 8);
	return clampToMap (map, c + cPosition (static_cast<int> (std::lround (r * std::cos (a))), static_cast<int> (std::lround (r * std::sin (a)))));
}

int dirOf (const cPosition& from, const cPosition& to)
{
	const cPosition d = to - from;
	if (d.x() == 0 && d.y() == 0) return 0;
	double a = std::atan2 (static_cast<double> (d.y()), static_cast<double> (d.x()));
	if (a < 0) a += 2 * kPi;
	return static_cast<int> (std::lround (a / (kPi / 4.0))) % 8;
}

bool isGround (const cMap& map, const cPosition& p)
{
	return map.isValidPosition (p) && !map.isWater (p) && !map.isCoast (p) && !map.isBlocked (p);
}

} // namespace

const char* toString (eRedScenario s)
{
	switch (s)
	{
		case eRedScenario::Rear: return "rear";
		case eRedScenario::Sea: return "sea";
		case eRedScenario::Subs: return "subs";
		default: return "combo";
	}
}

eRedScenario redScenarioFromEnv()
{
	const char* env = std::getenv ("MAXR_RED");
	const std::string v = env ? env : "";
	if (v == "rear") return eRedScenario::Rear;
	if (v == "sea") return eRedScenario::Sea;
	if (v == "subs") return eRedScenario::Subs;
	if (!v.empty() && v != "combo") Log.warn ("ai_red: MAXR_RED=" + v + " unknown (rear|sea|subs|combo), using combo");
	return eRedScenario::Combo;
}

std::vector<std::string> redLandingUnits (eRedScenario s)
{
	// стартовых кредитов 150 (eGameSettings: defaultCreditsNormal); цены — buildCost по данным
	switch (s)
	{
		case eRedScenario::Rear: // 18 + 30·2 + 9 + 18 = 105
			return {"awac", "bomber", "bomber", "commando", "air_transport"};
		case eRedScenario::Sea: // 12 + 12·3 + 24·2 = 96
			return {"sea_transport", "tank", "tank", "tank", "gunboat", "gunboat"};
		case eRedScenario::Subs: // 18·3 + 18 = 72
			return {"sub", "sub", "sub", "awac"};
		default: // combo: 18 + 9 + 18·2 + 30 + 12·3 = 129
			return {"awac", "commando", "air_transport", "air_transport", "bomber", "tank", "tank", "tank"};
	}
}

cRedBrain::cRedBrain (bool fair) :
	fair_ (fair)
{
	scenario_ = redScenarioFromEnv();
	if (const char* env = std::getenv ("MAXR_RED_ECO"); env != nullptr && *env != 0) eco_ = env;
	if (eco_ != "none" && eco_ != "base2")
	{
		Log.warn ("ai_red: MAXR_RED_ECO=" + eco_ + " unknown (none|base2), using none");
		eco_ = "none";
	}
	// честный налётчик ведёт своё хозяйство: замороженные правила base2 и свои заводы налёта
	if (fair_) eco_ = "base2";
	if (eco_ == "base2") ecoBrain_ = std::make_unique<base2::cRuleBrain>();
	if (const char* env = std::getenv ("MAXR_RED_START"); env != nullptr && *env != 0) startTurn_ = std::max (1, std::atoi (env));
}

cRedBrain::~cRedBrain() = default;

std::vector<nlohmann::json> cRedBrain::takeJournalEvents()
{
	auto ev = std::exchange (events_, {});
	if (ecoBrain_ && pass_ == 1)
		for (auto& e : ecoBrain_->takeJournalEvents()) ev.push_back (std::move (e));
	return ev;
}

void cRedBrain::stage (const std::string& name, bool ok, nlohmann::json details)
{
	if (ok && !stagesDone_.insert (name).second) return; // удачный этап — один раз
	details["event"] = "red_stage";
	details["scenario"] = toString (scenario_);
	details["stage"] = name;
	details["ok"] = ok;
	details["pass"] = pass_;
	Log.info ("ai_red: " + name + (ok ? " OK " : " FAIL ") + details.dump());
	events_.push_back (std::move (details));
}

void cRedBrain::stepEvent (const std::string& what, nlohmann::json details)
{
	details["event"] = "red_step";
	details["what"] = what;
	details["pass"] = pass_;
	events_.push_back (std::move (details));
}

cOrder cRedBrain::order (eOrderType type, unsigned int unitId, const std::string& reason) const
{
	cOrder o;
	o.type = type;
	o.playerId = playerId_;
	o.unitId = unitId;
	o.level = "red";
	o.reason = "red " + std::string (toString (scenario_)) + ": " + reason;
	return o;
}

const cVehicle* cRedBrain::own (const cModel& model, const std::string& type, int index) const
{
	const auto all = ownAll (model, type);
	return index < static_cast<int> (all.size()) ? all[index] : nullptr;
}

std::vector<const cVehicle*> cRedBrain::ownAll (const cModel& model, const std::string& type) const
{
	std::vector<const cVehicle*> out;
	for (const auto& [id, r] : raiders_)
	{
		if (r.lost || r.type != type) continue;
		if (const cVehicle* v = model.getVehicleFromID (id)) out.push_back (v);
	}
	return out; // raiders_ — по id, порядок детерминирован
}

// --- ход и заходы -------------------------------------------------------------------------
std::vector<cOrder> cRedBrain::decideTurn (int playerId, const cModel& model)
{
	turn_ = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	pass_ = 1;
	if (!begun_) begin (playerId, model);
	strikePhase_ = 0;
	if ((comboPhase_ >= 1 && comboPhase_ <= 4) || comboPhase_ == 10)
	{
		// удар оборвался на пределе заходов — подвести итог как есть и дальше свободный бой
		while (comboPasses_.size() < 2) comboPasses_.push_back (-1);
		comboPasses_.push_back (comboTankPass_);
		comboPhase_ = 6;
		comboReport_ = true;
	}
	auto out = act (playerId, model, 1);
	if (ecoBrain_)
	{
		// хозяйство — замороженные правила base2; машин и зданий налёта они не трогают
		for (auto& o : ecoBrain_->decideTurn (playerId, model))
		{
			const bool unitOrder = o.type != eOrderType::ChangeResearch && o.type != eOrderType::BuyUpgrades && o.type != eOrderType::EndTurn;
			// шаг 14б: «освободить площадку / выезд фабрики» — свою машину налёта без приказа base2 отогнать может
			// (иначе его стройка ждёт вечно: танки налёта стояли на площадке большого завода 43 хода, Delta)
			const bool shoo = o.type == eOrderType::Move && o.reason.find ("clear") != std::string::npos && owned (o.unitId)
				&& std::none_of (out.begin(), out.end(), [&] (const cOrder& x) { return x.unitId == o.unitId; });
			if (unitOrder && !shoo && (owned (o.unitId) || (o.unitId2 != kNoUnit && o.unitId2 != 0 && owned (o.unitId2)))) continue;
			// base2 пишет 0 как «нет цели» (до шага 13), а этот мозг — kNoUnit: перевести, как движок у base2
			if (o.unitId2 == 0 && (o.type == eOrderType::Attack || o.type == eOrderType::UpgradeVehicle
				|| (o.type == eOrderType::Move && o.moveEnd == eMoveEnd::Attack)))
				o.unitId2 = kNoUnit;
			out.push_back (std::move (o));
		}
	}
	return out;
}

std::vector<cOrder> cRedBrain::decidePass (int playerId, const cModel& model, int pass)
{
	pass_ = pass;
	return act (playerId, model, pass);
}

void cRedBrain::begin (int playerId, const cModel& model)
{
	begun_ = true;
	playerId_ = playerId;
	const cPlayer* me = model.getPlayer (playerId);
	for (const auto& p : model.getPlayerList())
		if (p->getId() != playerId && enemyId_ < 0) enemyId_ = p->getId();
	home_ = me->getLandingPos();
	if (fair_)
		fairBegin (model); // база врага неизвестна; свои — купленное при высадке
	else
	{
		const cPlayer* enemy = enemyId_ >= 0 ? model.getPlayer (enemyId_) : nullptr;
		base_ = enemy ? enemy->getLandingPos() : home_;
		const auto types = redLandingUnits (scenario_);
		for (const auto& v : me->getVehicles())
		{
			const std::string t = v->getStaticUnitData().getDefaultName();
			if (std::find (types.begin(), types.end(), t) != types.end()) raiders_[v->getId()] = {v->getId(), t, turn_, false};
		}
	}
	nlohmann::json units = nlohmann::json::array();
	for (const auto& [id, r] : raiders_)
	{
		units.push_back ({{"id", id}, {"type", r.type}});
		if (r.type == "air_transport") airTransports_.push_back (id);
	}
	stage ("start", true, {{"units", units}, {"home", posJson (home_)}, {"enemyBase", fair_ ? nlohmann::json() : posJson (base_)},
		{"eco", eco_}, {"startTurn", startTurn_}, {"enemy", enemyId_}, {"mode", fair_ ? "fair" : "crash"}, {"brain", name()}});
}

std::vector<cOrder> cRedBrain::act (int playerId, const cModel& model, int pass)
{
	std::vector<cOrder> out;
	wantPass_ = false;
	reserved_.clear();
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return out;
	cMapView view (model.getMap(), sharedPlayerOf (model, playerId));
	updateMemory (model, view);
	checkShots (model);
	if (pass == 1) checkLosses (model);
	airStepCut_ = false;
	std::vector<cOrder> svc; // red2: обслуживание самолётов в ангаре — поверх приказов сценария
	if (fair_)
	{
		// честный режим: хозяйство налёта, разведка, ангар; сценарий — когда база найдена и отряд собран
		if (pass == 1)
		{
			claimNew (model);
			builderStep (model, view, out);
			engineerStep (model, view, out);
			produce (model, out);
			checkForce (model);
			explore (model, view, out);
		}
		service (model, view, svc);
		if (!baseKnown_ || !forceReady_ || turn_ < startTurn_)
		{
			// отряд ждёт дома (танки охраняют базу); AWAC, разведав базу, возвращается (облёт — когда отряд собран)
			guardHome (model, view, out);
			if (const cVehicle* a = own (model, "awac"); a != nullptr && baseKnown_ && pass == 1 && service_.count (a->getId()) == 0
				&& !a->isUnitLoaded() && !a->isUnitMoving() && dist2 (a->getPosition(), home_) > 36)
				moveToward (model, view, *a, home_, out, "awac: base found, wait at home for the strike force", true);
			for (auto& o : svc) out.push_back (std::move (o));
			return out;
		}
	}
	else if (turn_ < startTurn_)
		return out;
	switch (scenario_)
	{
		case eRedScenario::Rear: rear (model, view, pass, out); break;
		case eRedScenario::Sea: sea (model, view, pass, out); break;
		case eRedScenario::Subs: subs (model, view, pass, out); break;
		case eRedScenario::Combo: combo (model, view, pass, out); break;
	}
	if (!service_.empty())
		out.erase (std::remove_if (out.begin(), out.end(), [&] (const cOrder& o) { return service_.count (o.unitId) != 0; }), out.end());
	for (auto& o : svc) out.push_back (std::move (o));
	if (airStepCut_ && !out.empty()) wantPass_ = true; // самолёт у базы врага летит дальше в следующем заходе
	return out;
}

// --- что видно и что помним ------------------------------------------------------------------
void cRedBrain::updateMemory (const cModel& model, const cMapView& view)
{
	const auto remember = [&] (const cUnit& u) {
		if (u.isABuilding() && static_cast<const cBuilding&> (u).isRubble()) return;
		if (!view.canSeeUnit (u)) return;
		const auto& sd = u.getStaticUnitData();
		sSeen s;
		s.pos = u.getPosition();
		s.type = norm (sd.getDefaultName());
		s.building = u.isABuilding();
		s.big = u.getIsBig();
		const bool hasAmmo = sd.canAttack != 0 && u.data.getAmmoMax() > 0;
		s.aa = hasAmmo && (sd.canAttack & eTerrainFlag::Air) != 0;
		s.armedGround = hasAmmo && (sd.canAttack & eTerrainFlag::Ground) != 0;
		s.armedSea = hasAmmo && (sd.canAttack & eTerrainFlag::Sea) != 0;
		s.naval = sd.factorSea > 0 && sd.factorGround <= 0 && sd.factorAir <= 0 && u.data.getBuildCost() > 2;
		s.range = u.data.getRange();
		s.cost = u.data.getBuildCost();
		s.seenTurn = turn_;
		s.disabled = u.isDisabled();
		s.visible = true;
		seen_[u.getId()] = s;
	};
	for (auto& [id, s] : seen_) s.visible = false;
	for (const auto& p : model.getPlayerList())
	{
		if (p->getId() == playerId_) continue;
		for (const auto& v : p->getVehicles())
			if (!v->isUnitLoaded()) remember (*v);
		for (const auto& b : p->getBuildings()) remember (*b);
	}
	for (auto it = seen_.begin(); it != seen_.end();)
	{
		const cUnit* u = model.getUnitFromID (it->first);
		bool drop = false;
		if (!fair_)
		{
			drop = u == nullptr; // уничтожен (упрощение red1: налётчик узнаёт об этом сразу)
			if (!drop && !view.canSeeUnit (*u) && !it->second.building)
				drop = view.isPositionVisible (it->second.pos) || turn_ - it->second.seenTurn > 6; // уехал
		}
		else if (!it->second.visible)
			// red2 — как игрок: пропажу видно, только когда клетка снова в обзоре; машина вне обзора — уехала
			drop = view.isPositionVisible (it->second.pos) || (!it->second.building && turn_ - it->second.seenTurn > 6);
		it = drop ? seen_.erase (it) : std::next (it);
	}
	// база врага: середина виденных зданий (без дорог и стен), пока не видно — его точка высадки
	long long sx = 0, sy = 0, n = 0;
	for (const auto& [id, s] : seen_)
		if (s.building && s.cost > 2)
		{
			sx += s.pos.x();
			sy += s.pos.y();
			++n;
		}
	if (n > 0)
	{
		base_ = cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n));
		if (fair_ && !baseKnown_)
		{
			baseKnown_ = true;
			stage ("base_found", true, {{"base", posJson (base_)}, {"buildingsSeen", n}, {"turn", turn_}});
		}
	}
}

bool cRedBrain::airDanger (const cPosition& p) const
{
	for (const auto& [id, s] : seen_)
		if (s.aa && !s.disabled && dist2 (p, s.pos) <= (s.range + 1) * (s.range + 1)) return true;
	return false;
}

bool cRedBrain::groundDanger (const cPosition& p, int margin) const
{
	for (const auto& [id, s] : seen_)
		if (s.armedGround && !s.disabled && dist2 (p, s.pos) <= (s.range + margin) * (s.range + margin)) return true;
	return false;
}

int cRedBrain::targetValue (const sSeen& s, int mode) const
{
	const std::string& t = s.type;
	if (mode == 2) // флот: подлодки
	{
		if (!s.naval) return 0;
		if (t == "corvet") return 100; // видит подлодки — первым
		if (t == "sea_transport" || t == "cargoship") return 95;
		if (t == "shipfactory") return 90;
		if (t == "dock") return 85;
		if (t == "gunboat" || t == "missel_ship" || t == "escort") return 70;
		return 50;
	}
	if (mode == 1) // оборона
	{
		if (s.disabled) return 5;
		if (t == "missel") return 100;
		if (t == "rocketlauncher") return 95;
		if (s.aa) return 90;
		if (t == "artillerie") return 85;
		if (t == "cluster") return 80;
		if (t == "assault") return 75;
		if (t == "turret") return 70;
		if (t == "tank") return 65;
		if (s.armedGround || s.armedSea) return 50;
		return targetValue (s, 0) / 3; // хозяйство — после обороны
	}
	if (!s.building) return s.armedGround ? 15 : 10;
	if (t == "mine") return 100;
	if (t == "energy_big") return 90;
	if (t == "energy_small") return 70;
	if (t == "radar") return 65;
	if (t.find ("factory") != std::string::npos) return 60;
	if (t == "research" || t == "goldraffinery") return 50;
	if (t == "connector") return 45;
	if (s.cost <= 2) return 3; // дороги, стены, мосты, платформы
	return 30;
}

// --- итог выстрелов прошлого захода ---------------------------------------------------------
void cRedBrain::checkShots (const cModel& model)
{
	const cPlayer* enemy = enemyId_ >= 0 ? model.getPlayer (enemyId_) : nullptr;
	for (const auto& s : std::exchange (shots_, {}))
	{
		const cUnit* t = model.getUnitFromID (s.target);
		const bool destroyed = t == nullptr;
		const int hp = t ? t->data.getHitpoints() : 0;
		nlohmann::json d = {{"unit", s.unit}, {"target", s.target}, {"targetType", s.targetType}, {"hpBefore", s.hpBefore},
			{"hpAfter", hp}, {"destroyed", destroyed}, {"turn", turn_}};
		if (s.what == "disable" || s.what == "combo_disable")
		{
			const cUnit* cmd = model.getUnitFromID (s.unit);
			const bool ok = t != nullptr && t->isDisabled();
			d["disabled"] = ok;
			d["disabledTurns"] = t ? t->getDisabledTurns() : 0;
			d["commandoDetected"] = cmd != nullptr && enemy != nullptr && cmd->isDetectedByPlayer (enemy);
			d["commandoAlive"] = cmd != nullptr;
			stepEvent (s.what + "_result", d);
			if (ok)
			{
				cmdDisabledTurn_ = turn_;
				if (auto it = seen_.find (s.target); it != seen_.end()) it->second.disabled = true;
				stage ("commando_disable", true, d);
			}
			if (s.what == "combo_disable") comboDisableOk_ = ok;
			continue;
		}
		const bool hit = destroyed || hp < s.hpBefore;
		d["hit"] = hit;
		stepEvent (s.what + "_result", d);
		if (!hit) continue;
		if (s.what == "bomb") stage ("bomb", true, d);
		else if (s.what == "combo_bomb") comboBombOk_ = true;
		else if (s.what == "combo_tank") comboTankOk_ = true;
		else if (s.what == "gunboat") stage ("shore_bombard", true, d);
		else if (s.what == "tank") stage ("tanks_attack", true, d);
		else if (s.what == "sub") stage ("sub_hit", true, d);
	}
}

void cRedBrain::checkLosses (const cModel& model)
{
	for (auto& [id, r] : raiders_)
	{
		if (r.lost) continue;
		if (model.getVehicleFromID (id) == nullptr)
		{
			r.lost = true;
			stepEvent ("lost", {{"unit", id}, {"type", r.type}, {"turn", turn_}});
		}
	}
}

// --- движение ------------------------------------------------------------------------------
bool cRedBrain::moveToward (const cModel& model, const cMapView& view, const cVehicle& v, const cPosition& destIn, std::vector<cOrder>& out,
	const std::string& reason, bool keepAirSafe)
{
	if (v.isUnitMoving() || v.isUnitLoaded() || v.isDisabled() || v.data.getSpeed() <= 0) return false;
	const cMap& map = *model.getMap();
	const bool plane = v.getStaticUnitData().factorAir > 0;
	cPosition dest = clampToMap (map, destIn);
	if (dest == v.getPosition()) return false;
	// самолёт: в обход зон ПВО — через точку облёта, если прямая идёт через зону
	if (plane && keepAirSafe)
	{
		const auto lineSafe = [&] (const cPosition& a, const cPosition& b) {
			const int steps = std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
			for (int i = 1; i <= steps; ++i)
			{
				const cPosition c (a.x() + (b.x() - a.x()) * i / steps, a.y() + (b.y() - a.y()) * i / steps);
				if (airDanger (c)) return false;
			}
			return true;
		};
		if (airDanger (dest))
		{
			// цель в зоне — ближайшая безопасная клетка на луче от базы
			const int d = dirOf (base_, dest);
			const int r0 = static_cast<int> (std::sqrt (static_cast<double> (dist2 (base_, dest))));
			for (int r = r0 + 1; r <= r0 + 12; ++r)
			{
				const cPosition c = onDir (map, base_, d, r);
				if (!airDanger (c))
				{
					dest = c;
					break;
				}
			}
		}
		if (!lineSafe (v.getPosition(), dest))
		{
			int best = std::numeric_limits<int>::max();
			cPosition via = dest;
			for (int r : {16, 20, 24, 28})
				for (int d = 0; d < 8; ++d)
				{
					const cPosition w = onDir (map, base_, d, r);
					if (airDanger (w) || !lineSafe (v.getPosition(), w) || !lineSafe (w, dest)) continue;
					const int c = flightCost (v.getPosition(), w) + flightCost (w, dest);
					if (c < best)
					{
						best = c;
						via = w;
					}
				}
			if (via != v.getPosition()) dest = via;
		}
	}
	auto path = toVector (cPathCalculator (v, view, dest, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
	if (path.empty()) // клетка занята — ближайшая свободная рядом
		for (int r = 1; r <= 3 && path.empty(); ++r)
			for (int dy = -r; dy <= r && path.empty(); ++dy)
				for (int dx = -r; dx <= r && path.empty(); ++dx)
				{
					const cPosition c = dest + cPosition (dx, dy);
					if (!map.isValidPosition (c) || !view.possiblePlace (v, c)) continue;
					if (plane && keepAirSafe && airDanger (c)) continue;
					path = toVector (cPathCalculator (v, view, c, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
				}
	cutToTurn (v, view, path);
	if (plane && keepAirSafe)
	{
		// не входить в зону ПВО по дороге: остановиться перед ней
		for (std::size_t i = 0; i < path.size(); ++i)
			if (airDanger (path[i]))
			{
				path.resize (i);
				break;
			}
		// у базы врага — по 3 клетки за заход: обзор AWAC 12 = досягаемость зенитки 8 + запас 1 + шаг 3, так зенитку
		// видно раньше, чем самолёт войдёт в её огонь (ответный огонь бьёт и по летящему в свой ход)
		int inside = 0;
		for (std::size_t i = 0; i < path.size() && (!fair_ || baseKnown_); ++i)
			if (dist2 (path[i], base_) <= 24 * 24 && ++inside > 3)
			{
				path.resize (i);
				airStepCut_ = true;
				break;
			}
	}
	while (!path.empty() && reserved_.count ({path.back().x(), path.back().y()}) != 0) path.pop_back();
	if (path.empty()) return false;
	cOrder o = order (eOrderType::Move, v.getId(), reason);
	o.path = path;
	o.position = path.back();
	out.push_back (std::move (o));
	reserved_.insert ({path.back().x(), path.back().y()});
	return true;
}

// выстрел с места по лучшей видимой цели в досягаемости
bool cRedBrain::fireInRange (const cModel& model, const cMapView& view, const cVehicle& v, int mode, std::vector<cOrder>& out, const std::string& what)
{
	if (v.isUnitMoving() || v.isUnitLoaded() || v.isDisabled() || v.data.getShots() <= 0 || v.data.getAmmo() <= 0) return false;
	int bestValue = 0;
	unsigned int bestId = kNoUnit;
	cPosition bestCell;
	for (const auto& [id, s] : seen_)
	{
		const cUnit* u = model.getUnitFromID (id);
		if (u == nullptr || !view.canSeeUnit (*u)) continue;
		const int value = targetValue (s, mode) + (s.armedGround && mode != 2 ? 20 : 0);
		if (value <= bestValue) continue;
		for (const auto& cell : u->getPositions())
			if (v.canAttackObjectAt (cell, view) && cAttackJob::selectTarget (cell, v.getStaticUnitData().canAttack, view, v.getOwner()) == u)
			{
				bestValue = value;
				bestId = id;
				bestCell = cell;
				break;
			}
	}
	if (bestValue <= 0) return false;
	const cUnit* t = model.getUnitFromID (bestId);
	cOrder o = order (eOrderType::Attack, v.getId(), what + ": fire at " + seen_[bestId].type);
	o.position = bestCell;
	out.push_back (std::move (o));
	shots_.push_back ({v.getId(), bestId, seen_[bestId].type, t->data.getHitpoints(), what});
	stepEvent (what, {{"unit", v.getId()}, {"unitType", v.getStaticUnitData().getDefaultName()}, {"target", bestId},
		{"targetType", seen_[bestId].type}, {"at", posJson (bestCell)}, {"from", posJson (v.getPosition())}, {"turn", turn_}});
	return true;
}

// бомбардировщик: лучшая цель, у которой есть клетка удара вне зон ПВО; подлететь и ударить в конце пути
bool cRedBrain::bomberStrike (const cModel& model, const cMapView& view, const cVehicle& b, int mode, std::vector<cOrder>& out, const std::string& what)
{
	if (b.isUnitMoving() || b.isUnitLoaded() || b.isDisabled() || b.data.getShots() <= 0 || b.data.getAmmo() <= 0) return false;
	const cMap& map = *model.getMap();
	const int range = b.data.getRange();
	const int speed = b.data.getSpeed();
	struct sCand
	{
		int value;
		int d;
		unsigned int id;
	};
	std::vector<sCand> cands;
	for (const auto& [id, s] : seen_)
	{
		const int v = targetValue (s, mode);
		if (v > 3) cands.push_back ({v, dist2 (b.getPosition(), s.pos), id});
	}
	std::sort (cands.begin(), cands.end(), [] (const sCand& a, const sCand& c) {
		return a.value != c.value ? a.value > c.value : (a.d != c.d ? a.d < c.d : a.id < c.id);
	});
	for (const auto& c : cands)
	{
		const sSeen& s = seen_[c.id];
		const cUnit* u = model.getUnitFromID (c.id);
		if (u == nullptr) continue;
		if (fair_ && !s.building && !s.visible) continue; // red2: где машина вне обзора — не знает
		int bestCost = std::numeric_limits<int>::max();
		cPosition fire;
		cPosition aim;
		for (int dy = -range - 1; dy <= range + 1; ++dy)
			for (int dx = -range - 1; dx <= range + 1; ++dx)
			{
				const cPosition f = s.pos + cPosition (dx, dy);
				if (!map.isValidPosition (f) || airDanger (f) || reserved_.count ({f.x(), f.y()}) != 0) continue;
				// выстрел в конце пути бьёт по клетке цели (у здания 2×2 — левая верхняя)
				const cPosition* hit = dist2 (f, u->getPosition()) <= range * range ? &u->getPosition() : nullptr;
				if (hit == nullptr) continue;
				if (f != b.getPosition() && !view.possiblePlace (b, f)) continue;
				const int cost = flightCost (b.getPosition(), f);
				if (cost > speed - 24) continue; // на отход — не меньше 6 клеток хода
				if (cost < bestCost)
				{
					bestCost = cost;
					fire = f;
					aim = *hit;
				}
			}
		if (bestCost == std::numeric_limits<int>::max()) continue;
		if (fire == b.getPosition())
		{
			if (!b.canAttackObjectAt (aim, view)) continue;
			cOrder o = order (eOrderType::Attack, b.getId(), what + ": bomb " + s.type);
			o.position = aim;
			out.push_back (std::move (o));
		}
		else
		{
			auto path = toVector (cPathCalculator (b, view, fire, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
			const std::size_t full = path.size();
			cutToTurn (b, view, path);
			if (path.empty() || path.size() != full) continue;
			bool safe = true;
			for (const auto& p : path) safe = safe && !airDanger (p);
			if (!safe) continue;
			cOrder o = order (eOrderType::Move, b.getId(), what + ": fly in and bomb " + s.type);
			o.path = path;
			o.position = path.back();
			o.unitId2 = c.id; // выстрел в конце пути (шаг 14б: «нет цели» — kNoUnit, id 0 — настоящая цель)
			o.moveEnd = eMoveEnd::Attack;
			out.push_back (std::move (o));
		}
		reserved_.insert ({fire.x(), fire.y()});
		shots_.push_back ({b.getId(), c.id, s.type, u->data.getHitpoints(), what});
		stepEvent (what, {{"unit", b.getId()}, {"target", c.id}, {"targetType", s.type}, {"fireFrom", posJson (fire)},
			{"from", posJson (b.getPosition())}, {"value", c.value}, {"turn", turn_}});
		return true;
	}
	return false;
}

// танк: выстрел с места, иначе подъехать к ближайшей цели и выстрелить в конце пути
bool cRedBrain::tankAssault (const cModel& model, const cMapView& view, const cVehicle& t, std::vector<cOrder>& out)
{
	if (fireInRange (model, view, t, 1, out, "tank")) return true;
	if (t.isUnitMoving() || t.isUnitLoaded() || t.isDisabled() || t.data.getSpeed() <= 0) return false;
	// цель: вооружённая ближе 10 клеток, иначе ближайшее здание с ценностью
	struct sCand
	{
		int key;
		unsigned int id;
	};
	std::vector<sCand> cands;
	for (const auto& [id, s] : seen_)
	{
		const int d = dist2 (t.getPosition(), s.pos);
		if (s.armedGround && !s.disabled && d <= 100) cands.push_back ({d, id});
		else if (s.building && targetValue (s, 0) > 3 && s.type != "connector") cands.push_back ({10000 + d, id});
	}
	std::sort (cands.begin(), cands.end(), [] (const sCand& a, const sCand& b) { return a.key != b.key ? a.key < b.key : a.id < b.id; });
	int tries = 0;
	for (const auto& c : cands)
	{
		if (++tries > 4) break;
		const sSeen& s = seen_[c.id];
		auto path = toVector (cPathCalculator (t, view, s.pos, true).calcPath());
		if (path.empty()) continue;
		const std::size_t full = path.size();
		cutToTurn (t, view, path);
		while (!path.empty() && reserved_.count ({path.back().x(), path.back().y()}) != 0) path.pop_back();
		if (path.empty()) return false;
		cOrder o = order (eOrderType::Move, t.getId(), "assault " + s.type);
		o.path = path;
		o.position = path.back();
		if (path.size() == full)
		{
			o.unitId2 = c.id;
			o.moveEnd = eMoveEnd::Attack;
			if (const cUnit* u = model.getUnitFromID (c.id)) shots_.push_back ({t.getId(), c.id, s.type, u->data.getHitpoints(), "tank"});
		}
		reserved_.insert ({path.back().x(), path.back().y()});
		out.push_back (std::move (o));
		stepEvent ("tank_move", {{"unit", t.getId()}, {"target", c.id}, {"targetType", s.type}, {"to", posJson (path.back())},
			{"attackAtEnd", path.size() == full}, {"turn", turn_}});
		return true;
	}
	return false;
}

// --- AWAC: облёт базы, выбор стороны -------------------------------------------------------
void cRedBrain::awacScout (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cVehicle* a = own (model, "awac");
	if (a == nullptr || a->isUnitMoving() || a->data.getSpeed() <= 0) return;
	(void) pass;
	const cMap& map = *model.getMap();
	if (side_ >= 0)
	{
		// сторона выбрана — висеть над краем базы с этой стороны, вне ПВО: обзор для удара
		moveToward (model, view, *a, onDir (map, base_, side_, kRingRadius + 1), out, "awac: watch the strike side", true);
		return;
	}
	if (awacRing_ < 0) awacRing_ = dirOf (base_, a->getPosition());
	const cPosition p = onDir (map, base_, awacRing_, kRingRadius);
	const bool there = dist2 (a->getPosition(), p) <= 4 || airDanger (p);
	if (there)
	{
		++awacVisited_;
		stepEvent ("awac_ring", {{"dir", awacRing_}, {"at", posJson (a->getPosition())}, {"danger", airDanger (p)}, {"seen", seen_.size()}, {"turn", turn_}});
		awacRing_ = (awacRing_ + 1) % 8;
	}
	if (awacVisited_ >= 4)
	{
		chooseSide (model, view);
		return;
	}
	if (!moveToward (model, view, *a, onDir (map, base_, awacRing_, kRingRadius), out, "awac: circle the enemy base", true))
	{
		++awacVisited_; // не долететь — следующая точка
		awacRing_ = (awacRing_ + 1) % 8;
	}
}

void cRedBrain::chooseSide (const cModel& model, const cMapView& /*view*/)
{
	const cMap& map = *model.getMap();
	const int homeDir = dirOf (base_, home_);
	std::array<int, 8> score{};
	std::array<int, 8> aaNear{};
	int buildings = 0, aa = 0;
	for (const auto& [id, s] : seen_)
	{
		buildings += s.building ? 1 : 0;
		aa += s.aa ? 1 : 0;
	}
	for (int d = 0; d < 8; ++d)
	{
		const cPosition st = onDir (map, base_, d, kStagingRadius);
		for (const auto& [id, s] : seen_)
		{
			if (s.aa && dist2 (s.pos, st) <= 18 * 18) ++aaNear[d];
			const int v = scenario_ == eRedScenario::Combo ? targetValue (s, 1) : targetValue (s, 0);
			if (v <= 3) continue;
			// есть клетка удара (дальность 4) вне ПВО в 9 клетках от стоянки
			bool ok = false;
			for (int dy = -4; dy <= 4 && !ok; ++dy)
				for (int dx = -4; dx <= 4 && !ok; ++dx)
				{
					const cPosition f = s.pos + cPosition (dx, dy);
					ok = dx * dx + dy * dy <= 16 && map.isValidPosition (f) && !airDanger (f) && flightCost (st, f) <= 36;
				}
			if (ok) score[d] += v;
		}
		if (airDanger (st)) score[d] = -1;
	}
	int best = -1;
	for (int k = 0; k < 8; ++k)
	{
		// при равенстве — сторона дальше от дома (rear, «с тыла») или к дому (combo: танкам ближе)
		const int d = scenario_ == eRedScenario::Combo ? (homeDir + (k % 2 == 0 ? k / 2 : 8 - (k + 1) / 2)) % 8 : (homeDir + 4 + (k % 2 == 0 ? k / 2 : 8 - (k + 1) / 2)) % 8;
		if (scenario_ == eRedScenario::Combo && k >= 3) break; // combo: к дому и ±45°
		if (best < 0 || score[d] > score[best] || (score[d] == score[best] && aaNear[d] < aaNear[best])) best = d;
	}
	side_ = best < 0 ? (homeDir + 4) % 8 : best;
	staging_ = onDir (map, base_, side_, kStagingRadius);
	tankStaging_ = onDir (map, base_, side_, kTankStagingRadius);
	nlohmann::json sc = nlohmann::json::array();
	for (int d = 0; d < 8; ++d) sc.push_back ({{"dir", d}, {"score", score[d]}, {"aa", aaNear[d]}});
	stage ("awac_scout", buildings > 0, {{"buildingsSeen", buildings}, {"aaSeen", aa}, {"ringPoints", awacVisited_}, {"turn", turn_}});
	stage ("side_chosen", true, {{"side", side_}, {"homeDir", homeDir}, {"staging", posJson (staging_)}, {"base", posJson (base_)},
		{"scores", sc}, {"turn", turn_}});
}

// --- коммандос -----------------------------------------------------------------------------
bool cRedBrain::pickCommandoTarget (const cModel& model)
{
	const cPosition side = side_ >= 0 ? onDir (*model.getMap(), base_, side_, 8) : base_;
	long long best = std::numeric_limits<long long>::max();
	unsigned int pick = kNoUnit;
	bool found = false;
	for (int tier = 0; tier < 3 && !found; ++tier)
		for (const auto& [id, s] : seen_)
		{
			const cUnit* u = model.getUnitFromID (id);
			if (u == nullptr || !u->getStaticUnitData().canBeDisabled || s.disabled || cmdUnreachable_.count (id) != 0) continue;
			if (u->isAVehicle() && u->getStaticUnitData().factorAir > 0) continue;
			if (fair_ && u->isAVehicle() && !s.visible) continue; // red2: где машина сейчас — не знает
			bool fits = false;
			if (tier == 0) fits = s.aa; // зенитка
			else if (tier == 1) fits = s.type == "radar" || (scenario_ == eRedScenario::Combo && s.armedGround && s.building);
			else fits = s.type == "mine";
			if (!fits) continue;
			const long long key = static_cast<long long> (dist2 (s.pos, side)) * 100000 + id;
			if (key < best)
			{
				best = key;
				pick = id;
				found = true;
			}
		}
	if (found && (!cmdHasTarget_ || pick != cmdTarget_))
		stepEvent ("commando_target", {{"target", pick}, {"targetType", seen_[pick].type}, {"at", posJson (seen_[pick].pos)}, {"turn", turn_}});
	cmdHasTarget_ = found;
	cmdTarget_ = pick;
	return found;
}

const cVehicle* cRedBrain::transport (const cModel& model, int which) const
{
	if (which >= static_cast<int> (airTransports_.size())) return nullptr;
	return model.getVehicleFromID (airTransports_[which]);
}

// авиатранспорт: забрать коммандос, довезти до клетки рядом с целью, выгрузить
void cRedBrain::ferryCommando (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cVehicle* c = own (model, "commando");
	const cVehicle* t = transport (model, 0);
	const cMap& map = *model.getMap();
	if (c == nullptr) return;
	if (t == nullptr && ferryState_ < 2) ferryState_ = 2; // транспорта нет — идёт сам
	if (ferryState_ == 0)
	{
		if (c->isUnitLoaded())
		{
			ferryState_ = 1;
			stage ("commando_boarded", true, {{"commando", c->getId()}, {"transport", t->getId()}, {"turn", turn_}});
		}
		else if (pass == 1 || t->data.getSpeed() > 0)
		{
			if (t->canLoad (c))
			{
				cOrder o = order (eOrderType::Load, t->getId(), "air transport takes the commando");
				o.unitId2 = c->getId();
				out.push_back (std::move (o));
				wantPass_ = true;
			}
			else if (!t->isUnitMoving() && t->data.getSpeed() > 0)
			{
				auto path = toVector (cPathCalculator (*t, view, c->getPosition(), static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
				const std::size_t full = path.size();
				cutToTurn (*t, view, path);
				if (!path.empty())
				{
					cOrder o = order (eOrderType::Move, t->getId(), "air transport flies to the commando");
					o.path = path;
					o.position = path.back();
					if (path.size() == full)
					{
						o.unitId2 = c->getId();
						o.moveEnd = eMoveEnd::Load;
					}
					out.push_back (std::move (o));
					wantPass_ = true;
				}
			}
		}
		return;
	}
	if (ferryState_ == 1)
	{
		if (!c->isUnitLoaded())
		{
			ferryState_ = 2;
			return;
		}
		if (!cmdHasTarget_ && side_ >= 0) pickCommandoTarget (model);
		if (!cmdHasTarget_) return; // ждать цель
		const sSeen& tg = seen_[cmdTarget_];
		if (dropAt_ == cPosition (0, 0) || airDanger (dropAt_))
		{
			// клетка высадки: поближе к цели (от 2 клеток), вне ПВО (цель-зенитка — за её досягаемостью), коммандос там встанет
			int best = std::numeric_limits<int>::max();
			for (int dy = -12; dy <= 12; ++dy)
				for (int dx = -12; dx <= 12; ++dx)
				{
					const cPosition p = tg.pos + cPosition (dx, dy);
					const int d = dx * dx + dy * dy;
					if (d < 4 || d > 144 || !map.isValidPosition (p) || airDanger (p)) continue;
					if (!map.possiblePlaceVehicle (c->getStaticUnitData(), p, nullptr)) continue;
					const int key = d * 1000 + flightCost (t->getPosition(), p);
					if (key < best)
					{
						best = key;
						dropAt_ = p;
					}
				}
			if (best == std::numeric_limits<int>::max()) dropAt_ = tg.pos; // некуда — к цели, коммандос дойдёт сам
		}
		if (t->getPosition() == dropAt_)
		{
			if (t->canExitTo (t->getPosition(), map, c->getStaticUnitData()))
			{
				cOrder o = order (eOrderType::ActivateUnit, t->getId(), "drop the commando");
				o.unitId2 = c->getId();
				o.position = t->getPosition();
				out.push_back (std::move (o));
				ferryState_ = 2;
				wantPass_ = true;
				stage ("commando_dropped", true, {{"at", posJson (t->getPosition())}, {"target", cmdTarget_}, {"targetType", tg.type},
					{"targetAt", posJson (tg.pos)}, {"turn", turn_}});
				return;
			}
			dropAt_ = cPosition (0, 0); // клетка занята — выбрать другую
		}
		if (moveToward (model, view, *t, dropAt_, out, "air transport carries the commando", true)) wantPass_ = true;
		return;
	}
	// транспорт выгрузил — домой
	if (t != nullptr && ferryState_ == 2 && pass == 1 && !t->isUnitMoving())
	{
		if (t->getPosition() == home_ || dist2 (t->getPosition(), home_) <= 9) ferryState_ = 3;
		else moveToward (model, view, *t, home_, out, "air transport returns home", true);
	}
}

// второй авиатранспорт (combo): взять танки дома и ждать с ними на стоянке самолётов; в ход удара — tankDrop
void cRedBrain::ferryTanks (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cVehicle* t = transport (model, 1);
	const auto tanks = ownAll (model, "tank");
	if (tankFerry_ == 9) return;
	if (t == nullptr)
	{
		bool inside = false;
		for (const cVehicle* k : tanks) inside = inside || k->isUnitLoaded();
		stage ("tanks_ferry", false, {{"why", "нет авиатранспорта для танков"}, {"tanksInside", inside}, {"turn", turn_}});
		tankFerry_ = 9; // едут сами
		return;
	}
	if (tankFerry_ == 0)
	{
		const cVehicle* next = nullptr;
		int inside = 0;
		for (const cVehicle* k : tanks)
			if (k->isUnitLoaded()) ++inside;
			else if (next == nullptr) next = k;
		if (next == nullptr || inside >= t->getStaticUnitData().storageUnitsMax)
		{
			tankFerry_ = 1;
			stage ("tanks_boarded", inside > 0, {{"tanks", inside}, {"turn", turn_}});
		}
		else
		{
			if (t->canLoad (next))
			{
				cOrder o = order (eOrderType::Load, t->getId(), "air transport takes a tank");
				o.unitId2 = next->getId();
				out.push_back (std::move (o));
				wantPass_ = true;
			}
			else if (!t->isUnitMoving() && t->data.getSpeed() > 0)
			{
				auto path = toVector (cPathCalculator (*t, view, next->getPosition(), static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
				const std::size_t full = path.size();
				cutToTurn (*t, view, path);
				if (!path.empty())
				{
					cOrder o = order (eOrderType::Move, t->getId(), "air transport flies to a tank");
					o.path = path;
					o.position = path.back();
					if (path.size() == full)
					{
						o.unitId2 = next->getId();
						o.moveEnd = eMoveEnd::Load;
					}
					out.push_back (std::move (o));
					wantPass_ = true;
				}
			}
			return;
		}
	}
	if (tankFerry_ == 1 && side_ >= 0 && comboPhase_ == 0)
	{
		// с танками — на стоянку самолётов (вне обзора и ПВО врага), ждать хода удара
		if (dist2 (t->getPosition(), staging_) > 9) moveToward (model, view, *t, staging_, out, "air transport with tanks to the staging point", true);
	}
	if (tankFerry_ == 3 && pass == 1 && side_ >= 0) // пуст — на стоянку
		if (dist2 (t->getPosition(), staging_) > 9) moveToward (model, view, *t, staging_, out, "air transport back to the staging point", true);
}

// ход удара: долететь до клетки высадки у обороны врага и выгружать танки по одному (выгрузка не тратит их ход);
// true — приказ отдан
bool cRedBrain::tankDrop (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	const cMap& map = *model.getMap();
	const cVehicle* t = transport (model, 1);
	if (t == nullptr || t->isUnitMoving()) return false;
	const cVehicle* cargo = nullptr;
	for (const cVehicle* k : ownAll (model, "tank"))
		if (k->isUnitLoaded() && std::find (t->storedUnits.begin(), t->storedUnits.end(), k) != t->storedUnits.end())
		{
			cargo = k;
			break;
		}
	if (cargo == nullptr)
	{
		if (tankFerry_ < 3)
		{
			tankFerry_ = 3;
			stage ("tanks_dropped", true, {{"at", posJson (t->getPosition())}, {"turn", turn_}, {"pass", pass_}});
		}
		return false;
	}
	tankFerry_ = 2;
	const auto freeGround = [&] (const cPosition& c) {
		return isGround (map, c) && reserved_.count ({c.x(), c.y()}) == 0 && view.possiblePlaceVehicle (cargo->getStaticUnitData(), c)
			&& (c == t->getPosition() || view.possiblePlace (*t, c));
	};
	if (!dropChosen_)
	{
		// клетка высадки: в 3–5 клетках от ближайшей цели обороны (иначе — от середины базы), вне ПВО
		cPosition aim = base_;
		long long best = std::numeric_limits<long long>::max();
		for (const auto& [id, s] : seen_)
		{
			const int v = targetValue (s, 1);
			if (v < 50) continue;
			const long long key = static_cast<long long> (dist2 (s.pos, staging_)) * 1000 - v;
			if (key < best)
			{
				best = key;
				aim = s.pos;
			}
		}
		int bestCost = std::numeric_limits<int>::max();
		for (int dy = -6; dy <= 6; ++dy)
			for (int dx = -6; dx <= 6; ++dx)
			{
				const cPosition c = aim + cPosition (dx, dy);
				const int d = dx * dx + dy * dy;
				if (d < 9 || d > 25 || !map.isValidPosition (c) || airDanger (c) || !freeGround (c)) continue;
				const int cost = flightCost (t->getPosition(), c);
				if (cost < bestCost)
				{
					bestCost = cost;
					dropZone_ = c;
				}
			}
		dropChosen_ = bestCost != std::numeric_limits<int>::max();
		if (!dropChosen_) dropZone_ = tankStaging_;
		dropChosen_ = true;
		stepEvent ("drop_zone", {{"at", posJson (dropZone_)}, {"aim", posJson (aim)}, {"turn", turn_}});
	}
	if (dist2 (t->getPosition(), dropZone_) <= 8 && t->canExitTo (t->getPosition(), map, cargo->getStaticUnitData()))
	{
		cOrder o = order (eOrderType::ActivateUnit, t->getId(), "drop a tank next to the enemy defence");
		o.unitId2 = cargo->getId();
		o.position = t->getPosition();
		out.push_back (std::move (o));
		reserved_.insert ({t->getPosition().x(), t->getPosition().y()});
		stepEvent ("tank_drop", {{"tank", cargo->getId()}, {"at", posJson (t->getPosition())}, {"turn", turn_}});
		// в том же заходе — на соседнюю свободную клетку под следующий танк (действия сервер исполняет по порядку)
		int left = 0;
		for (const cVehicle* k : ownAll (model, "tank"))
			left += k->isUnitLoaded() && std::find (t->storedUnits.begin(), t->storedUnits.end(), k) != t->storedUnits.end() ? 1 : 0;
		if (left <= 1 || t->data.getSpeed() <= 0) return true;
	}
	else if (t->data.getSpeed() <= 0)
		return false;
	if (dist2 (t->getPosition(), dropZone_) <= 8)
	{
		// клетка под транспортом занята (или танк только что вышел) — на соседнюю свободную сушу
		for (int r = 1; r <= 3; ++r)
			for (int dy = -r; dy <= r; ++dy)
				for (int dx = -r; dx <= r; ++dx)
				{
					const cPosition c = t->getPosition() + cPosition (dx, dy);
					if (std::max (std::abs (dx), std::abs (dy)) != r || !map.isValidPosition (c) || !freeGround (c) || airDanger (c)) continue;
					auto path = toVector (cPathCalculator (*t, view, c, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
					cutToTurn (*t, view, path);
					if (path.empty() || path.back() != c) continue;
					cOrder o = order (eOrderType::Move, t->getId(), "air transport moves over a free cell");
					o.path = path;
					o.position = c;
					out.push_back (std::move (o));
					reserved_.insert ({c.x(), c.y()});
					return true;
				}
		return reserved_.count ({t->getPosition().x(), t->getPosition().y()}) != 0; // танк вышел, а сдвинуться некуда
	}
	return moveToward (model, view, *t, dropZone_, out, "air transport flies tanks to the drop zone", true);
}

// коммандос идёт к цели. Итог: 0 — нечего делать (нет коммандос или цели, он в транспорте), 1 — идёт и в
// конце пути будет рядом (выключит в следующем заходе: коммандос стреляет на ходу), 2 — идёт, за ход не дойдёт,
// 3 — рядом и может выключать, 4 — рядом, но выстрела нет (уже пробовал в этом ходу)
int cRedBrain::commandoApproach (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	const cVehicle* c = own (model, "commando");
	if (c == nullptr || c->isUnitLoaded() || !cmdHasTarget_) return 0;
	const cUnit* target = model.getUnitFromID (cmdTarget_);
	if (target == nullptr)
	{
		cmdHasTarget_ = false;
		return 0;
	}
	const auto nextTo = [&] (const cPosition& p) {
		for (const auto& cell : target->getPositions())
			if (std::abs (cell.x() - p.x()) <= 1 && std::abs (cell.y() - p.y()) <= 1) return true;
		return false;
	};
	if (nextTo (c->getPosition())) return cCommandoData::canDoAction (*c, target, false) ? 3 : 4;
	if (c->isUnitMoving() || c->data.getSpeed() <= 0) return 2;
	auto path = toVector (cPathCalculator (*c, view, *target, true).calcPath());
	if (path.empty())
	{
		// не подойти: соседние клетки заняты зданиями (П22) — другая цель
		cmdUnreachable_.insert (cmdTarget_);
		stepEvent ("commando_target_unreachable", {{"target", cmdTarget_}, {"targetType", seen_[cmdTarget_].type}, {"from", posJson (c->getPosition())}, {"turn", turn_}});
		cmdHasTarget_ = false;
		pickCommandoTarget (model);
		return 0;
	}
	const std::size_t full = path.size();
	cutToTurn (*c, view, path);
	if (path.empty()) return 2;
	cOrder o = order (eOrderType::Move, c->getId(), "commando sneaks to " + seen_[cmdTarget_].type);
	o.path = path;
	o.position = path.back();
	out.push_back (std::move (o));
	wantPass_ = true;
	return path.size() == full ? 1 : 2;
}

bool cRedBrain::commandoDisable (const cModel& model, std::vector<cOrder>& out, const std::string& what)
{
	const cVehicle* c = own (model, "commando");
	if (c == nullptr || !cmdHasTarget_) return false;
	const cUnit* target = model.getUnitFromID (cmdTarget_);
	if (target == nullptr || target->isDisabled() || !cCommandoData::canDoAction (*c, target, false)) return false;
	const int chance = c->getCommandoData().computeChance (target, false);
	cOrder o = order (eOrderType::Disable, c->getId(), "commando disables " + seen_[cmdTarget_].type);
	o.unitId2 = cmdTarget_;
	out.push_back (std::move (o));
	++cmdAttempts_;
	shots_.push_back ({c->getId(), cmdTarget_, seen_[cmdTarget_].type, target->data.getHitpoints(), what});
	stepEvent (what, {{"unit", c->getId()}, {"target", cmdTarget_}, {"targetType", seen_[cmdTarget_].type}, {"chance", chance},
		{"attempt", cmdAttempts_}, {"turn", turn_}});
	return true;
}

// коммандос в ходе удара: 0 — не нужен, не может или ещё далеко, 1 — подходит (выключит в следующем заходе),
// 2 — приказ выключить отдан
int cRedBrain::commandoStep (const cModel& model, const cMapView& view, std::vector<cOrder>& out, const std::string& what)
{
	if (cmdTriedTurn_ == turn_ || ferryState_ < 2) return 0;
	if (!cmdHasTarget_ || seen_.count (cmdTarget_) == 0 || seen_[cmdTarget_].disabled) return 0;
	const int r = commandoApproach (model, view, out);
	if (r == 3 && commandoDisable (model, out, what))
	{
		cmdTriedTurn_ = turn_;
		waitCommando_ = 0;
		return 2;
	}
	return r == 1 ? 1 : 0;
}

bool cRedBrain::commandoNeeded (const cModel& model) const
{
	if (own (model, "commando") == nullptr || !cmdHasTarget_) return false;
	const auto it = seen_.find (cmdTarget_);
	return it != seen_.end() && !it->second.disabled && cmdTriedTurn_ != turn_;
}

// --- rear: налёт с тыла ---------------------------------------------------------------------
void cRedBrain::rear (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cMap& map = *model.getMap();
	awacScout (model, view, pass, out);
	ferryCommando (model, view, pass, out);
	const auto bombers = ownAll (model, "bomber");
	if (side_ < 0)
	{
		// ждать выбора стороны вдали от базы, на стороне дома
		for (const cVehicle* b : bombers)
			moveToward (model, view, *b, onDir (map, base_, dirOf (base_, home_), 24), out, "bomber waits for the scout", true);
		return;
	}
	// цель коммандос: выбрать заново, если её нет, она выключена или это не зенитка (зенитка могла появиться)
	if (pass == 1 && (!cmdHasTarget_ || seen_.count (cmdTarget_) == 0 || seen_[cmdTarget_].disabled || !seen_[cmdTarget_].aa))
		pickCommandoTarget (model);
	std::vector<const cVehicle*> armed;
	for (const cVehicle* b : bombers)
		if (b->data.getAmmo() > 0) armed.push_back (b);
		else if (pass == 1) // пусто — домой
		{
			stage ("out_of_ammo", true, {{"unit", b->getId()}, {"turn", turn_}});
			moveToward (model, view, *b, home_, out, "bomber out of ammo: home", true);
		}
	if (armed.empty())
	{
		if (pass == 1) commandoStep (model, view, out, "disable");
		return;
	}
	if (strikePhase_ == 0)
	{
		bool atStaging = true;
		for (const cVehicle* b : armed) atStaging = atStaging && dist2 (b->getPosition(), staging_) <= 16;
		if (pass != 1)
		{
			// удар начинается только в первом заходе; в остальных — долететь до стоянки
			for (const cVehicle* b : armed)
				if (dist2 (b->getPosition(), staging_) > 16) moveToward (model, view, *b, staging_, out, "bomber to the staging point", true);
			return;
		}
		if (!atStaging)
		{
			for (const cVehicle* b : armed)
				if (dist2 (b->getPosition(), staging_) > 16) moveToward (model, view, *b, staging_, out, "bomber to the staging point", true);
			if (ferryState_ >= 2) commandoApproach (model, view, out); // коммандос тем временем идёт к цели
			return;
		}
		const bool needed = commandoNeeded (model);
		const int r = needed ? commandoStep (model, view, out, "disable") : 0;
		if (needed && r == 0 && waitCommando_ < 8)
		{
			// коммандос ещё в пути (в транспорте или пешком) — ждать, всего не больше 8 ходов
			++waitCommando_;
			stepEvent ("wait_commando", {{"turn", turn_}, {"waited", waitCommando_}, {"ferry", ferryState_}});
			return;
		}
		strikeTurn_ = turn_;
		stepEvent ("strike_begin", {{"turn", turn_}, {"bombers", armed.size()}, {"commando", r}});
		if (r == 2 || r == 1)
		{
			strikePhase_ = r == 2 ? 1 : 10; // выключает — бомбить в следующем заходе; подходит — выключит в следующем
			wantPass_ = true;
			return;
		}
		strikePhase_ = 1;
	}
	if (strikePhase_ == 10)
	{
		strikePhase_ = 1;
		if (commandoStep (model, view, out, "disable") == 2)
		{
			wantPass_ = true;
			return;
		}
	}
	if (strikePhase_ == 1)
	{
		bool any = false;
		for (const cVehicle* b : armed) any = bomberStrike (model, view, *b, 0, out, "bomb") || any;
		strikePhase_ = 2;
		if (any)
		{
			wantPass_ = true;
			return;
		}
		int aaSeen = 0;
		for (const auto& [id, sn] : seen_) aaSeen += sn.aa && !sn.disabled ? 1 : 0;
		stage ("bomb", false, {{"why", "нет цели с клеткой удара вне ПВО"}, {"turn", turn_}, {"seen", seen_.size()}, {"aaSeen", aaSeen}});
		strikePhase_ = 3;
	}
	if (strikePhase_ == 2)
	{
		bool any = false;
		for (const cVehicle* b : armed) any = fireInRange (model, view, *b, 0, out, "bomb") || any;
		strikePhase_ = 3;
		if (any)
		{
			wantPass_ = true;
			return;
		}
	}
	if (strikePhase_ == 3)
	{
		bool any = false;
		for (const cVehicle* b : armed) any = moveToward (model, view, *b, staging_, out, "bomber: strike done, back to staging", true) || any;
		strikePhase_ = 4;
		++strikes_;
		if (stagesDone_.count ("bomb") != 0)
		{
			bool safe = true;
			for (const auto& o : out)
				if (o.type == eOrderType::Move) safe = safe && !airDanger (o.position);
			stage ("retreat", safe, {{"turn", turn_}, {"strikes", strikes_}, {"moved", any}});
		}
	}
}

// --- combo: коммандос выключил зенитку → бомбардировщик → танки, по заходам одного хода ----------
void cRedBrain::combo (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cMap& map = *model.getMap();
	awacScout (model, view, pass, out);
	ferryCommando (model, view, pass, out);
	ferryTanks (model, view, pass, out);
	const cVehicle* bomber = own (model, "bomber");
	const auto tanks = ownAll (model, "tank");
	// танк в ходе удара: выстрел с места или подъехать и выстрелить; его выстрелы — «combo_tank»
	const auto tankShot = [&] (const cVehicle& t) {
		if (t.isUnitLoaded()) return false;
		if (!tankAssault (model, view, t, out)) return false;
		for (auto& s : shots_)
			if (s.unit == t.getId() && s.what == "tank") s.what = "combo_tank";
		return true;
	};
	if (comboReport_ && pass == 1)
	{
		comboReport_ = false;
		const bool inOrder = comboPasses_.size() == 3 && comboPasses_[0] >= 0 && comboPasses_[0] < comboPasses_[1]
			&& comboPasses_[1] < comboPasses_[2];
		stage ("combo", comboDisableOk_ && comboBombOk_ && comboTankOk_ && inOrder, {{"turn", strikeTurn_},
			{"passes", comboPasses_}, {"inOrder", inOrder}, {"disable", comboDisableOk_}, {"bomb", comboBombOk_}, {"tanks", comboTankOk_}});
		comboPhase_ = 5; // дальше — свободный бой
	}
	if (side_ < 0)
	{
		if (pass == 1)
		{
			const int hd = dirOf (base_, home_);
			if (bomber) moveToward (model, view, *bomber, onDir (map, base_, hd, 24), out, "bomber waits for the scout", true);
			if (tankFerry_ == 9)
				for (const cVehicle* t : tanks) moveToward (model, view, *t, onDir (map, base_, hd, 20), out, "tank moves up", false);
		}
		return;
	}
	// цель коммандос: выбрать заново, если её нет, она выключена или это не зенитка (зенитка могла появиться)
	if (pass == 1 && (!cmdHasTarget_ || seen_.count (cmdTarget_) == 0 || seen_[cmdTarget_].disabled || !seen_[cmdTarget_].aa))
		pickCommandoTarget (model);
	if (comboPhase_ == 5)
	{
		// после удара: высадить оставшиеся танки, танки бьют, бомбардировщик — по обороне, коммандос выключает
		if (pass == 1) commandoStep (model, view, out, "disable");
		if (tankDrop (model, view, out)) wantPass_ = true;
		for (const cVehicle* t : tanks)
			if (!t->isUnitLoaded()) wantPass_ = tankAssault (model, view, *t, out) || wantPass_;
		if (bomber && bomber->data.getAmmo() > 0)
		{
			if (pass == 1) wantPass_ = bomberStrike (model, view, *bomber, 1, out, "bomb") || wantPass_;
			else if (!fireInRange (model, view, *bomber, 1, out, "bomb") && pass == 2)
				moveToward (model, view, *bomber, staging_, out, "bomber back to staging", true);
		}
		return;
	}
	if (comboPhase_ == 0)
	{
		if (pass != 1)
		{
			if (bomber && bomber->data.getAmmo() > 0 && dist2 (bomber->getPosition(), staging_) > 16)
				moveToward (model, view, *bomber, staging_, out, "bomber to the staging point", true);
			return;
		}
		// сбор: танки в авиатранспорте на стоянке самолётов (или — без транспорта — своим ходом у края базы),
		// бомбардировщик — на стоянке, коммандос — у цели
		if (tankFerry_ == 9)
		{
			cPosition pick = tankStaging_;
			bool found = false;
			for (int r = 8; r <= 16 && !found; ++r)
			{
				const cPosition c0 = onDir (map, base_, side_, r);
				for (int dy = -1; dy <= 1 && !found; ++dy)
					for (int dx = -1; dx <= 1 && !found; ++dx)
					{
						const cPosition c = c0 + cPosition (dx, dy);
						if (isGround (map, c) && !groundDanger (c, 1))
						{
							pick = c;
							found = true;
						}
					}
			}
			tankStaging_ = pick;
		}
		bool tanksReady = false;
		if (tankFerry_ == 9)
		{
			tanksReady = !tanks.empty();
			for (const cVehicle* t : tanks)
				if (dist2 (t->getPosition(), tankStaging_) > 16)
				{
					tanksReady = false;
					moveToward (model, view, *t, tankStaging_, out, "tank to the staging point", false);
				}
		}
		else if (const cVehicle* tt = transport (model, 1))
		{
			bool inside = false;
			for (const cVehicle* k : tanks) inside = inside || k->isUnitLoaded();
			tanksReady = tankFerry_ == 1 && inside && dist2 (tt->getPosition(), staging_) <= 9;
		}
		bool bomberReady = bomber != nullptr && bomber->data.getAmmo() > 0;
		if (bomber && dist2 (bomber->getPosition(), staging_) > 16)
		{
			bomberReady = false;
			moveToward (model, view, *bomber, staging_, out, "bomber to the staging point", true);
		}
		if (ferryState_ >= 2 && !(tanksReady && bomberReady)) commandoApproach (model, view, out); // коммандос идёт к цели
		if (!tanksReady || !bomberReady)
		{
			stepEvent ("combo_gather", {{"turn", turn_}, {"tanksReady", tanksReady}, {"bomberReady", bomberReady}, {"ferry", ferryState_}});
			if (tanks.empty() && bomber == nullptr && stagesDone_.count ("combo_lost") == 0)
				stage ("combo_lost", true, {{"why", "нет танков и бомбардировщика — удара не будет"}, {"turn", turn_}});
			return;
		}
		const bool needed = commandoNeeded (model);
		const int r = needed ? commandoStep (model, view, out, "combo_disable") : 0;
		if (needed && r == 0 && waitCommando_ < 8)
		{
			++waitCommando_;
			stepEvent ("wait_commando", {{"turn", turn_}, {"waited", waitCommando_}, {"ferry", ferryState_}});
			return;
		}
		strikeTurn_ = turn_;
		comboPasses_.clear();
		comboDisableOk_ = comboBombOk_ = comboTankOk_ = false;
		comboTankPass_ = -1;
		dropChosen_ = false;
		stepEvent ("combo_begin", {{"turn", turn_}, {"tanks", tanks.size()}, {"commando", r},
			{"cmdTarget", cmdHasTarget_ ? nlohmann::json (seen_[cmdTarget_].type) : nlohmann::json()}});
		if (r == 2)
		{
			comboPasses_.push_back (pass);
			comboPhase_ = 1;
			wantPass_ = true;
			return;
		}
		if (r == 1)
		{
			comboPhase_ = 10;
			wantPass_ = true;
			return;
		}
		comboPasses_.push_back (-1);
		comboPhase_ = 1;
	}
	if (comboPhase_ == 10)
	{
		comboPhase_ = 1;
		if (commandoStep (model, view, out, "combo_disable") == 2)
		{
			comboPasses_.push_back (pass);
			wantPass_ = true;
			return;
		}
		comboPasses_.push_back (-1);
	}
	if (comboPhase_ == 1)
	{
		// бомбардировщик бьёт оборону; транспорт с танками тем временем летит к месту высадки
		comboPhase_ = 2;
		const bool bombed = bomber && bomberStrike (model, view, *bomber, 1, out, "combo_bomb");
		comboPasses_.push_back (bombed ? pass : -1);
		if (tankDrop (model, view, out) || bombed)
		{
			wantPass_ = true;
			return;
		}
	}
	if (comboPhase_ == 2)
	{
		// танки: высадка по одному (выгрузка не тратит их ход), вышедшие — сразу в бой
		const bool dropping = tankDrop (model, view, out);
		bool attacked = false;
		for (const cVehicle* t : tanks) attacked = tankShot (*t) || attacked;
		if (attacked && comboTankPass_ < 0) comboTankPass_ = pass;
		if (!dropping) comboPhase_ = 3;
		if (dropping || attacked)
		{
			wantPass_ = true;
			return;
		}
	}
	if (comboPhase_ == 3)
	{
		comboPhase_ = 4;
		bool any = false;
		for (const cVehicle* t : tanks) any = tankShot (*t) || any;
		if (any && comboTankPass_ < 0) comboTankPass_ = pass;
		if (bomber) any = fireInRange (model, view, *bomber, 1, out, "combo_bomb") || any;
		if (any)
		{
			wantPass_ = true;
			return;
		}
	}
	if (comboPhase_ == 4)
	{
		if (bomber) moveToward (model, view, *bomber, staging_, out, "bomber: strike done, back to staging", true);
		if (const cVehicle* tt = transport (model, 1))
			moveToward (model, view, *tt, staging_, out, "air transport: back to the staging point", true);
		comboPasses_.push_back (comboTankPass_);
		comboPhase_ = 6; // итог — в первом заходе следующего хода
		comboReport_ = true;
	}
}

// --- sea: высадка с моря ---------------------------------------------------------------------
void cRedBrain::sea (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cMap& map = *model.getMap();
	const cVehicle* ship = own (model, "sea_transport");
	const auto tanks = ownAll (model, "tank");
	const auto gunboats = ownAll (model, "gunboat");
	// канонерки: бить с места; иначе — к цели у берега (путь «на выстрел»), иначе — за транспортом
	for (const cVehicle* g : gunboats)
	{
		if (fireInRange (model, view, *g, 1, out, "gunboat"))
		{
			wantPass_ = true;
			continue;
		}
		if (pass != 1 || g->isUnitMoving() || g->data.getSpeed() <= 0) continue;
		bool moved = false;
		struct sCand
		{
			int d;
			unsigned int id;
		};
		std::vector<sCand> cands;
		for (const auto& [id, s] : seen_)
			if (targetValue (s, 1) > 3) cands.push_back ({dist2 (g->getPosition(), s.pos), id});
		std::sort (cands.begin(), cands.end(), [] (const sCand& a, const sCand& b) { return a.d != b.d ? a.d < b.d : a.id < b.id; });
		int tries = 0;
		for (const auto& c : cands)
		{
			if (++tries > 4) break;
			auto path = toVector (cPathCalculator (*g, view, seen_[c.id].pos, true).calcPath());
			if (path.empty()) continue;
			cutToTurn (*g, view, path);
			while (!path.empty() && reserved_.count ({path.back().x(), path.back().y()}) != 0) path.pop_back();
			if (path.empty()) break;
			cOrder o = order (eOrderType::Move, g->getId(), "gunboat closes in on " + seen_[c.id].type);
			o.path = path;
			o.position = path.back();
			reserved_.insert ({path.back().x(), path.back().y()});
			out.push_back (std::move (o));
			moved = true;
			wantPass_ = true;
			break;
		}
		if (!moved && beachChosen_) moveToward (model, view, *g, beach_, out, "gunboat to the beach", false);
		else if (!moved) moveToward (model, view, *g, base_, out, "gunboat toward the enemy coast", false);
	}
	if (ship == nullptr && seaState_ < 3)
	{
		bool loaded = false;
		for (const cVehicle* t : tanks) loaded = loaded || t->isUnitLoaded();
		if (seaState_ != 4)
		{
			stage ("landing", false, {{"why", "морской транспорт потерян"}, {"state", seaState_}, {"turn", turn_}});
			seaState_ = 4;
		}
		if (loaded) return;
	}
	if (seaState_ == 0)
	{
		if (pass == 1) ++embarkTurns_;
		int inside = 0, outside = 0;
		for (const cVehicle* t : tanks)
		{
			if (t->isUnitLoaded())
			{
				++inside;
				continue;
			}
			++outside;
			if (ship->canLoad (t))
			{
				cOrder o = order (eOrderType::Load, ship->getId(), "sea transport takes a tank");
				o.unitId2 = t->getId();
				out.push_back (std::move (o));
				wantPass_ = true;
				continue;
			}
			if (t->isUnitMoving() || t->data.getSpeed() <= 0) continue;
			auto path = toVector (cPathCalculator (*t, view, *ship, true).calcPath());
			const std::size_t full = path.size();
			cutToTurn (*t, view, path);
			if (path.empty()) continue;
			cOrder o = order (eOrderType::Move, t->getId(), "tank goes aboard");
			o.path = path;
			o.position = path.back();
			if (path.size() == full)
			{
				o.unitId2 = ship->getId();
				o.moveEnd = eMoveEnd::GetIn;
			}
			out.push_back (std::move (o));
			wantPass_ = true;
		}
		// танкам не дойти — транспорт сам подходит к ближайшему
		bool shipMoved = false;
		if (outside > 0 && embarkTurns_ >= 2 && pass == 1 && !ship->isUnitMoving())
			for (const cVehicle* t : tanks)
				if (!t->isUnitLoaded())
				{
					auto path = toVector (cPathCalculator (*ship, view, *t, true).calcPath());
					cutToTurn (*ship, view, path);
					if (path.empty()) continue; // к этому не подойти — к следующему
					cOrder o = order (eOrderType::Move, ship->getId(), "sea transport comes for a tank");
					o.path = path;
					o.position = path.back();
					out.push_back (std::move (o));
					shipMoved = true;
					break;
				}
		// шаг 14б: ни танку к транспорту, ни транспорту к танку (полоса берега, куда танк не встаёт, заставлена
		// зданиями) — встреча: клетка воды или берега, где транспорт встанет, рядом — суша, куда танк доедет
		if (outside > 0 && embarkTurns_ >= 2 && pass == 1 && !shipMoved && !ship->isUnitMoving())
		{
			const auto ordered = [&] (unsigned int id) {
				for (const auto& o : out)
					if (o.unitId == id) return true;
				return false;
			};
			const cVehicle* lead = nullptr;
			for (const cVehicle* t : tanks)
				if (!t->isUnitLoaded() && !ordered (t->getId()) && lead == nullptr) lead = t;
			if (!rvChosen_ && lead != nullptr)
			{
				int checks = 0;
				for (int r = 1; r <= 16 && !rvChosen_ && checks < 40; ++r)
					for (int dy = -r; dy <= r && !rvChosen_ && checks < 40; ++dy)
						for (int dx = -r; dx <= r && !rvChosen_ && checks < 40; ++dx)
						{
							if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
							const cPosition w = lead->getPosition() + cPosition (dx, dy);
							if (!map.isValidPosition (w) || !(map.isWater (w) || map.isCoast (w)) || !view.possiblePlace (*ship, w)) continue;
							cPosition land (-1, -1);
							for (int ey = -1; ey <= 1 && land.x() < 0; ++ey)
								for (int ex = -1; ex <= 1 && land.x() < 0; ++ex)
								{
									const cPosition l = w + cPosition (ex, ey);
									if ((ex != 0 || ey != 0) && isGround (map, l) && (l == lead->getPosition() || view.possiblePlace (*lead, l))) land = l;
								}
							if (land.x() < 0) continue;
							++checks;
							if (toVector (cPathCalculator (*ship, view, w, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath()).empty()) continue;
							if (land != lead->getPosition()
								&& toVector (cPathCalculator (*lead, view, land, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath()).empty())
								continue;
							rvWater_ = w;
							rvLand_ = land;
							rvChosen_ = true;
							stepEvent ("embark_rendezvous", {{"water", posJson (w)}, {"land", posJson (land)}, {"turn", turn_}});
						}
			}
			if (rvChosen_)
			{
				if (ship->getPosition() != rvWater_) moveToward (model, view, *ship, rvWater_, out, "sea transport to the rendezvous", false);
				for (const cVehicle* t : tanks)
					if (!t->isUnitLoaded() && !ordered (t->getId()) && dist2 (t->getPosition(), rvWater_) > 2)
						moveToward (model, view, *t, rvLand_, out, "tank to the rendezvous with the sea transport", false);
			}
		}
		if (outside == 0 || (inside > 0 && embarkTurns_ > 10))
		{
			seaState_ = 1;
			stage ("embark", inside > 0, {{"tanks", inside}, {"left", outside}, {"turn", turn_}});
		}
		else if (inside == 0 && embarkTurns_ > 16)
		{
			stage ("embark", false, {{"why", "танки не сели за 16 ходов"}, {"turn", turn_}});
			seaState_ = 4;
		}
		if (seaState_ != 1) return;
	}
	if (seaState_ == 1)
	{
		if (!beachChosen_)
		{
			// берег: вода или берег у суши, ближе к базе врага (но не ближе 5 клеток), куда транспорт доплывёт
			struct sCand
			{
				int d;
				cPosition p;
			};
			std::vector<sCand> cands;
			const int n = map.getSize().x();
			for (int y = 0; y < n; ++y)
				for (int x = 0; x < n; ++x)
				{
					const cPosition p (x, y);
					if (!map.isWater (p) && !map.isCoast (p)) continue;
					const int d = dist2 (p, base_);
					if (d < 25 || d > 40 * 40) continue;
					bool shore = false;
					for (int dy = -1; dy <= 1 && !shore; ++dy)
						for (int dx = -1; dx <= 1 && !shore; ++dx)
							shore = (dx != 0 || dy != 0) && isGround (map, p + cPosition (dx, dy));
					if (shore) cands.push_back ({d, p});
				}
			std::sort (cands.begin(), cands.end(), [] (const sCand& a, const sCand& b) {
				return a.d != b.d ? a.d < b.d : (a.p.y() != b.p.y() ? a.p.y() < b.p.y() : a.p.x() < b.p.x());
			});
			int tries = 0;
			for (const auto& c : cands)
			{
				if (++tries > 40) break;
				if (!toVector (cPathCalculator (*ship, view, c.p, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath()).empty())
				{
					beach_ = c.p;
					beachChosen_ = true;
					break;
				}
			}
			stage ("beach_chosen", beachChosen_, {{"beach", beachChosen_ ? posJson (beach_) : nlohmann::json()}, {"candidates", cands.size()},
				{"base", posJson (base_)}, {"turn", turn_}});
			if (!beachChosen_)
			{
				seaState_ = 4;
				return;
			}
		}
		if (pass == 1 || ship->data.getSpeed() > 0)
		{
			bool landNear = false;
			for (int dy = -1; dy <= 1 && !landNear; ++dy)
				for (int dx = -1; dx <= 1 && !landNear; ++dx)
					landNear = isGround (map, ship->getPosition() + cPosition (dx, dy)) && dist2 (ship->getPosition(), base_) <= dist2 (beach_, base_) + 16;
			if (ship->getPosition() == beach_ || landNear)
				seaState_ = 2;
			else if (pass == 1)
			{
				if (moveToward (model, view, *ship, beach_, out, "sea transport sails to the beach", false)) wantPass_ = true;
				return;
			}
			else
				return;
		}
	}
	if (seaState_ == 2)
	{
		int unloaded = 0;
		for (const cVehicle* t : tanks)
		{
			if (!t->isUnitLoaded()) continue;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const cPosition c = ship->getPosition() + cPosition (dx, dy);
					if ((dx == 0 && dy == 0) || !map.isValidPosition (c) || reserved_.count ({c.x(), c.y()}) != 0) continue;
					if (!ship->canExitTo (c, map, t->getStaticUnitData())) continue;
					cOrder o = order (eOrderType::ActivateUnit, ship->getId(), "unload a tank on the beach");
					o.unitId2 = t->getId();
					o.position = c;
					out.push_back (std::move (o));
					reserved_.insert ({c.x(), c.y()});
					++unloaded;
					dx = dy = 2; // выход из двух циклов
				}
		}
		seaState_ = 3;
		stage ("landing", unloaded > 0, {{"tanks", unloaded}, {"at", posJson (ship->getPosition())}, {"distToBase",
			static_cast<int> (std::sqrt (static_cast<double> (dist2 (ship->getPosition(), base_))))}, {"turn", turn_}});
		if (unloaded > 0)
		{
			wantPass_ = true;
			return; // выгрузка не тратит ход танков — в бой в следующем заходе
		}
	}
	// не все танки вышли на берег — выгружать дальше
	if (seaState_ == 3 && ship != nullptr)
		for (const cVehicle* t : tanks)
		{
			if (!t->isUnitLoaded()) continue;
			bool done = false;
			for (int dy = -1; dy <= 1 && !done; ++dy)
				for (int dx = -1; dx <= 1 && !done; ++dx)
				{
					const cPosition c = ship->getPosition() + cPosition (dx, dy);
					if ((dx == 0 && dy == 0) || !map.isValidPosition (c) || reserved_.count ({c.x(), c.y()}) != 0) continue;
					if (!ship->canExitTo (c, map, t->getStaticUnitData())) continue;
					cOrder o = order (eOrderType::ActivateUnit, ship->getId(), "unload a tank on the beach");
					o.unitId2 = t->getId();
					o.position = c;
					out.push_back (std::move (o));
					reserved_.insert ({c.x(), c.y()});
					wantPass_ = true;
					done = true;
				}
		}
	if (seaState_ >= 3)
	{
		for (const cVehicle* t : tanks)
			if (!t->isUnitLoaded()) wantPass_ = tankAssault (model, view, *t, out) || wantPass_;
		if (ship != nullptr && pass == 1 && seaState_ == 3)
		{
			bool empty = true;
			for (const cVehicle* t : tanks) empty = empty && !t->isUnitLoaded();
			if (empty) moveToward (model, view, *ship, home_, out, "sea transport goes back", false);
		}
	}
}

// --- subs: подлодки охотятся на флот, транспорты и верфь -------------------------------------
void cRedBrain::subs (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cMap& map = *model.getMap();
	const auto subsList = ownAll (model, "sub");
	if (const cVehicle* a = own (model, "awac"); a != nullptr && pass == 1)
	{
		// AWAC: облёт берега базы врага, вне ПВО (обзор для подлодок)
		if (awacRing_ < 0) awacRing_ = dirOf (base_, a->getPosition());
		if (dist2 (a->getPosition(), onDir (map, base_, awacRing_, kRingRadius + 2)) <= 4) awacRing_ = (awacRing_ + 1) % 8;
		moveToward (model, view, *a, onDir (map, base_, awacRing_, kRingRadius + 2), out, "awac: watch the enemy coast", true);
	}
	if (subsList.empty()) return;
	if (patrol_.empty() && pass == 1)
	{
		// точки дозора: вода у базы врага по 8 сторонам, куда подлодка доплывёт
		for (int r : {8, 13, 18})
			for (int d = 0; d < 8; ++d)
			{
				const cPosition c = onDir (map, base_, d, r);
				cPosition best;
				int bestD = std::numeric_limits<int>::max();
				for (int dy = -4; dy <= 4; ++dy)
					for (int dx = -4; dx <= 4; ++dx)
					{
						const cPosition p = c + cPosition (dx, dy);
						if (!map.isValidPosition (p) || !map.isWater (p)) continue;
						const int dd = dx * dx + dy * dy;
						if (dd < bestD)
						{
							bestD = dd;
							best = p;
						}
					}
				if (bestD == std::numeric_limits<int>::max()) continue;
				if (std::find (patrol_.begin(), patrol_.end(), best) != patrol_.end()) continue;
				// доплывёт хоть одна (у края карты подлодки при высадке запирают друг друга)
				bool reach = false;
				for (const cVehicle* sv : subsList)
					reach = reach || !toVector (cPathCalculator (*sv, view, best, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath()).empty();
				if (!reach) continue;
				patrol_.push_back (best);
			}
		nlohmann::json pts = nlohmann::json::array();
		for (const auto& p : patrol_) pts.push_back (posJson (p));
		stage ("patrol_planned", !patrol_.empty(), {{"points", pts}, {"base", posJson (base_)}, {"turn", turn_}});
		if (patrol_.empty())
		{
			patrol_.push_back (subsList.front()->getPosition()); // некуда — стоять
			patrolNone_ = true;
		}
	}
	bool contact = false;
	for (const auto& [id, s] : seen_) contact = contact || targetValue (s, 2) > 0;
	if (contact) stage ("hunt_contact", true, {{"turn", turn_}});
	else if (turn_ == startTurn_ + 40 && pass == 1)
		stage ("hunt_contact", false, {{"why", "за 40 ходов у врага не видно кораблей, верфи и дока"}, {"turn", turn_}});
	int k = 0;
	for (const cVehicle* s : subsList)
	{
		++k;
		if (fireInRange (model, view, *s, 2, out, "sub"))
		{
			wantPass_ = true;
			continue;
		}
		if (s->isUnitMoving() || s->data.getSpeed() <= 0) continue;
		// видимая морская цель — сблизиться на выстрел
		bool moved = false;
		long long best = std::numeric_limits<long long>::max();
		unsigned int target = kNoUnit;
		bool found = false;
		for (const auto& [id, sn] : seen_)
		{
			const int v = targetValue (sn, 2);
			if (v <= 0) continue;
			const long long key = static_cast<long long> (100 - v) * 1000000 + dist2 (s->getPosition(), sn.pos) * 10 + 0;
			if (key < best)
			{
				best = key;
				target = id;
				found = true;
			}
		}
		if (found)
		{
			auto path = toVector (cPathCalculator (*s, view, seen_[target].pos, true).calcPath());
			cutToTurn (*s, view, path);
			while (!path.empty() && reserved_.count ({path.back().x(), path.back().y()}) != 0) path.pop_back();
			if (!path.empty())
			{
				cOrder o = order (eOrderType::Move, s->getId(), "sub hunts " + seen_[target].type);
				o.path = path;
				o.position = path.back();
				reserved_.insert ({path.back().x(), path.back().y()});
				out.push_back (std::move (o));
				moved = true;
				wantPass_ = true; // подошла — выстрел в следующем заходе
			}
		}
		if (moved) continue;
		auto& idx = patrolIdx_[s->getId()];
		if (idx == 0 && patrol_.size() > 1) idx = static_cast<int> ((k * patrol_.size()) / (subsList.size() + 1)) + 1;
		const cPosition p = patrol_[(idx - 1 + patrol_.size()) % patrol_.size()];
		if (dist2 (s->getPosition(), p) <= 2 && !patrolNone_)
		{
			stage ("patrol", true, {{"unit", s->getId()}, {"at", posJson (p)}, {"turn", turn_}});
			idx = idx % static_cast<int> (patrol_.size()) + 1;
		}
		if (moveToward (model, view, *s, patrol_[(idx - 1 + patrol_.size()) % patrol_.size()], out, "sub patrols the enemy coast", false))
			wantPass_ = true; // запертые соседкой — в следующем заходе
	}
}

} // namespace ai
