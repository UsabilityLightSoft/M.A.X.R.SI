/***************************************************************************
 * ai_tactics.cpp — тактика заходов ИИИ (П19–П22, огонь наугад; Р17). См. ai_tactics.h.
 ***************************************************************************/

#include "ai_tactics.h"

#include "ai_army_ops.h"
#include "ai_defense.h"
#include "ai_roles.h"
#include "ai_world_model.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace ai
{

namespace
{

constexpr int kRunHpPct = 60;     // П19/П20: раненая (hp ниже этой доли) после выстрела уходит из «удара с подъезда» врага
constexpr int kPressRatioPct = 200; // «давить»: операция бьёт цель с силой ≥ этой доли нужной — решающая атака, раненая остаётся
constexpr int kBaseHitGap = 6;    // П22: урон из ниоткуда на суше ближе (Чебышёв) к краю базы — «у базы» (диверсант)
constexpr int kBlindMax = 8;      // огонь наугад: выстрелов за ход — не больше
constexpr int kRunSearch = 8;     // П20: клетку отхода ищем не дальше (Чебышёв)
constexpr int kCommandoHit = 6;   // П22: урон диверсанта за выстрел (данные commando: 6) — больше за ход у базы — пушка из тумана

int cheb (const cPosition& a, const cPosition& b)
{
	return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
}

nlohmann::json posJ (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

// выключатель опыта «=0» (значение переменной окружения передаёт вызывающий: std::getenv ("ИМЯ") — так замороженная копия его
// не читает, см. make_base*.py)
bool off (const char* v)
{
	return v != nullptr && v[0] == '0';
}

} // namespace

//------------------------------------------------------------------------------
void cTactics::event (const char* what, nlohmann::json extra)
{
	extra["event"] = "tactic";
	extra["level"] = "micro";
	extra["what"] = what;
	extra["turn"] = turn_;
	extra["player"] = playerId_;
	events_.push_back (std::move (extra));
}

std::vector<nlohmann::json> cTactics::takeEvents()
{
	std::vector<nlohmann::json> out;
	out.swap (events_);
	return out;
}

//------------------------------------------------------------------------------
void cTactics::update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cBaseDefense& defense, int turn)
{
	turn_ = turn;
	playerId_ = player.getId();
	hits_.clear();
	sites_.clear();
	blindDone_.clear();
	ranThisTurn_.clear();
	const cMap& map = *model.getMap();
	const cPosition center = defense.baseCenter();
	const int baseR = defense.baseRadius() + kBaseHitGap;

	// П22: урон из ниоткуда — hp упал с прошлого хода, а ни один вооружённый враг из памяти (не старше хода) не доставал клетку
	auto explained = [&] (const cPosition& p, bool plane) {
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (!m.armed() || world.age (m) > 1) continue;
			if (plane ? !m.hitsAir() : !(m.hitsGround() || (m.canAttack & 2) != 0)) continue;
			const int reach = m.building ? m.range + 1 : m.range + m.speed + 1;
			if (cheb (m.pos, p) <= reach) return true;
		}
		return false;
	};
	std::map<unsigned int, int> hpNow;
	std::set<unsigned int> disabledNow;
	auto check = [&] (const cUnit& u, bool plane) {
		hpNow[u.getId()] = u.data.getHitpoints();
		if (u.isDisabled()) disabledNow.insert (u.getId());
		const auto it = hp_.find (u.getId());
		if (it == hp_.end() || u.data.getHitpoints() >= it->second) return;
		const cPosition p = u.getPosition();
		if (explained (p, plane)) return;
		sHiddenHit h;
		h.pos = p;
		h.unit = u.getId();
		h.type = u.getStaticUnitData().getDefaultName();
		h.damage = it->second - u.data.getHitpoints();
		// подлодка бьёт только то, что на воде (корабль, мост, платформа, док; на берегу — корабль и здание)
		const bool onWater = map.isWater (p) || (map.isCoast (p) && (u.isABuilding() || u.getStaticUnitData().factorSea > 0));
		h.water = !plane && onWater;
		// у базы: вооружённых врагов из памяти (не старше 2 ходов) ближе 12 клеток нет — не бой, а невидимый стрелок
		bool fight = false;
		for (const auto& kv : world.enemies())
			if (kv.second.armed() && world.age (kv.second) <= 2 && cheb (kv.second.pos, p) <= 12) fight = true;
		// диверсант бьёт слабо (урон 6 до брони): попадание больше — пушка из тумана (артиллерия, ракеты), не тревога «диверсант»
		h.base = !plane && !h.water && !fight && center.x() >= 0 && cheb (p, center) <= baseR && h.damage <= kCommandoHit;
		hits_.push_back (h);
	};
	for (const auto& v : player.getVehicles())
		if (!v->isUnitLoaded()) check (*v, v->getStaticUnitData().factorAir > 0);
	for (const auto& b : player.getBuildings())
		check (*b, false);
	for (const auto& h : hits_)
	{
		++counts_.hiddenHits;
		if (h.water) ++counts_.subHits;
		if (h.base) ++counts_.baseHits;
		event ("hidden hit", {{"unitId", h.unit}, {"type", h.type}, {"pos", posJ (h.pos)}, {"damage", h.damage}, {"water", h.water}, {"base", h.base},
			{"why", h.water ? "damage with no known enemy in reach, near water: a submarine (only corvettes see it)"
							: (h.base ? "damage with no known enemy in reach, at the base: a hidden commando or a gun from the fog — infantry sweeps"
									  : "damage with no known enemy in reach: a gun from the fog (or a mine)")}});
	}
	// огонь наугад: свои, выключенные с прошлого хода (у них стоит диверсант — он действует с соседней клетки)
	for (const unsigned int id : disabledNow)
	{
		if (disabledLast_.count (id) != 0) continue;
		const cUnit* u = model.getUnitFromID (id);
		if (u == nullptr) continue;
		sites_.push_back ({u->getPosition(), u->getIsBig(), id, u->getStaticUnitData().getDefaultName()});
	}
	hp_ = std::move (hpNow);
	disabledLast_ = std::move (disabledNow);
}

