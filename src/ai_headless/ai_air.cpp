/***************************************************************************
 * ai_air.cpp — авиация ИИИ (П15–П17, П10; шаг 13, часть 3). Описание — в заголовке.
 ***************************************************************************/

#include "ai_air.h"

#include "ai_commando.h"
#include "ai_defense.h"
#include "ai_navy.h"
#include "ai_roles.h"
#include "ai_world_model.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <queue>
#include <tuple>

namespace ai
{

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr int kAirIncome = 24;      // программа «удар»: добыча металла в ход не ниже…
constexpr int kAirTurn = 90;        // …и не раньше этого хода (экономика сначала: урок флота части 2)
constexpr int kAirLateTurn = 180;   // или с этого хода при добыче ≥ kAirLateIncome
constexpr int kAirLateIncome = 16;
constexpr int kAirOnlyIncome = 16;  // упор «воздух» уверенно (враг только за водой, моря нет) — с этой добычи
constexpr int kSeenFresh = 20;      // округа клетки удара видна не раньше стольких ходов назад
constexpr int kAroundR = 9;         // округа клетки удара: досягаемость зенитки 8 + 1
constexpr int kReserve = 24;        // очков хода на отход после удара (6 клеток)
constexpr int kNearEnemy = 26;      // у базы врага (Чебышёв от зданий) — AWAC по kAwacStep клеток за заход
constexpr int kAwacStep = 4;
constexpr int kAwacRing = 13;       // облёт базы врага (обзор AWAC 12)
constexpr int kServiceHp = 50;      // самолёт с hp ниже (%) — в ангар
constexpr int kEnemyAirFresh = 15;  // авиация врага в памяти не старше — угроза (истребители)
constexpr int kSubNear = 24;        // подлодка врага у нашей верфи и кораблей (Чебышёв) — цель бомбардировщиков (п. 100)
constexpr int kStageDist = 16;      // стоянка бомбардировщиков — в стольких клетках от ближайшего здания врага
constexpr int kFactoryWait = 20;    // остров или враг точно за водой, программа «удар» включена столько ходов, а авиазавода нет — он раньше шахт
constexpr int kIslandSharePct = 40; // своя суша меньше этой доли всей суши — остров (мест под шахты мало, враг за водой)
constexpr int kRaidPrepare = 30;    // подготовка налёта (коммандос к зенитке) — не дольше
constexpr int kDeathZoneR = 10;     // где погиб свой самолёт — опасно (радиус)…
constexpr int kDeathZoneTurns = 40; // …столько ходов
constexpr int kLossWindow = 30;     // потерь самолётов за столько ходов…
constexpr int kLossPause = 4;       // …не меньше — пауза в производстве на kLossWindow ходов
constexpr int kStageGap = 20;       // стоянка бомбардировщиков — не ближе к зданиям врага
constexpr int kRaidWalk = 35;       // налёт — если свой коммандос не дальше от зенитки (пешком ≤ 7 ходов)
constexpr int kArmyDangerR = 10;    // армия врага (машины с оружием, виденные за kArmyFresh ходов) — опасно самолёту в этом радиусе
constexpr int kArmyFresh = 3;
constexpr int kDangerStep = 400;    // штраф пути самолёта за опасную клетку (обычный шаг — 4)
constexpr int kStatusEvery = 10;

int cheb (const cPosition& a, const cPosition& b)
{
	return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
}

long long dist2 (const cPosition& a, const cPosition& b)
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

std::vector<cPosition> toVec (const std::forward_list<cPosition>& l)
{
	return std::vector<cPosition> (l.begin(), l.end());
}

void cutToTurn (const cVehicle& v, const cMapView& view, std::vector<cPosition>& path)
{
	int budget = v.data.getSpeed();
	cPosition prev = v.getPosition();
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

int pathCost (const cVehicle& v, const cMapView& view, const std::vector<cPosition>& path)
{
	int sum = 0;
	cPosition prev = v.getPosition();
	for (const auto& p : path)
	{
		sum += cPathCalculator::calcNextCost (prev, p, &v, &view);
		prev = p;
	}
	return sum;
}

nlohmann::json posJ (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

cPosition clampTo (int W, int H, cPosition p)
{
	return cPosition (std::clamp (p.x(), 1, W - 2), std::clamp (p.y(), 1, H - 2));
}

// точка на направлении d (0..7, по часовой от «восток») на расстоянии r от c
cPosition onDir (int W, int H, const cPosition& c, int d, int r)
{
	const double a = kPi / 4.0 * ((d % 8 + 8) % 8);
	return clampTo (W, H, c + cPosition (static_cast<int> (std::lround (r * std::cos (a))), static_cast<int> (std::lround (r * std::sin (a)))));
}

int hpPct (const cVehicle& v)
{
	const int m = v.data.getHitpointsMax();
	return m > 0 ? v.data.getHitpoints() * 100 / m : 100;
}

const char* roleKey (eRole r)
{
	return roleName (r);
}

} // namespace

//------------------------------------------------------------------------------
const char* cAirForce::programName() const
{
	return program_ == eProgram::Strike ? "strike" : "none";
}

void cAirForce::event (const char* what, nlohmann::json extra)
{
	nlohmann::json e = {{"event", "air"}, {"level", "operational"}, {"what", what}, {"turn", turn_}};
	if (extra.is_object())
		for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
	events_.push_back (std::move (e));
}

std::vector<nlohmann::json> cAirForce::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

nlohmann::json cAirForce::toJson() const
{
	nlohmann::json sides = nlohmann::json::array();
	for (int s : sideScore_) sides.push_back (s);
	return {{"program", programName()}, {"why", why_}, {"awacs", counts_.awacs}, {"bombers", counts_.bombers}, {"fighters", counts_.fighters},
		{"transports", counts_.transports}, {"airFactories", counts_.airFactories}, {"hangars", counts_.hangars},
		{"strikes", counts_.strikes}, {"hits", counts_.hits}, {"kills", counts_.kills}, {"subStrikes", counts_.subStrikes},
		{"intercepts", counts_.intercepts}, {"raids", counts_.raids}, {"raidDisables", counts_.raidDisables}, {"ferries", counts_.ferries},
		{"aaFerries", counts_.aaFerries},
		{"lost", counts_.lost}, {"enemyAa", counts_.enemyAa}, {"enemyAir", counts_.enemyAir}, {"focus", posJ (focus_)},
		{"focusKnown", focusKnown_}, {"sides", sides}, {"bestSide", bestSide_},
		{"raid", {{"state", static_cast<int> (raid_.state)}, {"target", raid_.target == kNoUnit ? -1 : static_cast<long long> (raid_.target)},
			{"aa", raid_.aa == kNoUnit ? -1 : static_cast<long long> (raid_.aa)}}}};
}

//------------------------------------------------------------------------------
const cBuilding* cAirForce::hangar (const cModel& model) const
{
	for (unsigned int id : hangars_)
		if (const cBuilding* b = model.getBuildingFromID (id); b != nullptr && !b->isDisabled()) return b;
	return nullptr;
}

//------------------------------------------------------------------------------
void cAirForce::buildAa (const cModel& model, const cPlayer& player, const cWorldModel& world)
{
	aa_.clear();
	const cMap& map = *model.getMap();
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.hitsAir() || m.plane) continue; // самолёты врага — не зенитки (от истребителей не спрятаться клеткой)
		if (!m.building && world.age (m) > 6) continue;
		// выключенная (видна сейчас) — не стреляет
		if (const cUnit* u = model.getUnitFromID (m.id); u != nullptr && m.visible && player.canSeeUnit (*u, map) && u->isDisabled()) continue;
		sAaSource s;
		s.pos = m.pos;
		s.big = m.big;
		s.id = m.id;
		s.building = m.building;
		s.reach = m.range + (m.building ? 0 : m.speed) + 1;
		aa_.push_back (s);
	}
	counts_.enemyAa = static_cast<int> (aa_.size());
}

void cAirForce::buildAaGrid()
{
	aaGrid_.assign (static_cast<size_t> (W_) * H_, 0);
	for (const auto& s : aa_)
	{
		const int r = s.big ? s.reach + 1 : s.reach;
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (dx * dx + dy * dy > r * r) continue;
				const int x = s.pos.x() + dx, y = s.pos.y() + dy;
				if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
				aaGrid_[static_cast<size_t> (y) * W_ + x] = 1;
			}
	}
	for (const auto& z : deathZones_)
		for (int dy = -kDeathZoneR; dy <= kDeathZoneR; ++dy)
			for (int dx = -kDeathZoneR; dx <= kDeathZoneR; ++dx)
			{
				if (dx * dx + dy * dy > kDeathZoneR * kDeathZoneR) continue;
				const int x = z.first.x() + dx, y = z.first.y() + dy;
				if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
				aaGrid_[static_cast<size_t> (y) * W_ + x] = 1;
			}
}

bool cAirForce::aaDanger (const cPosition& p, unsigned int ignore) const
{
	if (ignore == kNoUnit && aaGrid_.size() == static_cast<size_t> (W_) * H_)
	{
		if (p.x() < 0 || p.y() < 0 || p.x() >= W_ || p.y() >= H_) return false;
		return aaGrid_[static_cast<size_t> (p.y()) * W_ + p.x()] != 0;
	}
	for (const auto& s : aa_)
	{
		if (s.id == ignore) continue;
		const int r = s.big ? s.reach + 1 : s.reach;
		if (dist2 (p, s.pos) <= static_cast<long long> (r) * r) return true;
	}
	return deathZone (p);
}

bool cAirForce::deathZone (const cPosition& p) const
{
	for (const auto& z : deathZones_)
		if (dist2 (p, z.first) <= static_cast<long long> (kDeathZoneR) * kDeathZoneR) return true;
	return false;
}

bool cAirForce::targetAlive (const cModel& model, const cPlayer& player, const sEnemyMemory& m) const
{
	if (deadTargets_.count (m.id) != 0) return false;
	const cMap& map = *model.getMap();
	if (map.isValidPosition (m.pos) && player.canSeeAt (m.pos))
	{
		const cUnit* u = model.getUnitFromID (m.id);
		if (u == nullptr || !player.canSeeUnit (*u, map)) return false; // клетка видна, а цели там нет
	}
	return true;
}

bool cAirForce::seenAround (const cWorldModel& world, const cPosition& p) const
{
	for (int dy = -kAroundR; dy <= kAroundR; dy += 3)
		for (int dx = -kAroundR; dx <= kAroundR; dx += 3)
		{
			if (dx * dx + dy * dy > kAroundR * kAroundR) continue;
			const cPosition c = p + cPosition (dx, dy);
			if (c.x() < 0 || c.y() < 0 || c.x() >= W_ || c.y() >= H_) continue;
			if (world.seenTurn (c) < turn_ - kSeenFresh) return false;
		}
	return true;
}