//------------------------------------------------------------------------------
void cTactics::noteOutranged (unsigned int id, bool closeIn, const cPosition& from, const cPosition& to, int enemyRange, int ownRange)
{
	if (closeIn) ++counts_.closeIns;
	else ++counts_.stepOuts;
	event (closeIn ? "outranged: close in" : "outranged: step out", {{"unitId", id}, {"from", posJ (from)}, {"to", posJ (to)},
		{"enemyRange", enemyRange}, {"ownRange", ownRange},
		{"why", closeIn ? "the enemy outranges us: waiting at his fire edge gives him the first shot — close in and fire this turn"
						: "the enemy outranges us and we cannot reach him this turn: leave his reach instead of waiting for his first shot"}});
}

//------------------------------------------------------------------------------
std::vector<cOrder> cTactics::hitAndRun (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
	const cRoleBook& roles, const cArmyOps& ops, const std::set<unsigned int>& skip, int pass)
{
	std::vector<cOrder> out;
	if (off (std::getenv ("MAXR_HITRUN"))) return out; // опыт «вариант против варианта»: MAXR_HITRUN=0 — без П19/П20
	(void) model;
	// операции, что бьют цель с перевесом, — «давить»
	std::map<unsigned int, int> pressOf; // машина → операция «давить»
	for (const auto& op : ops.operations())
		if (op.state == eOpState::Attack && op.need > 0 && op.force * 100 >= op.need * kPressRatioPct)
			for (unsigned int id : op.members) pressOf[id] = op.id;
	const cPosition home = player.getLandingPos();
	for (const auto& vp : player.getVehicles())
	{
		const cVehicle& v = *vp;
		if (skip.count (v.getId()) != 0 || ranThisTurn_.count (v.getId()) != 0) continue;
		const eRole r = roles.roleOf (v);
		if (!isArmyRole (r) && r != eRole::AntiAir) continue;
		if (v.isUnitLoaded() || v.isDisabled() || v.isUnitMoving() || v.getMoveJob() != nullptr || v.isAttacking() || v.isBeingAttacked()) continue;
		if (v.data.getShots() >= v.data.getShotsMax() || v.data.getSpeed() <= 0) continue; // не стреляла или хода нет
		const int hpMax = v.data.getHitpointsMax();
		if (hpMax <= 0 || v.data.getHitpoints() * 100 >= hpMax * kRunHpPct) continue; // целая — под второй выстрел врага
		const cPosition p = v.getPosition();
		if (world.groundStrike (p) <= 0) continue; // враг не достанет — стоять
		// переживёт ход врага (ожидаемый урон врага в клетке меньше её hp) — стоять: выстрел ответного огня дороже отхода (r17i: отход
		// всякой раненой — 4168 раз на старых картах, D 0.458 против 0.489 без него)
		if (world.groundThreat (p) < v.data.getHitpoints()) continue;
		if (const auto pr = pressOf.find (v.getId()); pr != pressOf.end())
		{
			++counts_.presses;
			event ("press", {{"unitId", v.getId()}, {"op", pr->second}, {"hp", v.data.getHitpoints()}, {"hpMax", hpMax}, {"pass", pass},
				{"why", "decisive attack (force >= 2x need): the wounded stays, the enemy economy is worth the risk"}});
			ranThisTurn_.insert (v.getId());
			continue;
		}
		cPosition safe;
		if (!world.nearestSafeCell (v, view, p, kRunSearch, home, safe, eThreatLayer::Strike) || safe == p) continue;
		auto path = world.safePath (v, view, safe, eThreatLayer::Strike);
		// остатком хода (выстрел уже сделан): сколько пройдёт
		{
			int budget = v.data.getSpeed();
			cPosition prev = p;
			size_t n = 0;
			for (; n < path.size(); ++n)
			{
				const int c = cPathCalculator::calcNextCost (prev, path[n], &v, &view);
				if (c > budget) break;
				budget -= c;
				prev = path[n];
			}
			path.resize (n);
		}
		if (path.empty()) continue;
		const bool out_of_reach = world.groundStrike (path.back()) == 0;
		cOrder o;
		o.type = eOrderType::Move;
		o.playerId = player.getId();
		o.unitId = v.getId();
		o.path = std::move (path);
		o.position = o.path.back();
		o.level = "micro";
		o.reason = std::string ("pass ") + std::to_string (pass) + ": hit and run (wounded, fired: out of the enemy reach)";
		out.push_back (std::move (o));
		ranThisTurn_.insert (v.getId());
		++counts_.runs;
		event ("hit and run", {{"unitId", v.getId()}, {"role", roleName (r)}, {"hp", v.data.getHitpoints()}, {"hpMax", hpMax},
			{"from", posJ (p)}, {"to", posJ (out.back().position)}, {"safe", out_of_reach}, {"pass", pass},
			{"why", "fired this turn and wounded: step out of the enemy reach with the rest of the move, healthy units take the next shot"}});
	}
	return out;
}