bool cAirForce::reservedCell (const cPosition& p) const
{
	return std::find (reserved_.begin(), reserved_.end(), p) != reserved_.end();
}

std::vector<cPosition> cAirForce::flyPath (const cVehicle& v, const cMapView& view, const cPosition& dest, bool safe, unsigned int ignore,
	int stepCap) const
{
	if (dest.x() < 0 || dest == v.getPosition()) return {};
	std::vector<cPosition> path;
	if (!safe)
		path = toVec (cPathCalculator (v, view, dest, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
	else
	{
		// A* по клеткам (самолёту местность не мешает): шаг 4, по диагонали 6; опасная клетка (зенитки, места гибели, армия
		// врага) — штраф kDangerStep: путь в обход, если он есть; поиск — в окне вокруг старта и цели
		const cPosition s = v.getPosition();
		const int pad = 24;
		const int x0 = std::max (0, std::min (s.x(), dest.x()) - pad), x1 = std::min (W_ - 1, std::max (s.x(), dest.x()) + pad);
		const int y0 = std::max (0, std::min (s.y(), dest.y()) - pad), y1 = std::min (H_ - 1, std::max (s.y(), dest.y()) + pad);
		const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
		const size_t n = static_cast<size_t> (bw) * bh;
		std::vector<int> g (n, std::numeric_limits<int>::max());
		std::vector<int> prev (n, -1);
		std::vector<char> closed (n, 0);
		auto idx = [&] (const cPosition& p) { return static_cast<int> ((p.y() - y0) * bw + (p.x() - x0)); };
		auto posOf = [&] (int i) { return cPosition (x0 + i % bw, y0 + i / bw); };
		auto danger = [&] (const cPosition& p) { return aaDanger (p, ignore) || armyNear (p); };
		auto h = [&] (const cPosition& p) { return flightCost (p, dest); };
		using tNode = std::tuple<int, int, int>;
		std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
		const int si = idx (s), di = idx (dest);
		g[si] = 0;
		open.emplace (h (s), si, si);
		while (!open.empty())
		{
			const auto [f, ord, i] = open.top();
			open.pop();
			(void) f;
			(void) ord;
			if (closed[i]) continue;
			closed[i] = 1;
			if (i == di) break;
			const cPosition p = posOf (i);
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					if (dx == 0 && dy == 0) continue;
					const cPosition q = p + cPosition (dx, dy);
					if (q.x() < x0 || q.y() < y0 || q.x() > x1 || q.y() > y1) continue;
					const int qi = idx (q);
					if (closed[qi]) continue;
					const int step = (dx != 0 && dy != 0 ? 6 : 4) + (danger (q) ? kDangerStep : 0);
					const int ng = g[i] + step;
					if (ng >= g[qi]) continue;
					g[qi] = ng;
					prev[qi] = i;
					open.emplace (ng + h (q), qi, qi);
				}
		}
		if (prev[di] >= 0)
		{
			for (int i = di; i != si; i = prev[i]) path.push_back (posOf (i));
			std::reverse (path.begin(), path.end());
		}
		// клетка цели занята другим самолётом — путь движка (он найдёт соседнюю)
		if (!path.empty() && !view.possiblePlace (v, path.back())) path.clear();
	}
	cutToTurn (v, view, path);
	if (safe)
		for (size_t i = 0; i < path.size(); ++i)
			if (aaDanger (path[i], ignore) || (armyNear (path[i]) && !armyNear (v.getPosition())))
			{
				path.resize (i);
				break;
			}
	if (stepCap > 0 && static_cast<int> (path.size()) > stepCap) path.resize (stepCap);
	while (!path.empty() && reservedCell (path.back())) path.pop_back();
	return path;
}

bool cAirForce::seenWithin (const cWorldModel& world, const cPosition& p, int r, int fresh) const
{
	for (int dy = -r; dy <= r; dy += 2)
		for (int dx = -r; dx <= r; dx += 2)
		{
			if (dx * dx + dy * dy > r * r) continue;
			const cPosition c = p + cPosition (dx, dy);
			if (c.x() < 0 || c.y() < 0 || c.x() >= W_ || c.y() >= H_) continue;
			if (world.seenTurn (c) < turn_ - fresh) return false;
		}
	return true;
}

bool cAirForce::pushMove (const cVehicle& v, std::vector<cPosition> path, const std::string& reason, std::vector<cOrder>& out,
	unsigned int target, eMoveEnd end)
{
	if (path.empty()) return false;
	cOrder o;
	o.type = eOrderType::Move;
	o.playerId = playerId_;
	o.unitId = v.getId();
	o.path = std::move (path);
	o.position = o.path.back();
	o.unitId2 = target;
	o.moveEnd = end;
	o.level = "micro";
	o.reason = "air: " + reason;
	reserved_.push_back (o.position);
	lastPos_[v.getId()] = o.position; // где окажется к концу хода (погибнет там — место опасно)
	out.push_back (std::move (o));
	return true;
}

cPosition cAirForce::safeNear (const cMapView& view, const cVehicle& v, const cPosition& anchor, int r, bool farFromEnemy) const
{
	(void) farFromEnemy;
	for (int rr = 0; rr <= r; ++rr)
	{
		cPosition best (-1, -1);
		long long bestKey = std::numeric_limits<long long>::max();
		for (int dy = -rr; dy <= rr; ++dy)
			for (int dx = -rr; dx <= rr; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != rr) continue;
				const cPosition c = anchor + cPosition (dx, dy);
				if (!view.isValidPosition (c) || aaDanger (c) || reservedCell (c)) continue;
				if (c != v.getPosition() && !view.possiblePlace (v, c)) continue;
				const long long key = dist2 (c, v.getPosition()) * 100000 + static_cast<long long> (c.y()) * W_ + c.x();
				if (key < bestKey)
				{
					bestKey = key;
					best = c;
				}
			}
		if (best.x() >= 0) return best;
	}
	return cPosition (-1, -1);
}

//------------------------------------------------------------------------------
void cAirForce::update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, const cNavy& navy, const cCommandos& commandos, int income, int factories, int turn)
{
	turn_ = turn;
	playerId_ = player.getId();
	const cMap& map = *model.getMap();
	W_ = map.getSize().x();
	H_ = map.getSize().y();
	home_ = defense.baseCenter();
	const auto& ud = *model.getUnitsData();

	// свои авиазаводы, ангары, верфь
	hangars_.clear();
	yard_ = cPosition (-1, -1);
	const int strikes = counts_.strikes, hits = counts_.hits, kills = counts_.kills, subStrikes = counts_.subStrikes,
		intercepts = counts_.intercepts, raids = counts_.raids, raidDisables = counts_.raidDisables, ferries = counts_.ferries, lost = counts_.lost,
		aaFerries = counts_.aaFerries;
	counts_ = {};
	counts_.strikes = strikes;
	counts_.hits = hits;
	counts_.kills = kills;
	counts_.subStrikes = subStrikes;
	counts_.intercepts = intercepts;
	counts_.raids = raids;
	counts_.raidDisables = raidDisables;
	counts_.ferries = ferries;
	counts_.lost = lost;
	counts_.aaFerries = aaFerries;
	for (const auto& b : player.getBuildings())
	{
		const auto& sd = b->getStaticUnitData();
		if (sd.canBuild == "Plane") ++counts_.airFactories;
		if (sd.storageUnitsMax > 0 && std::find (sd.storeUnitsTypes.begin(), sd.storeUnitsTypes.end(), "Plane") != sd.storeUnitsTypes.end())
			hangars_.push_back (b->getId());
		if (sd.canBuild == "Ship" && yard_.x() < 0) yard_ = b->getPosition();
	}
	counts_.hangars = static_cast<int> (hangars_.size());
	// свои самолёты; погибшие — в журнал
	std::set<unsigned int> alive;
	for (const auto& v : player.getVehicles())
	{
		const eRole r = roles.roleOf (*v);
		if (!isAirRole (r)) continue;
		alive.insert (v->getId());
		typeOf_[v->getId()] = v->getStaticUnitData().getDefaultName();
		counts_.planeMetal += v->data.getBuildCost();
		switch (r)
		{
			case eRole::Awac: ++counts_.awacs; break;
			case eRole::Bomber: ++counts_.bombers; break;
			case eRole::Fighter: ++counts_.fighters; break;
			case eRole::AirTransport: ++counts_.transports; break;
			default: ++counts_.other; break;
		}
	}
	for (unsigned int id : known_)
		if (alive.count (id) == 0)
		{
			++counts_.lost;
			const auto lp = lastPos_.find (id);
			const cPosition at = lp != lastPos_.end() ? lp->second : cPosition (-1, -1);
			if (at.x() >= 0) deathZones_.emplace_back (at, turn);
			lossTurns_.push_back (turn);
			event ("lost", {{"unitId", id}, {"type", typeOf_[id]}, {"pos", posJ (at)},
				{"why", "AA or fighters we did not see: the place is dangerous for 40 turns"}});
			strikes_.erase (id);
			serviceSet_.erase (id);
			awacRing_.erase (id);
			lastPos_.erase (id);
		}
	known_ = alive;
	deathZones_.erase (std::remove_if (deathZones_.begin(), deathZones_.end(), [&] (const auto& z) { return turn - z.second > kDeathZoneTurns; }),
		deathZones_.end());
	lossTurns_.erase (std::remove_if (lossTurns_.begin(), lossTurns_.end(), [&] (int t) { return turn - t > kLossWindow; }), lossTurns_.end());
	if (static_cast<int> (lossTurns_.size()) >= kLossPause && pauseUntil_ < turn)
	{
		pauseUntil_ = turn + kLossWindow;
		event ("pause", {{"until", pauseUntil_}, {"losses", lossTurns_.size()},
			{"why", "planes keep dying (unseen AA): no new planes for a while, metal goes to the army"}});
	}
	for (const auto& v : player.getVehicles())
		if (alive.count (v->getId()) != 0 && !v->isUnitLoaded()) lastPos_[v->getId()] = v->getPosition();

	// картина врага: зенитки, авиация, подлодки у нашей верфи и кораблей, где база
	buildAa (model, player, world);
	armyDanger_.assign (static_cast<size_t> (W_) * H_, 0);
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (m.building || m.plane || m.ship || !m.armed() || world.age (m) > kArmyFresh) continue;
		for (int dy = -kArmyDangerR; dy <= kArmyDangerR; ++dy)
			for (int dx = -kArmyDangerR; dx <= kArmyDangerR; ++dx)
			{
				const int x = m.pos.x() + dx, y = m.pos.y() + dy;
				if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
				armyDanger_[static_cast<size_t> (y) * W_ + x] = 1;
			}
	}
	buildAaGrid();
	int enemyAir = 0, enemyPlanes = 0;
	bool subsNear = false;
	std::vector<cPosition> ourNaval;
	if (yard_.x() >= 0) ourNaval.push_back (yard_);
	for (const auto& v : player.getVehicles())
		if (isNavalRole (roles.roleOf (*v)) && !v->isUnitLoaded()) ourNaval.push_back (v->getPosition());
	long long sx = 0, sy = 0, n = 0;
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (m.owner < 0) continue;
		const sID id (m.typeFirst, m.typeSecond);
		if (m.building)
		{
			if (ud.isValidId (id) && ud.getDynamicUnitData (id).getBuildCost() > 2)
			{
				sx += m.pos.x();
				sy += m.pos.y();
				++n;
			}
			continue;
		}
		if (m.plane && world.age (m) <= kEnemyAirFresh && ud.isValidId (id))
		{
			enemyAir += ud.getDynamicUnitData (id).getBuildCost();
			++enemyPlanes;
		}
		if (m.ship && ud.isValidId (id) && (ud.getStaticUnitData (id).isStealthOn & eTerrainFlag::Sea) != 0 && world.age (m) <= 10)
			for (const auto& p : ourNaval)
				if (cheb (p, m.pos) <= kSubNear) subsNear = true;
	}
	counts_.enemyAir = enemyAir;
	const bool known = n > 0;
	focus_ = known ? cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n)) : navy.enemyFocus();
	if (focus_.x() < 0) focus_ = cPosition (W_ - 1 - home_.x(), H_ - 1 - home_.y());
	if (known != focusKnown_ && program_ != eProgram::None)
		event ("enemy focus", {{"pos", posJ (focus_)}, {"known", known},
			{"why", known ? "centre of the seen enemy buildings" : "enemy not seen: the point opposite our base"}});
	focusKnown_ = known;

	// программа (простой вариант — сколько металла в авиацию, потом решает Модель)
	const auto& mp = defense.map();
	eProgram p = eProgram::None;
	std::string why;
	// подлодки у верфи помним 40 ходов: верфь они топят быстро, а новая верфь встанет туда же
	if (subsNear) antiSubUntil_ = turn + 40;
	antiSub_ = turn < antiSubUntil_;
	if (antiSub_)
	{
		p = eProgram::Strike;
		why = "enemy subs at our shipyard or ships: bombers hit subs (only corvettes see them, bombers and corvettes hit them)";
	}
	else if (std::string (mp.emphasis) == "air" && mp.sure && factories >= 1 && income >= kAirOnlyIncome)
	{
		p = eProgram::Strike;
		why = "enemy only beyond water and no sea near the base: planes are the way to reach him";
	}
	else if (factories >= 2 && ((income >= kAirIncome && turn >= kAirTurn) || (income >= kAirLateIncome && turn >= kAirLateTurn)))
	{
		p = eProgram::Strike;
		why = "economy is up (income " + std::to_string (income) + "): AWAC finds sides without AA, bombers hit mines and power";
	}
	else
		why = "economy first (income " + std::to_string (income) + ", factories " + std::to_string (factories) + ")";
	// опыт «вариант против варианта»: MAXR_AIR=0 — авиация выключена (программа «none», истребителей нет)
	const char* off = std::getenv ("MAXR_AIR");
	const bool disabled = off != nullptr && off[0] == '0';
	if (disabled)
	{
		p = eProgram::None;
		why = "MAXR_AIR=0";
	}
	if (p != program_ || (p != eProgram::None && why != lastProgramWhy_ && p == eProgram::Strike && antiSub_))
		event ("program", {{"from", programName()}, {"to", p == eProgram::Strike ? "strike" : "none"}, {"why", why}, {"income", income},
			{"emphasis", mp.emphasis}, {"sure", mp.sure}, {"enemyAir", enemyAir}, {"enemyAa", counts_.enemyAa}, {"subsNear", subsNear}});
	if (p != eProgram::Strike) strikeSince_ = -1;
	else if (strikeSince_ < 0) strikeSince_ = turn;
	program_ = p;
	why_ = why;
	lastProgramWhy_ = why;
	// истребители (П10): видна авиация врага — охотники (база — зенитками П5)
	fighterWant_ = disabled ? 0 : (enemyPlanes > 0 ? std::min (4, 1 + enemyPlanes / 2) : 0);
	if ((fighterWant_ > 0) != (lastFighterWant_ > 0)) // в журнал — только «появилась / пропала авиация врага»
		event ("fighters", {{"want", fighterWant_}, {"enemyPlanes", enemyPlanes}, {"enemyAir", enemyAir},
			{"why", fighterWant_ > 0 ? "enemy planes seen: hunt bombers, transports and AWAC outside their AA (base defence — AA guns)"
									 : "no enemy planes seen recently"}});
	lastFighterWant_ = fighterWant_;
	fightersWanted_ = fighterWant_ > 0;
	bomberWant_ = program_ == eProgram::Strike ? (antiSub_ ? 3 : (income >= 36 ? 4 : 2)) : 0;
	// авиатранспорт (П17): коммандос далеко идти (чужие, налёт, диверсия)
	transportWant_ = program_ == eProgram::Strike && (!commandos.ferryRequests().empty() || commandos.counts().commandos >= 2);
	wantFactory_ = !disabled && counts_.airFactories == 0 && factories >= 1 && (program_ == eProgram::Strike || (fightersWanted_ && income >= kAirLateIncome));
	wantHangar_ = counts_.airFactories > 0 && counts_.hangars == 0 && counts_.bombers + counts_.fighters + counts_.awacs > 0;
	urgent_ = antiSub_ || (fightersWanted_ && counts_.fighters == 0);
	// авиазавод и ангар раньше шахт: подлодки у верфи, упор «воздух» уверенно или (своя суша — остров: < kIslandSharePct всей суши,
	// или враг точно только за водой) программа «удар» ждёт авиазавод kFactoryWait ходов. Gen_Archipelago: после шахт авиазавод
	// был в 11 партиях из 32; на материках ранний авиазавод съедал экономику — Gen_Continents 0.542 → 0.430 (s13p3w)
	const bool island = mp.landSharePct > 0 && mp.landSharePct < kIslandSharePct;
	factoryUrgent_ = !disabled && (antiSub_ || (std::string (mp.emphasis) == "air" && mp.sure)
		|| ((island || (mp.enemyLand == 0 && mp.sure)) && program_ == eProgram::Strike && strikeSince_ >= 0 && turn - strikeSince_ >= kFactoryWait));

	// раз в несколько ходов — оценка сторон базы врага (журнал: AWAC ищет стороны без ПВО)
	if (focusKnown_ && program_ == eProgram::Strike && turn_ % 5 == 0) scoreSides (model, world);
	if (turn_ % kStatusEvery == 0 && (program_ != eProgram::None || counts_.planeMetal > 0)) event ("status", toJson());
}

//------------------------------------------------------------------------------
void cAirForce::scoreSides (const cModel& model, const cWorldModel& world)
{
	sideScore_.assign (8, 0);
	for (int d = 0; d < 8; ++d)
	{
		const cPosition st = onDir (W_, H_, focus_, d, kStageDist);
		if (aaDanger (st))
		{
			sideScore_[d] = -1;
			continue;
		}
		for (const auto& kv : world.enemies())
		{
			const int v = strikeValue (model, kv.second);
			if (v <= 0 || !kv.second.building) continue;
			if (dist2 (kv.second.pos, st) > 20 * 20) continue;
			bool ok = false;
			for (int dy = -4; dy <= 4 && !ok; ++dy)
				for (int dx = -4; dx <= 4 && !ok; ++dx)
				{
					const cPosition f = kv.second.pos + cPosition (dx, dy);
					ok = dx * dx + dy * dy <= 16 && f.x() >= 0 && f.y() >= 0 && f.x() < W_ && f.y() < H_ && !aaDanger (f);
				}
			if (ok) sideScore_[d] += v;
		}
	}
	int best = -1;
	for (int d = 0; d < 8; ++d)
		if (sideScore_[d] > 0 && (best < 0 || sideScore_[d] > sideScore_[best])) best = d;
	bestSide_ = best;
	if (best != lastBestSide_)
	{
		nlohmann::json sc = nlohmann::json::array();
		for (int d = 0; d < 8; ++d) sc.push_back (sideScore_[d]);
		event ("sides", {{"best", best}, {"scores", sc}, {"focus", posJ (focus_)}, {"enemyAa", counts_.enemyAa},
			{"why", best >= 0 ? "side with the most target value reachable outside AA" : "every side is covered by AA (or no targets seen)"}});
		lastBestSide_ = best;
	}
}

//------------------------------------------------------------------------------
sID cAirForce::nextPlane (const cModel& model, const cPlayer& player, const cRoleBook& book, cComposition& comp, const std::string& buildAs)
{
	(void) player;
	const auto& ud = *model.getUnitsData();
	auto typeOf = [&] (eRole r) {
		for (const auto& sd : ud.getStaticUnitsData())
			if (sd.ID.isAVehicle() && !sd.isAlien && sd.buildAs == buildAs && book.roleOfType (sd.ID) == r) return sd.ID;
		return sID();
	};
	struct sStep
	{
		eRole role;
		int want;
		const char* why;
	};
	std::vector<sStep> steps;
	if (antiSub_) steps.push_back ({eRole::Bomber, 2, "enemy subs at our shipyard or ships: bombers hit subs"});
	if (fightersWanted_) steps.push_back ({eRole::Fighter, std::min (2, fighterWant_), "enemy planes seen: hunters"});
	if (program_ == eProgram::Strike)
	{
		steps.push_back ({eRole::Awac, 1, "AWAC: find enemy AA and the sides without it (sees 12, AA hits 8)"});
		steps.push_back ({eRole::Bomber, 2, "bombers: hit mines and power where there is no AA, strike and leave"});
		if (transportWant_) steps.push_back ({eRole::AirTransport, 1, "air transport: commandos to far targets (aliens, raids)"});
		steps.push_back ({eRole::Bomber, bomberWant_, "more bombers (income allows)"});
	}
	if (fightersWanted_) steps.push_back ({eRole::Fighter, fighterWant_, "enemy planes seen: hunters"});
	if (fightersWanted_) steps.push_back ({eRole::AirTransport, 1, "enemy bombers seen: carry mobile AA to the threatened side (П17)"});
	nlohmann::json options = nlohmann::json::array();
	for (const auto& s : steps) options.push_back ({roleName (s.role), comp.count (s.role), s.want});
	const bool paused = turn_ < pauseUntil_;
	for (const auto& s : steps)
	{
		if (comp.count (s.role) >= s.want) continue;
		if (paused && !(antiSub_ && s.role == eRole::Bomber)) continue; // потери — пауза (кроме бомбардировщиков против подлодок)
		const sID t = typeOf (s.role);
		if (t == sID()) continue;
		comp.noteQueued (book, t);
		event ("build plane", {{"type", ud.getStaticUnitData (t).getDefaultName()}, {"role", roleName (s.role)}, {"why", s.why},
			{"program", programName()}, {"options", options}});
		return t;
	}
	return sID();
}