//------------------------------------------------------------------------------
std::vector<cOrder> cTactics::blindFire (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
	const std::vector<const cVehicle*>& shooters, int pass)
{
	std::vector<cOrder> out;
	if (sites_.empty() || shooters.empty() || off (std::getenv ("MAXR_BLIND"))) return out; // MAXR_BLIND=0 — без огня наугад
	(void) world;
	const cMap& map = *model.getMap();
	std::set<unsigned int> used;
	for (const auto& s : sites_)
	{
		if (counts_.blindShots >= 0 && static_cast<int> (blindDone_.size()) >= kBlindMax) break;
		// клетки рядом с выключенным (диверсант действует только с соседней): суша, не своя и без видимого врага
		std::vector<cPosition> cells;
		const int sz = s.big ? 2 : 1;
		for (int dy = -1; dy <= sz; ++dy)
			for (int dx = -1; dx <= sz; ++dx)
			{
				if (dx >= 0 && dx < sz && dy >= 0 && dy < sz) continue;
				const cPosition c = s.pos + cPosition (dx, dy);
				if (!map.isValidPosition (c) || map.isWater (c)) continue;
				if (blindDone_.count (static_cast<long long> (c.y()) * 100000 + c.x()) != 0) continue;
				const auto& field = map.getField (c);
				bool skip = false;
				for (const cBuilding* b : field.getBuildings())
					if (b->getOwner() == &player || b->getStaticUnitData().surfacePosition == eSurfacePosition::Ground) skip = true; // своё или здание — диверсант не встанет
				if (const cVehicle* fv = field.getVehicle(); fv != nullptr && (fv->getOwner() == &player || view.canSeeUnit (*fv))) skip = true;
				if (!skip) cells.push_back (c);
			}
		if (cells.empty()) continue;
		std::vector<cPosition> fired;
		for (const auto& c : cells)
		{
			if (static_cast<int> (blindDone_.size()) >= kBlindMax) break;
			// стрелок: свободный, достаёт клетку; многозарядный и дешёвый — раньше (по id при равенстве)
			const cVehicle* best = nullptr;
			long long bestKey = std::numeric_limits<long long>::max();
			for (const cVehicle* sv : shooters)
			{
				if (used.count (sv->getId()) != 0 || sv->data.getShots() <= 0 || sv->data.getAmmo() <= 0) continue;
				if (!sv->canAttackObjectAt (c, view, true)) continue;
				const long long key = static_cast<long long> (-sv->data.getShots()) * 1000000 + sv->data.getBuildCost() * 1000 + sv->getId() % 1000;
				if (key < bestKey)
				{
					bestKey = key;
					best = sv;
				}
			}
			if (best == nullptr) continue;
			if (best->data.getShots() <= 1) used.insert (best->getId()); // у многозарядного — ещё выстрел на другую клетку
			cOrder o;
			o.type = eOrderType::Attack;
			o.playerId = player.getId();
			o.unitId = best->getId();
			o.position = c;
			o.blindFire = true;
			o.level = "micro";
			o.reason = std::string ("pass ") + std::to_string (pass) + ": blind fire next to the disabled " + s.type + " (a hidden commando may stand there)";
			out.push_back (std::move (o));
			blindDone_.insert (static_cast<long long> (c.y()) * 100000 + c.x());
			fired.push_back (c);
			++counts_.blindShots;
		}
		if (!fired.empty())
		{
			++counts_.blindSites;
			nlohmann::json cj = nlohmann::json::array();
			for (const auto& c : fired) cj.push_back ({c.x(), c.y()});
			event ("blind fire", {{"site", posJ (s.pos)}, {"disabled", s.unit}, {"type", s.type}, {"cells", cj}, {"candidates", cells.size()}, {"pass", pass},
				{"why", "own unit disabled since last turn with no commando seen: he acts only from an adjacent cell and does not leave in that turn"}});
		}
	}
	return out;
}

} // namespace ai