//------------------------------------------------------------------------------
int cAirForce::strikeValue (const cModel& model, const sEnemyMemory& m) const
{
	if (m.owner < 0) return 0;
	const auto& ud = *model.getUnitsData();
	const sID id (m.typeFirst, m.typeSecond);
	if (!ud.isValidId (id)) return 0;
	const auto& sd = ud.getStaticUnitData (id);
	if (m.plane) return 0; // бомбардировщик воздух не бьёт
	if (!m.building)
	{
		if (!m.visible) return 0; // машина — только видимая сейчас
		if (m.ship)
		{
			const bool sub = (sd.isStealthOn & eTerrainFlag::Sea) != 0;
			bool near = false;
			if (yard_.x() >= 0 && cheb (m.pos, yard_) <= kSubNear) near = true;
			if (sub) return near ? 300 : 150;
			return m.armed() ? (near ? 200 : 120) : 60;
		}
		if (sd.canAttack != 0 && m.range >= 6) return 55; // артиллерия, ракеты — бьют нашу армию издали
		if (!sd.canBuild.empty() || sd.vehicleData.canSurvey) return 40; // стройщики — экономика
		if (m.armed()) return 20;
		return 10;
	}
	const int cost = ud.getDynamicUnitData (id).getBuildCost();
	if (cost <= 2) return 0; // дороги, соединители, стены, платформы, мины
	if ((sd.canAttack & eTerrainFlag::Air) != 0) return 0; // зенитку бомбардировщик не бьёт: она бьёт дальше (8 против 4)
	if (sd.buildingData.canMineMaxRes > 0) return 100;
	if (sd.produceEnergy > 0 || sd.ID == sID (1, 7) || sd.ID == sID (1, 8)) return sd.buildingData.isBig ? 95 : 60;
	if (sd.canBuild == "Ship") return 85;
	if (sd.canBuild == "Plane") return 85;
	if (!sd.canBuild.empty()) return 70;
	if (sd.ID == sID (1, 26)) return 65; // радар
	if (sd.buildingData.canResearch || sd.buildingData.convertsGold > 0) return 55;
	if (sd.canAttack != 0) return 45; // орудия (турели, артиллерия, ракетные пушки)
	return 25;
}

//------------------------------------------------------------------------------
bool cAirForce::pickStrike (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& b, unsigned int& target,
	cPosition& fire, int& value, unsigned int ignoreAa, bool onlyNaval) const
{
	const int range = b.data.getRange();
	const int budget = b.data.getSpeed() - kReserve;
	if (range <= 0 || budget < 0 || b.data.getShots() <= 0 || b.data.getAmmo() <= 0) return false;
	const int reachCells = budget / 4 + range + 1;
	long long bestKey = std::numeric_limits<long long>::min();
	bool found = false;
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (cheb (m.pos, b.getPosition()) > reachCells) continue;
		if (onlyNaval && !m.ship) continue;
		const int v = strikeValue (model, m);
		if (v <= 0) continue;
		if (!targetAlive (model, *b.getOwner(), m)) continue;
		// цель видна или здание (здание стоит на месте; выстрел в конце пути — если увидим его с клетки удара)
		cPosition bestF (-1, -1);
		int bestCost = std::numeric_limits<int>::max();
		for (int dy = -range; dy <= range; ++dy)
			for (int dx = -range; dx <= range; ++dx)
			{
				if (dx * dx + dy * dy > range * range) continue;
				const cPosition f = m.pos + cPosition (dx, dy);
				if (!view.isValidPosition (f) || reservedCell (f) || aaDanger (f, ignoreAa)) continue;
				if (f != b.getPosition() && !view.possiblePlace (b, f)) continue;
				const int cost = flightCost (b.getPosition(), f);
				if (cost > budget || cost >= bestCost) continue;
				if (!seenAround (world, f)) continue;
				if (!m.building && !m.ship && (!seenWithin (world, f, 8, 0) || armyNear (f))) continue; // армия: ПВО рядом — видна ли сейчас
				bestCost = cost;
				bestF = f;
			}
		if (bestF.x() < 0) continue;
		const long long key = static_cast<long long> (v) * 1000 - bestCost;
		if (key > bestKey)
		{
			bestKey = key;
			target = m.id;
			fire = bestF;
			value = v;
			found = true;
		}
	}
	return found;
}

// выстрел с места по цели (видна и в дальности)
bool cAirForce::fireNow (const cModel& model, const cMapView& view, const cVehicle& v, unsigned int target, std::vector<cOrder>& out, const char* why)
{
	if (v.data.getShots() <= 0 || v.data.getAmmo() <= 0) return false;
	const cUnit* u = model.getUnitFromID (target);
	if (u == nullptr || !view.canSeeUnit (*u)) return false;
	for (const auto& cell : u->getPositions())
		if (v.canAttackObjectAt (cell, view, false))
		{
			cOrder o;
			o.type = eOrderType::Attack;
			o.playerId = playerId_;
			o.unitId = v.getId();
			o.position = cell;
			o.level = "micro";
			o.reason = std::string ("air: ") + why + " " + u->getStaticUnitData().getDefaultName();
			out.push_back (std::move (o));
			shots_.push_back ({v.getId(), target, u->getStaticUnitData().getDefaultName(), u->data.getHitpoints(), why, turn_});
			return true;
		}
	return false;
}

bool cAirForce::strike (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& b, std::vector<cOrder>& out,
	int pass, unsigned int ignoreAa, bool onlyNaval, const char* why)
{
	unsigned int target = kNoUnit;
	cPosition fire;
	int value = 0;
	if (!pickStrike (model, view, world, b, target, fire, value, ignoreAa, onlyNaval)) return false;
	const sEnemyMemory& m = world.enemies().at (target);
	const auto& ud = *model.getUnitsData();
	const std::string type = ud.getStaticUnitData (sID (m.typeFirst, m.typeSecond)).getDefaultName();
	const cUnit* u = model.getUnitFromID (target);
	bool ok = false;
	if (fire == b.getPosition())
		ok = fireNow (model, view, b, target, out, why);
	else
	{
		auto path = flyPath (b, view, fire, true, ignoreAa, 0);
		if (!path.empty() && path.back() == fire && pathCost (b, view, path) <= b.data.getSpeed() - kReserve)
		{
			ok = pushMove (b, std::move (path), std::string (why) + ": fly in and bomb " + type, out, target, eMoveEnd::Attack);
			if (ok && u != nullptr) shots_.push_back ({b.getId(), target, type, u->data.getHitpoints(), why, turn_});
		}
	}
	if (!ok) return false;
	++counts_.strikes;
	if (m.ship) ++counts_.subStrikes;
	strikes_[b.getId()] = {target, turn_, pass, false};
	event ("strike", {{"unitId", b.getId()}, {"target", target}, {"type", type}, {"value", value}, {"fire", posJ (fire)},
		{"from", posJ (b.getPosition())}, {"why", why}, {"pass", pass}, {"ignoredAa", ignoreAa == kNoUnit ? -1 : static_cast<long long> (ignoreAa)}});
	wantPass_ = true;
	return true;
}

// отход: из досягаемости ПВО и подальше от зданий врага (их обзор), к стоянке или домой
bool cAirForce::retreat (const cModel& model, const cMapView& view, const cVehicle& v, std::vector<cOrder>& out, const char* why)
{
	(void) model;
	if (v.data.getSpeed() <= 0) return false;
	cPosition anchor = home_;
	if (raid_.state != eRaid::Idle && raid_.staging.x() >= 0) anchor = raid_.staging;
	const cPosition dest = safeNear (view, v, anchor, 6, true);
	if (dest.x() < 0 || dest == v.getPosition()) return false;
	auto path = flyPath (v, view, dest, true, kNoUnit, 0);
	if (path.empty())
	{
		// в зоне ПВО сейчас — прочь любой ценой (к дому), без проверки клеток пути
		if (!aaDanger (v.getPosition())) return false;
		path = flyPath (v, view, dest, false, kNoUnit, 0);
	}
	return pushMove (v, std::move (path), why, out);
}

// ангар: без боезапаса или ранен — сесть, пополнить и починить (металл подбазы), выйти полным
bool cAirForce::service (const cModel& model, const cPlayer& player, const cMapView& view, const cVehicle& v, std::vector<cOrder>& out)
{
	const cBuilding* h = hangar (model);
	if (h == nullptr) return false;
	const cMap& map = *model.getMap();
	const auto& sd = v.getStaticUnitData();
	const bool armed = sd.canAttack != 0 && v.data.getAmmoMax() > 0;
	const bool needAmmo = armed && v.data.getAmmo() * 3 < v.data.getAmmoMax(); // меньше трети — на дозарядку
	const bool needRepair = hpPct (v) < kServiceHp;
	const bool inside = v.isUnitLoaded() && std::find (h->storedUnits.begin(), h->storedUnits.end(), &v) != h->storedUnits.end();
	if (inside)
	{
		serviceSet_.insert (v.getId());
		const bool fullAmmo = !armed || v.data.getAmmo() >= v.data.getAmmoMax();
		const bool fullHp = v.data.getHitpoints() >= v.data.getHitpointsMax();
		if (!fullAmmo && h->canSupply (&v, eSupplyType::REARM))
		{
			cOrder o;
			o.type = eOrderType::Rearm;
			o.playerId = playerId_;
			o.unitId = h->getId();
			o.unitId2 = v.getId();
			o.level = "micro";
			o.reason = "air: hangar rearms " + sd.getDefaultName();
			out.push_back (std::move (o));
		}
		if (!fullHp && h->canSupply (&v, eSupplyType::REPAIR))
		{
			cOrder o;
			o.type = eOrderType::Repair;
			o.playerId = playerId_;
			o.unitId = h->getId();
			o.unitId2 = v.getId();
			o.level = "micro";
			o.reason = "air: hangar repairs " + sd.getDefaultName();
			out.push_back (std::move (o));
		}
		if (fullAmmo && fullHp)
		{
			for (int dy = -1; dy <= 2; ++dy)
				for (int dx = -1; dx <= 2; ++dx)
				{
					const cPosition c = h->getPosition() + cPosition (dx, dy);
					if (!map.isValidPosition (c) || h->isNextTo (c) == false) continue;
					if (c.x() >= h->getPosition().x() && c.y() >= h->getPosition().y() && c.x() <= h->getPosition().x() + 1 && c.y() <= h->getPosition().y() + 1)
						continue;
					if (!h->canExitTo (c, map, sd) || reservedCell (c)) continue;
					cOrder o;
					o.type = eOrderType::ActivateUnit;
					o.playerId = playerId_;
					o.unitId = h->getId();
					o.unitId2 = v.getId();
					o.position = c;
					o.level = "micro";
					o.reason = "air: " + sd.getDefaultName() + " leaves the hangar (rearmed, repaired)";
					out.push_back (std::move (o));
					reserved_.push_back (c);
					serviceSet_.erase (v.getId());
					event ("serviced", {{"unitId", v.getId()}, {"type", sd.getDefaultName()}});
					return true;
				}
		}
		return true;
	}
	if (v.isUnitLoaded()) return true; // в транспорте или другом хранилище — не трогать
	if (!needAmmo && !needRepair)
	{
		serviceSet_.erase (v.getId());
		return false;
	}
	if (serviceSet_.insert (v.getId()).second)
		event ("to hangar", {{"unitId", v.getId()}, {"type", sd.getDefaultName()}, {"ammo", v.data.getAmmo()}, {"hp", v.data.getHitpoints()}});
	if (v.isUnitMoving() || v.getMoveJob() != nullptr) return true;
	if (h->isNextTo (v.getPosition()) && h->canLoad (&v))
	{
		cOrder o;
		o.type = eOrderType::Load;
		o.playerId = playerId_;
		o.unitId = h->getId();
		o.unitId2 = v.getId();
		o.level = "micro";
		o.reason = "air: " + sd.getDefaultName() + " lands in the hangar";
		out.push_back (std::move (o));
		wantPass_ = true;
		return true;
	}
	cPosition best (-1, -1);
	long long bestD = std::numeric_limits<long long>::max();
	for (int dy = -1; dy <= 2; ++dy)
		for (int dx = -1; dx <= 2; ++dx)
		{
			const cPosition c = h->getPosition() + cPosition (dx, dy);
			if (!map.isValidPosition (c) || !h->isNextTo (c)) continue;
			if (c.x() >= h->getPosition().x() && c.y() >= h->getPosition().y() && c.x() <= h->getPosition().x() + 1 && c.y() <= h->getPosition().y() + 1)
				continue;
			if (!view.possiblePlace (v, c) || reservedCell (c)) continue;
			const long long d = dist2 (c, v.getPosition());
			if (d < bestD)
			{
				bestD = d;
				best = c;
			}
		}
	if (best.x() < 0) return true;
	auto full = toVec (cPathCalculator (v, view, best, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
	const size_t fullN = full.size();
	auto path = full;
	cutToTurn (v, view, path);
	// путь домой в обход ПВО (если можно)
	if (!path.empty())
	{
		bool danger = false;
		for (const auto& p : path) danger = danger || aaDanger (p);
		if (danger)
		{
			auto safe = flyPath (v, view, best, true, kNoUnit, 0);
			if (!safe.empty()) path = std::move (safe);
		}
	}
	(void) player;
	if (path.empty()) return true;
	const bool arrive = path.size() == fullN && path.back() == best;
	pushMove (v, std::move (path), sd.getDefaultName() + (needAmmo ? " out of ammo" : " damaged") + ": to the hangar", out,
		arrive ? h->getId() : kNoUnit, arrive ? eMoveEnd::GetIn : eMoveEnd::Attack);
	return true;
}

//------------------------------------------------------------------------------
void cAirForce::checkShots (const cModel& model)
{
	for (const auto& s : std::exchange (shots_, {}))
	{
		const cUnit* t = model.getUnitFromID (s.target);
		const bool destroyed = t == nullptr;
		const int hp = t ? t->data.getHitpoints() : 0;
		const bool hit = destroyed || hp < s.hpBefore;
		if (hit) ++counts_.hits;
		if (destroyed)
		{
			++counts_.kills;
			deadTargets_.insert (s.target); // видели, как погибла (выстрел — только по видимой цели)
		}
		event ("shot result", {{"unitId", s.unit}, {"target", s.target}, {"type", s.type}, {"hpBefore", s.hpBefore}, {"hpAfter", hp},
			{"hit", hit}, {"destroyed", destroyed}, {"kind", s.what}});
	}
}

//------------------------------------------------------------------------------
// AWAC (П15): облёт базы врага по кольцу вне ПВО, у неё — короткими шагами; в налёт — над стороной удара
void cAirForce::awacPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& a,
	std::vector<cOrder>& out, int pass)
{
	(void) player;
	if (a.data.getSpeed() <= 0 || a.isUnitMoving()) return;
	// у базы врага — шагами (новую зенитку увидеть до входа в её огонь)
	bool nearEnemy = false;
	for (const auto& kv : world.enemies())
		if (kv.second.building && kv.second.owner >= 0 && cheb (kv.second.pos, a.getPosition()) <= kNearEnemy)
		{
			nearEnemy = true;
			break;
		}
	if (focusKnown_ && cheb (focus_, a.getPosition()) <= kNearEnemy) nearEnemy = true;
	const int cap = nearEnemy ? kAwacStep : 0;
	// в опасности (зенитку увидели) — прочь
	if (aaDanger (a.getPosition()))
	{
		retreat (model, view, a, out, "awac: AA spotted, back out of its reach");
		return;
	}
	cPosition goal;
	const char* why = "awac: circle the enemy base outside AA";
	if (raid_.state != eRaid::Idle && raid_.staging.x() >= 0)
	{
		// налёт — смотреть цель с безопасной клетки ближе к ней
		goal = raid_.targetPos;
		why = "awac: watch the raid target";
	}
	else
	{
		int& ring = awacRing_[a.getId()];
		const cPosition p = onDir (W_, H_, focus_, ring, kAwacRing);
		if (cheb (a.getPosition(), p) <= 2 || aaDanger (p))
		{
			// следующая точка — та, что дольше всего не видели
			int best = (ring + 1) % 8;
			int bestSeen = std::numeric_limits<int>::max();
			for (int k = 1; k <= 8; ++k)
			{
				const int d = (ring + k) % 8;
				const cPosition q = onDir (W_, H_, focus_, d, kAwacRing);
				const int s = world.seenTurn (q);
				if (s < bestSeen)
				{
					bestSeen = s;
					best = d;
				}
			}
			ring = best;
			event ("awac ring", {{"unitId", a.getId()}, {"dir", ring}, {"at", posJ (a.getPosition())}, {"enemyAa", counts_.enemyAa}, {"pass", pass}});
		}
		goal = onDir (W_, H_, focus_, ring, kAwacRing);
		if (!focusKnown_) why = "awac: look for the enemy base (opposite our base)";
	}
	// точка в ПВО — ближайшая безопасная на луче от базы врага
	if (aaDanger (goal))
	{
		const double ang = std::atan2 (static_cast<double> (goal.y() - focus_.y()), static_cast<double> (goal.x() - focus_.x()));
		const int r0 = cheb (goal, focus_);
		for (int r = r0 + 1; r <= r0 + 14; ++r)
		{
			const cPosition c = clampTo (W_, H_, focus_ + cPosition (static_cast<int> (std::lround (r * std::cos (ang))), static_cast<int> (std::lround (r * std::sin (ang)))));
			if (!aaDanger (c))
			{
				goal = c;
				break;
			}
		}
	}
	auto path = flyPath (a, view, goal, true, kNoUnit, cap);
	if (!path.empty())
	{
		pushMove (a, std::move (path), why, out);
		if (cap > 0) wantPass_ = true; // шаг — и ещё заход: посмотреть, что открылось
	}
}

//------------------------------------------------------------------------------
// истребитель (П10): охота на авиацию врага вне его ПВО — бомбардировщики, транспорты, AWAC; выстрел в конце подлёта, уход
void cAirForce::fighterPlan (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& f, std::vector<cOrder>& out,
	int pass)
{
	if (f.isUnitMoving()) return;
	if (const auto it = strikes_.find (f.getId()); it != strikes_.end() && it->second.turn == turn_)
	{
		if (!it->second.retreat && fireNow (model, view, f, it->second.target, out, "fighter: second shot")) return;
		it->second.retreat = true;
		retreat (model, view, f, out, "fighter: shot done, leave");
		return;
	}
	if (f.data.getShots() <= 0 || f.data.getAmmo() <= 0) return;
	const auto& ud = *model.getUnitsData();
	const int range = f.data.getRange();
	const int budget = f.data.getSpeed() - kReserve;
	unsigned int best = kNoUnit;
	cPosition bestF;
	long long bestKey = std::numeric_limits<long long>::min();
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.plane || !m.visible || m.owner < 0) continue;
		const sID id (m.typeFirst, m.typeSecond);
		if (!ud.isValidId (id)) continue;
		const auto& sd = ud.getStaticUnitData (id);
		int v = 40;
		if (sd.storageUnitsMax > 0) v = 90;                    // транспорт (с грузом — сумма)
		else if (m.armed() && (m.canAttack & (eTerrainFlag::Ground | eTerrainFlag::Sea)) != 0) v = 100; // бомбардировщик
		else if (!m.armed()) v = 80;                           // AWAC — «ослепить» его авиацию
		if (aaDanger (m.pos)) continue;                        // под его зенитками — не лезть
		cPosition bf (-1, -1);
		int bc = std::numeric_limits<int>::max();
		for (int dy = -range; dy <= range; ++dy)
			for (int dx = -range; dx <= range; ++dx)
			{
				if (dx * dx + dy * dy > range * range) continue;
				const cPosition c = m.pos + cPosition (dx, dy);
				if (!view.isValidPosition (c) || reservedCell (c) || aaDanger (c)) continue;
				if (c != f.getPosition() && !view.possiblePlace (f, c)) continue;
				const int cost = flightCost (f.getPosition(), c);
				if (cost > budget || cost >= bc) continue;
				bc = cost;
				bf = c;
			}
		if (bf.x() < 0) continue;
		const long long key = static_cast<long long> (v) * 1000 - bc;
		if (key > bestKey)
		{
			bestKey = key;
			best = m.id;
			bestF = bf;
		}
	}
	if (best == kNoUnit)
	{
		// без цели — у центра базы (дежурство: налёт врага рядом — догнать)
		if (home_.x() >= 0 && cheb (f.getPosition(), home_) > 4)
		{
			const cPosition d = safeNear (view, f, home_, 4, false);
			if (d.x() >= 0) pushMove (f, flyPath (f, view, d, true, kNoUnit, 0), "fighter: on duty over the base", out);
		}
		return;
	}
	const cUnit* u = model.getUnitFromID (best);
	const std::string type = u ? u->getStaticUnitData().getDefaultName() : std::string ("plane");
	bool ok = false;
	if (bestF == f.getPosition())
		ok = fireNow (model, view, f, best, out, "fighter: hunt");
	else
	{
		auto path = flyPath (f, view, bestF, true, kNoUnit, 0);
		if (!path.empty() && path.back() == bestF)
		{
			ok = pushMove (f, std::move (path), "fighter: hunt " + type + " outside its AA", out, best, eMoveEnd::Attack);
			if (ok && u) shots_.push_back ({f.getId(), best, type, u->data.getHitpoints(), "intercept", turn_});
		}
	}
	if (!ok) return;
	++counts_.intercepts;
	strikes_[f.getId()] = {best, turn_, pass, false};
	event ("intercept", {{"unitId", f.getId()}, {"target", best}, {"type", type}, {"fire", posJ (bestF)}, {"pass", pass}});
	wantPass_ = true;
}

//------------------------------------------------------------------------------
// авиатранспорт (П17): коммандос к далёкой цели — подлететь, взять, высадить вне ПВО у цели, домой
void cAirForce::ferryPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, cCommandos& commandos,
	std::vector<cOrder>& out, int pass)
{
	(void) world;
	const cMap& map = *model.getMap();
	const cVehicle* t = ferry_.transport != kNoUnit ? model.getVehicleFromID (ferry_.transport) : nullptr;
	if (t == nullptr)
	{
		if (ferry_.commando != kNoUnit) commandos.noteFerried (ferry_.commando, false); // транспорт погиб — коммандос свободен
		ferry_ = {};
		for (const auto& v : player.getVehicles())
			if (v->getStaticUnitData().factorAir > 0 && v->getStaticUnitData().storageUnitsMax > 0 && !v->isUnitLoaded() && serviceSet_.count (v->getId()) == 0)
			{
				t = v.get();
				ferry_.transport = v->getId();
				break;
			}
		if (t == nullptr) return;
	}
	claimed_.insert (t->getId());
	if (t->isUnitMoving() || t->isDisabled()) return;
	const cVehicle* c = ferry_.commando != kNoUnit ? model.getVehicleFromID (ferry_.commando) : nullptr;
	const bool cargoIn = c != nullptr && c->isUnitLoaded() && std::find (t->storedUnits.begin(), t->storedUnits.end(), c) != t->storedUnits.end();
	if (ferry_.commando != kNoUnit && c == nullptr)
	{
		commandos.noteFerried (ferry_.commando, false);
		ferry_.commando = kNoUnit;
		ferry_.state = 2;
	}
	// П17: идёт перевозка зенитки — её и продолжать
	if (aaFerry_.state != 0 && ferry_.commando == kNoUnit)
	{
		if (roles_ != nullptr) aaFerry (model, player, view, world, *roles_, *t, out, pass);
		return;
	}
	// нет задания — взять первую просьбу
	if (ferry_.commando == kNoUnit && ferry_.state != 2)
	{
		const cCommandos::sFerry* req = nullptr;
		for (const auto& r : commandos.ferryRequests())
			if (!r.stealthOnly)
			{
				req = &r;
				break;
			}
		if (req == nullptr)
		{
			// просьб коммандос нет — налёт врага у базы: зенитку на угрожаемую сторону
			if (roles_ != nullptr) aaFerry (model, player, view, world, *roles_, *t, out, pass);
			return;
		}
		const auto& r = *req;
		ferry_.commando = r.commando;
		ferry_.dest = r.dest;
		ferry_.state = 0;
		ferry_.since = turn_;
		c = model.getVehicleFromID (r.commando);
		commandos.noteFerried (r.commando, true); // коммандос ждёт транспорт на месте
		event ("ferry", {{"transport", t->getId()}, {"commando", r.commando}, {"dest", posJ (r.dest)}, {"why", r.why}});
	}
	if (ferry_.state == 0 && c != nullptr)
	{
		if (cargoIn)
		{
			ferry_.state = 1;
			commandos.noteFerried (c->getId(), true);
		}
		else if (turn_ - ferry_.since > 12)
		{
			event ("ferry dropped", {{"transport", t->getId()}, {"commando", ferry_.commando}, {"why", "could not pick up in 12 turns"}});
			commandos.noteFerried (ferry_.commando, false);
			ferry_.commando = kNoUnit;
			ferry_.state = 2;
		}
		else if (t->canLoad (c))
		{
			cOrder o;
			o.type = eOrderType::Load;
			o.playerId = playerId_;
			o.unitId = t->getId();
			o.unitId2 = c->getId();
			o.level = "micro";
			o.reason = "air: transport takes the commando";
			out.push_back (std::move (o));
			commandos.noteFerried (c->getId(), true);
			wantPass_ = true;
			return;
		}
		else if (t->data.getSpeed() > 0 && !c->isUnitMoving())
		{
			auto full = toVec (cPathCalculator (*t, view, c->getPosition(), static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
			const size_t n = full.size();
			auto path = full;
			cutToTurn (*t, view, path);
			bool safe = true;
			for (const auto& p : path) safe = safe && !aaDanger (p);
			if (!path.empty() && safe)
			{
				const bool arrive = path.size() == n;
				pushMove (*t, std::move (path), "transport flies to the commando", out, arrive ? c->getId() : kNoUnit,
					arrive ? eMoveEnd::Load : eMoveEnd::Attack);
				wantPass_ = true;
			}
			return;
		}
	}
	if (ferry_.state == 1 && c != nullptr && cargoIn)
	{
		// клетка высадки: ближе к цели (до 12 клеток: у зенитки налёта — за её досягаемостью, дальше коммандос пешком), суша
		// для коммандос, вне ПВО и вне радиуса сыщиков врага; транспорт там встанет
		if (ferry_.drop.x() < 0 || aaDanger (ferry_.drop))
		{
			ferry_.drop = cPosition (-1, -1);
			long long bestKey = std::numeric_limits<long long>::max();
			for (int dy = -12; dy <= 12; ++dy)
				for (int dx = -12; dx <= 12; ++dx)
				{
					const cPosition p = ferry_.dest + cPosition (dx, dy);
					if (!map.isValidPosition (p) || aaDanger (p) || commandos.detectorNear (p)) continue;
					if (!map.possiblePlaceVehicle (c->getStaticUnitData(), p, &player)) continue;
					if (p != t->getPosition() && !view.possiblePlace (*t, p)) continue;
					const long long key = dist2 (p, ferry_.dest) * 1000 + flightCost (t->getPosition(), p);
					if (key < bestKey)
					{
						bestKey = key;
						ferry_.drop = p;
					}
				}
			if (ferry_.drop.x() < 0)
			{
				event ("ferry dropped", {{"transport", t->getId()}, {"commando", ferry_.commando}, {"why", "no drop cell outside AA near the target"}});
				ferry_.state = 3; // выгрузить у себя
				ferry_.drop = t->getPosition();
			}
		}
		if (t->getPosition() == ferry_.drop || ferry_.state == 3)
		{
			if (t->canExitTo (t->getPosition(), map, c->getStaticUnitData()))
			{
				cOrder o;
				o.type = eOrderType::ActivateUnit;
				o.playerId = playerId_;
				o.unitId = t->getId();
				o.unitId2 = c->getId();
				o.position = t->getPosition();
				o.level = "micro";
				o.reason = "air: drop the commando near its target (unloading costs it no move)";
				out.push_back (std::move (o));
				++counts_.ferries;
				event ("dropped", {{"transport", t->getId()}, {"commando", c->getId()}, {"at", posJ (t->getPosition())}, {"pass", pass}});
				commandos.noteFerried (c->getId(), false);
				ferry_.commando = kNoUnit;
				ferry_.state = 2;
				ferry_.drop = cPosition (-1, -1);
				wantPass_ = true;
				return;
			}
			ferry_.drop = cPosition (-1, -1); // клетка не годна — выбрать другую в следующем заходе
			return;
		}
		auto path = flyPath (*t, view, ferry_.drop, true, kNoUnit, 0);
		if (!path.empty())
		{
			pushMove (*t, std::move (path), "transport carries the commando (cargo value: keep out of AA)", out);
			wantPass_ = true;
		}
		return;
	}
	if (ferry_.state >= 2 && pass == 1)
	{
		const cBuilding* h = hangar (model);
		const cPosition back = h ? h->getPosition() : home_;
		if (cheb (t->getPosition(), back) > 3)
		{
			const cPosition d = safeNear (view, *t, back, 4, false);
			if (d.x() >= 0) pushMove (*t, flyPath (*t, view, d, true, kNoUnit, 0), "transport back home", out);
		}
		else
		{
			ferry_.state = 0; // дома — свободен
			ferry_.commando = kNoUnit;
			ferry_.since = turn_;
		}
	}
}

//------------------------------------------------------------------------------
// П17: мобильная зенитка на угрожаемую сторону. Угроза — самолёт врага, что бьёт землю (виден за 2 хода), ближе 24 к центру
// базы; зенитка — своя, с выстрелом, не ближе 8 к угрозе, свободная; место — между угрозой и нашими зданиями, 4 клетки от
// самолёта к центру базы (зенитка бьёт на 7). Выгрузка выстрела не тратит (П6: своим ходом зенитка его потеряла бы)
bool cAirForce::aaFerry (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cVehicle& t, std::vector<cOrder>& out, int pass)
{
	(void) pass;
	const cMap& map = *model.getMap();
	const cVehicle* aa = aaFerry_.aa != kNoUnit ? model.getVehicleFromID (aaFerry_.aa) : nullptr;
	if (aaFerry_.state != 0 && (aa == nullptr || turn_ - aaFerry_.since > 4))
	{
		event ("aa ferry over", {{"aa", aaFerry_.aa}, {"why", aa == nullptr ? "aa lost" : "took too long"}});
		aaFerry_ = {};
		// груз остался внутри — выгрузить, где стоим
		for (const cVehicle* in : t.storedUnits)
			if (t.canExitTo (t.getPosition(), map, in->getStaticUnitData()))
			{
				cOrder o;
				o.type = eOrderType::ActivateUnit;
				o.playerId = playerId_;
				o.unitId = t.getId();
				o.unitId2 = in->getId();
				o.position = t.getPosition();
				o.level = "micro";
				o.reason = "air: transport unloads the AA where it is";
				out.push_back (std::move (o));
				break;
			}
		return true;
	}
	if (aaFerry_.state == 0)
	{
		if (home_.x() < 0) return false;
		// угроза: ударный самолёт врага у базы
		const sEnemyMemory* threat = nullptr;
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (!m.plane || !m.armed() || (m.canAttack & (eTerrainFlag::Ground | eTerrainFlag::Sea)) == 0 || world.age (m) > 2) continue;
			if (cheb (m.pos, home_) > 24) continue;
			if (threat == nullptr || dist2 (m.pos, home_) < dist2 (threat->pos, home_)) threat = &m;
		}
		if (threat == nullptr) return false;
		// место: от самолёта к центру базы на 4 клетки, суша для зенитки, не в зоне огня врага
		const cPosition dir (home_.x() > threat->pos.x() ? 1 : (home_.x() < threat->pos.x() ? -1 : 0),
			home_.y() > threat->pos.y() ? 1 : (home_.y() < threat->pos.y() ? -1 : 0));
		const cPosition want = threat->pos + dir * 4;
		const cVehicle* best = nullptr;
		long long bestD = std::numeric_limits<long long>::max();
		for (const auto& v : player.getVehicles())
		{
			if (roles.roleOf (*v) != eRole::AntiAir || v->isUnitLoaded() || v->isDisabled() || v->isUnitMoving() || v->getMoveJob() != nullptr) continue;
			if (v->data.getShots() <= 0 || cheb (v->getPosition(), threat->pos) <= 8 || cheb (v->getPosition(), t.getPosition()) > 30) continue;
			const long long d = dist2 (v->getPosition(), t.getPosition());
			if (d < bestD)
			{
				bestD = d;
				best = v.get();
			}
		}
		if (best == nullptr) return false;
		cPosition dest (-1, -1);
		long long bk = std::numeric_limits<long long>::max();
		for (int dy = -3; dy <= 3; ++dy)
			for (int dx = -3; dx <= 3; ++dx)
			{
				const cPosition q = want + cPosition (dx, dy);
				if (!map.isValidPosition (q) || !map.possiblePlaceVehicle (best->getStaticUnitData(), q, &player)) continue;
				if (world.groundFire (q) > 0 || aaDanger (q)) continue;
				if (q != t.getPosition() && !view.possiblePlace (t, q)) continue;
				const long long key = dist2 (q, want) * 1000 + static_cast<long long> (q.y()) * W_ + q.x();
				if (key < bk)
				{
					bk = key;
					dest = q;
				}
			}
		if (dest.x() < 0) return false;
		aaFerry_ = {best->getId(), dest, 1, turn_};
		aa = best;
		event ("aa ferry", {{"transport", t.getId()}, {"aa", best->getId()}, {"threat", posJ (threat->pos)}, {"dest", posJ (dest)},
			{"why", "enemy bomber near the base: the AA flown in keeps its shot (driving there would cost it, П6)"},
			{"options", "wounded to repair — not chosen (repairers and depot do it near the front); landing in the rear — commandos"}});
	}
	const bool inside = aa->isUnitLoaded() && std::find (t.storedUnits.begin(), t.storedUnits.end(), aa) != t.storedUnits.end();
	if (aaFerry_.state == 1)
	{
		if (inside)
			aaFerry_.state = 2;
		else if (t.canLoad (aa))
		{
			cOrder o;
			o.type = eOrderType::Load;
			o.playerId = playerId_;
			o.unitId = t.getId();
			o.unitId2 = aa->getId();
			o.level = "micro";
			o.reason = "air: transport takes the AA (П17)";
			out.push_back (std::move (o));
			wantPass_ = true;
			return true;
		}
		else
		{
			auto full = toVec (cPathCalculator (t, view, aa->getPosition(), static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
			const size_t n = full.size();
			auto path = full;
			cutToTurn (t, view, path);
			if (!path.empty())
			{
				const bool arrive = path.size() == n;
				pushMove (t, std::move (path), "transport flies to the AA (П17)", out, arrive ? aa->getId() : kNoUnit,
					arrive ? eMoveEnd::Load : eMoveEnd::Attack);
				wantPass_ = true;
			}
			return true;
		}
	}
	if (aaFerry_.state == 2 && inside)
	{
		if (t.getPosition() == aaFerry_.dest)
		{
			if (t.canExitTo (t.getPosition(), map, aa->getStaticUnitData()))
			{
				cOrder o;
				o.type = eOrderType::ActivateUnit;
				o.playerId = playerId_;
				o.unitId = t.getId();
				o.unitId2 = aa->getId();
				o.position = t.getPosition();
				o.level = "micro";
				o.reason = "air: drop the AA on the threatened side (keeps its shot, П6, П17)";
				out.push_back (std::move (o));
				++counts_.aaFerries;
				event ("aa dropped", {{"transport", t.getId()}, {"aa", aa->getId()}, {"at", posJ (t.getPosition())}});
				aaFerry_ = {};
				return true;
			}
			aaFerry_.dest = cPosition (-1, -1);
			aaFerry_.state = 0;
			return true;
		}
		auto path = flyPath (t, view, aaFerry_.dest, true, kNoUnit, 0);
		if (!path.empty())
		{
			pushMove (t, std::move (path), "transport carries the AA to the threatened side", out);
			wantPass_ = true;
		}
		return true;
	}
	return aaFerry_.state != 0;
}

//------------------------------------------------------------------------------
// налёт «AWAC + коммандос + бомбардировщики»: ценная цель прикрыта одной зениткой — коммандос выключает её в первом
// заходе хода удара, бомбардировщики бьют следом и уходят
void cAirForce::raidPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, cCommandos& commandos,
	std::vector<const cVehicle*>& bombers, std::vector<cOrder>& out, int pass)
{
	const auto& ud = *model.getUnitsData();
	auto armed = [&] () {
		std::vector<const cVehicle*> r;
		for (const cVehicle* b : bombers)
			if (b->data.getAmmo() > 0 && b->data.getShots() > 0 && !b->isUnitMoving() && serviceSet_.count (b->getId()) == 0) r.push_back (b);
		return r;
	};
	if (raid_.state == eRaid::Idle)
	{
		if (pass != 1 || program_ != eProgram::Strike || !focusKnown_ || counts_.awacs == 0) return;
		if (commandos.counts().commandos == 0) return;
		if (armed().empty()) return;
		// цель — здание ценностью ≥ 60, у которого клетки удара прикрыты ровно одной зениткой-зданием (её можно выключить)
		long long bestKey = std::numeric_limits<long long>::min();
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (!m.building) continue;
			const int v = strikeValue (model, m);
			if (v < 60 || !targetAlive (model, player, m)) continue;
			for (int dy = -4; dy <= 4; ++dy)
				for (int dx = -4; dx <= 4; ++dx)
				{
					if (dx * dx + dy * dy > 16) continue;
					const cPosition f = m.pos + cPosition (dx, dy);
					if (!view.isValidPosition (f) || !seenAround (world, f)) continue;
					const sAaSource* only = nullptr;
					int cover = 0;
					for (const auto& s : aa_)
					{
						const int r = s.big ? s.reach + 1 : s.reach;
						if (dist2 (f, s.pos) <= static_cast<long long> (r) * r)
						{
							++cover;
							only = &s;
						}
					}
					if (cover != 1 || !only->building) continue;
					const auto mit = world.enemies().find (only->id);
					if (mit == world.enemies().end()) continue;
					const sID aid (mit->second.typeFirst, mit->second.typeSecond);
					if (!ud.isValidId (aid) || !ud.getStaticUnitData (aid).canBeDisabled) continue;
					// зенитку стережёт пехота (все клетки рядом — в обзоре известных сыщиков врага) — коммандос не подойти; свой
					// коммандос — не дальше kRaidWalk от неё (налёт s13p3p: 12 из 21 сняты «нет клетки рядом вне обзора пехоты»)
					if (commandos.nearestFree (model, world, only->pos) > kRaidWalk) continue;
					bool open = false;
					for (int ey = -1; ey <= 1 && !open; ++ey)
						for (int ex = -1; ex <= 1 && !open; ++ex)
						{
							const cPosition q = only->pos + cPosition (ex, ey);
							open = (ex != 0 || ey != 0) && view.isValidPosition (q) && !commandos.detectorNear (q);
						}
					if (!open) continue;
					const long long key = static_cast<long long> (v) * 1000 - cheb (f, home_);
					if (key > bestKey)
					{
						bestKey = key;
						raid_.target = m.id;
						raid_.targetPos = m.pos;
						raid_.aa = only->id;
						raid_.aaPos = only->pos;
						raid_.fire = f;
					}
				}
		}
		if (raid_.target == kNoUnit) return;
		// коммандос к зенитке идёт пешком (не дальше kRaidWalk): авиатранспорт с ним над базой врага сбивают
		if (commandos.nearestFree (model, world, raid_.aaPos) > kRaidWalk)
		{
			raid_ = {};
			return;
		}
		// стоянка: безопасная клетка в 8–12 клетках от клетки удара, дальше от зенитки
		raid_.staging = cPosition (-1, -1);
		{
			long long bk = std::numeric_limits<long long>::max();
			for (int dy = -12; dy <= 12; ++dy)
				for (int dx = -12; dx <= 12; ++dx)
				{
					const int r = std::max (std::abs (dx), std::abs (dy));
					if (r < 8) continue;
					const cPosition c = raid_.fire + cPosition (dx, dy);
					if (!view.isValidPosition (c) || aaDanger (c)) continue;
					const long long key = -dist2 (c, raid_.aaPos) + dist2 (c, home_) / 4;
					if (key < bk)
					{
						bk = key;
						raid_.staging = c;
					}
				}
		}
		if (raid_.staging.x() < 0)
		{
			raid_ = {};
			return;
		}
		raid_.state = eRaid::Prepare;
		raid_.since = turn_;
		commandos.requestRaid (raid_.aa, raid_.aaPos, false, turn_);
		const auto& tm = world.enemies().at (raid_.target);
		const auto& am = world.enemies().at (raid_.aa);
		event ("raid planned", {{"target", raid_.target}, {"targetType", ud.getStaticUnitData (sID (tm.typeFirst, tm.typeSecond)).getDefaultName()},
			{"aa", raid_.aa}, {"aaType", ud.getStaticUnitData (sID (am.typeFirst, am.typeSecond)).getDefaultName()},
			{"fire", posJ (raid_.fire)}, {"staging", posJ (raid_.staging)},
			{"why", "the target is covered by one AA only: the commando silences it, the bombers strike in the same turn"}});
		return;
	}
	// цель или зенитки больше нет (в памяти) — снять
	if (world.enemies().count (raid_.target) == 0 || (raid_.state == eRaid::Prepare && world.enemies().count (raid_.aa) == 0))
	{
		event ("raid done", {{"target", raid_.target}, {"why", "target or AA gone from memory"}, {"strikeTurn", raid_.strikeTurn}});
		commandos.cancelRaid ("air raid over");
		raid_ = {};
		return;
	}
	if (raid_.state == eRaid::Prepare)
	{
		if (turn_ - raid_.since > kRaidPrepare)
		{
			event ("raid cancelled", {{"target", raid_.target}, {"why", "prepared too long"}});
			commandos.cancelRaid ("air raid prepared too long");
			raid_ = {};
			return;
		}
		if (commandos.raidTarget() != raid_.aa)
		{
			// коммандос отказался (обнаружен, нет пути) — налёт снят
			event ("raid cancelled", {{"target", raid_.target}, {"why", "no commando for the AA"}});
			raid_ = {};
			return;
		}
		const auto ready = armed();
		std::vector<const cVehicle*> inReach;
		for (const cVehicle* b : ready)
			if (flightCost (b->getPosition(), raid_.fire) <= b->data.getSpeedMax() - kReserve) inReach.push_back (b);
		if (pass == 1 && commandos.raidReady (model) && !inReach.empty())
		{
			// ход удара: заход 1 — выключить
			if (commandos.raidDisable (model, player, out, pass))
			{
				raid_.state = eRaid::Strike;
				raid_.phase = 2;
				raid_.strikeTurn = turn_;
				++counts_.raids;
				event ("raid strike turn", {{"target", raid_.target}, {"aa", raid_.aa}, {"bombers", inReach.size()}, {"pass", pass}});
				for (const cVehicle* b : bombers) claimed_.insert (b->getId());
				wantPass_ = true;
			}
			return;
		}
		// бомбардировщики — на стоянку
		if (pass == 1)
			for (const cVehicle* b : ready)
				if (cheb (b->getPosition(), raid_.staging) > 2)
				{
					const cPosition d = safeNear (view, *b, raid_.staging, 3, false);
					if (d.x() >= 0) pushMove (*b, flyPath (*b, view, d, true, kNoUnit, 0), "bomber to the raid staging point", out);
				}
		return;
	}
	if (raid_.state == eRaid::Strike)
	{
		if (raid_.strikeTurn != turn_)
		{
			event ("raid done", {{"target", raid_.target}, {"why", "strike turn over"}});
			commandos.cancelRaid ("air raid over");
			raid_ = {};
			return;
		}
		const cUnit* aa = model.getUnitFromID (raid_.aa);
		if (raid_.phase == 2)
		{
			if (aa != nullptr && !aa->isDisabled())
			{
				event ("raid aborted", {{"target", raid_.target}, {"aa", raid_.aa}, {"why", "the commando failed to disable the AA"}, {"pass", pass}});
				commandos.cancelRaid ("disable failed");
				raid_ = {};
				return;
			}
			++counts_.raidDisables;
			bool any = false;
			for (const cVehicle* b : armed())
			{
				claimed_.insert (b->getId());
				any = strike (model, view, world, *b, out, pass, raid_.aa, false, "raid") || any;
			}
			raid_.phase = 3;
			event ("raid bombs", {{"target", raid_.target}, {"aa", raid_.aa}, {"struck", any}, {"pass", pass}});
			wantPass_ = true;
			return;
		}
		if (raid_.phase >= 3)
		{
			raid_.phase = 4;
			return; // второй выстрел и отход — общим порядком удара (strikes_)
		}
	}
}

//------------------------------------------------------------------------------
void cAirForce::plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, cCommandos& commandos, std::vector<cOrder>& out)
{
	(void) defense;
	roles_ = &roles;
	claimed_.clear();
	reserved_.clear();
	wantPass_ = false;
	checkShots (model);
	for (auto it = strikes_.begin(); it != strikes_.end();)
		it = it->second.turn == turn_ ? std::next (it) : strikes_.erase (it);
	std::vector<const cVehicle*> planes;
	for (const auto& v : player.getVehicles())
		if (isAirRole (roles.roleOf (*v)))
		{
			planes.push_back (v.get());
			claimed_.insert (v->getId());
		}
	// обслуживание в ангаре — раньше всего
	std::vector<const cVehicle*> free;
	for (const cVehicle* v : planes)
	{
		if (v->isDisabled()) continue;
		if (ferry_.transport == v->getId()) continue;
		if (service (model, player, view, *v, out)) continue;
		if (v->isUnitLoaded()) continue;
		free.push_back (v);
	}
	// налёт с коммандос — до одиночных ударов
	std::vector<const cVehicle*> bombers;
	for (const cVehicle* v : free)
		if (roles.roleOf (*v) == eRole::Bomber) bombers.push_back (v);
	raidPlan (model, player, view, world, commandos, bombers, out, 1);
	const bool raidStrike = raid_.state == eRaid::Strike;
	for (const cVehicle* v : free)
	{
		const eRole r = roles.roleOf (*v);
		if (v->isUnitMoving() || v->getMoveJob() != nullptr) continue;
		if (r == eRole::Awac)
		{
			awacPlan (model, player, view, world, *v, out, 1);
			continue;
		}
		if (r == eRole::Fighter)
		{
			fighterPlan (model, view, world, *v, out, 1);
			continue;
		}
		if (r == eRole::Bomber)
		{
			if (raidStrike) continue; // ход удара налёта: бомбы — во втором заходе
			bool ordered = false;
			for (const auto& o : out)
				if (o.unitId == v->getId()) ordered = true;
			if (ordered) continue;
			// в зоне ПВО — прочь
			if (aaDanger (v->getPosition()))
			{
				retreat (model, view, *v, out, "bomber: inside AA reach, leave");
				continue;
			}
			// подлодки и корабли у нашей верфи (п. 100) — первыми; иначе цель вне ПВО
			if (strike (model, view, world, *v, out, 1, kNoUnit, true, "anti-ship")) continue;
			// бомбардировщик есть (свой — по программе, захваченный самолёт чужих — без неё) — бить
			if (strike (model, view, world, *v, out, 1, kNoUnit, false, "strike")) continue;
			// без цели в досягаемости: готовится налёт — на его стоянке; база врага известна — к стоянке у неё (вне ПВО),
			// иначе — у ангара
			cPosition anchor = home_;
			const char* why = "bomber: wait at the base";
			if (raid_.state == eRaid::Prepare && raid_.staging.x() >= 0)
			{
				anchor = raid_.staging;
				why = "bomber: wait at the raid staging point (the commando sneaks to the AA)";
			}
			else if (antiSub_ && yard_.x() >= 0)
			{
				anchor = yard_;
				why = "bomber: guard our shipyard (enemy subs near: bombers hit subs)";
			}
			else if (program_ == eProgram::Strike && focusKnown_ && !antiSub_)
			{
				// стоянка: в kStageDist клетках от базы врага на лучшей стороне (иначе — на стороне к нам), вне ПВО
				const int side = bestSide_ >= 0 ? bestSide_
					: static_cast<int> (std::lround (std::atan2 (static_cast<double> (home_.y() - focus_.y()), static_cast<double> (home_.x() - focus_.x())) / (kPi / 4.0)) + 8) % 8;
				anchor = onDir (W_, H_, focus_, side, kStageDist);
				// не ближе kStageGap к зданиям врага: мобильная ПВО и зенитки, которых мы не видели, — у его базы
				for (int r = kStageDist; r <= kStageDist + 20; r += 2)
				{
					const cPosition q = onDir (W_, H_, focus_, side, r);
					bool close = false;
					for (const auto& kv : world.enemies())
						if (kv.second.building && kv.second.owner >= 0 && cheb (kv.second.pos, q) < kStageGap) close = true;
					anchor = q;
					if (!close && !aaDanger (q)) break;
				}
				why = "bomber: to the staging point near the enemy base (outside AA, 20 cells from his buildings)";
			}
			else if (const cBuilding* h = hangar (model))
				anchor = h->getPosition();
			if (cheb (v->getPosition(), anchor) > 3)
			{
				const cPosition d = safeNear (view, *v, anchor, 5, false);
				if (d.x() >= 0) pushMove (*v, flyPath (*v, view, d, true, kNoUnit, 0), why, out);
			}
			continue;
		}
	}
	// перевозка коммандос
	ferryPlan (model, player, view, world, commandos, out, 1);
	// прочие самолёты без дела (транспорт без задания) — у ангара
}

//------------------------------------------------------------------------------
std::vector<cOrder> cAirForce::planPass (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
	const cRoleBook& roles, cCommandos& commandos, int pass)
{
	std::vector<cOrder> out;
	roles_ = &roles;
	wantPass_ = false;
	checkShots (model);
	std::vector<const cVehicle*> bombers;
	for (const auto& v : player.getVehicles())
		if (roles.roleOf (*v) == eRole::Bomber && !v->isUnitLoaded() && !v->isDisabled()) bombers.push_back (v.get());
	raidPlan (model, player, view, world, commandos, bombers, out, pass);
	for (const auto& v : player.getVehicles())
	{
		const eRole r = roles.roleOf (*v);
		if (!isAirRole (r) || v->isUnitLoaded() || v->isDisabled() || v->isUnitMoving() || v->getMoveJob() != nullptr) continue;
		bool ordered = false;
		for (const auto& o : out)
			if (o.unitId == v->getId()) ordered = true;
		if (ordered) continue;
		if (r == eRole::Awac)
		{
			// AWAC у базы врага — ещё шаг (обзор открылся)
			awacPlan (model, player, view, world, *v, out, pass);
			continue;
		}
		const auto it = strikes_.find (v->getId());
		if (it == strikes_.end() || it->second.turn != turn_) continue;
		// после удара: второй выстрел, затем отход («ударил — ушёл»)
		if (!it->second.retreat && fireNow (model, view, *v, it->second.target, out, r == eRole::Fighter ? "fighter: second shot" : "bomber: second shot"))
		{
			it->second.retreat = true;
			wantPass_ = true;
			continue;
		}
		it->second.retreat = true;
		if (retreat (model, view, *v, out, r == eRole::Fighter ? "fighter: hit and leave" : "bomber: hit and leave")) wantPass_ = true;
	}
	ferryPlan (model, player, view, world, commandos, out, pass);
	return out;
}

} // namespace ai
