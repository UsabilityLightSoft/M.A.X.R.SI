/***************************************************************************
 * ai_world_model.cpp — память о враге и карта угроз (шаг 7 PLAN.md).
 ***************************************************************************/

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
#include <cstdlib>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace ai
{

namespace
{

// записать наблюдаемый юнит в память
void observe (sEnemyMemory& m, const cUnit& u, bool isBuilding, int turn)
{
	const auto& sd = u.getStaticUnitData();
	m.id = u.getId();
	m.owner = u.getOwner() ? u.getOwner()->getId() : -1;
	m.typeFirst = sd.ID.firstPart;
	m.typeSecond = sd.ID.secondPart;
	m.building = isBuilding;
	m.big = u.getIsBig();
	m.plane = !isBuilding && sd.factorAir > 0;
	m.ship = !isBuilding && sd.factorSea > 0 && sd.factorGround == 0;
	m.pos = u.getPosition();
	m.seenTurn = turn;
	m.visible = true;
	m.damage = u.data.getDamage();
	m.shots = std::min (u.data.getShotsMax(), u.data.getAmmo());
	m.range = u.data.getRange();
	m.armor = u.data.getArmor();
	m.hp = u.data.getHitpoints();
	m.hpMax = u.data.getHitpointsMax();
	m.speed = isBuilding ? 0 : u.data.getSpeedMax() / 4;
	m.canAttack = static_cast<int> (sd.canAttack);
	m.shotsMax = u.data.getShotsMax();
	m.scan = u.data.getScan();
	m.disabled = u.isDisabled();
	m.driveFire = !isBuilding && sd.vehicleData.canDriveAndFire;
	// сколько проедет и ещё выстрелит: движение режет выстрелы (cVehicle::DecSpeed:
	// выстрелов не больше остаток хода × выстрелы / ход), клетка по суше — 4 очка хода
	m.moveFire = 0;
	if (!isBuilding && m.shotsMax > 0)
	{
		const int speedMax = u.data.getSpeedMax();
		if (m.driveFire)
			m.moveFire = speedMax / 4;
		else if (m.shotsMax >= 2)
			m.moveFire = (speedMax - (speedMax + m.shotsMax - 1) / m.shotsMax) / 4;
	}
}

} // namespace

//------------------------------------------------------------------------------
int sEnemyMemory::damageVs (int targetArmor) const
{
	if (!armed()) return 0;
	return std::max (1, damage - targetArmor) * shots;
}

//------------------------------------------------------------------------------
void cWorldModel::update (const cModel& model, const cPlayer& self, int turn)
{
	turn_ = turn;
	const cMap& map = *model.getMap();
	map_ = &map;
	for (auto& kv : memory_)
		kv.second.visible = false;

	// какие клетки видны сейчас (шаг 8: разведка идёт к невиданным)
	w_ = map.getSize().x();
	h_ = map.getSize().y();
	// шаг 13: подпись мостов и платформ на воде и берегу (области проходимости наземных классов)
	{
		unsigned long long sign = 0;
		for (const auto& player : model.getPlayerList())
			for (const auto& b : player->getBuildings())
			{
				const auto sp = b->getStaticUnitData().surfacePosition;
				if (sp != eSurfacePosition::AboveSea && sp != eSurfacePosition::Base && sp != eSurfacePosition::AboveBase) continue;
				const cPosition p = b->getPosition();
				if (!map.isValidPosition (p) || (!map.isWater (p) && !map.isCoast (p))) continue;
				sign += (static_cast<unsigned long long> (p.y()) * w_ + p.x() + 1) * 2654435761ULL;
			}
		bridgeSign_ = static_cast<long long> (sign & 0x7FFFFFFFFFFFFFFFULL);
	}
	if (seen_.size() != static_cast<size_t> (w_) * h_) seen_.assign (static_cast<size_t> (w_) * h_, -1);
	for (int y = 0; y < h_; ++y)
		for (int x = 0; x < w_; ++x)
			if (self.canSeeAt (cPosition (x, y))) seen_[static_cast<size_t> (y) * w_ + x] = turn;

	// видимые сейчас враги — в память (как digest: cPlayer::canSeeUnit)
	for (const auto& player : model.getPlayerList())
	{
		if (player->getId() == self.getId()) continue;
		for (const auto& v : player->getVehicles())
		{
			if (!self.canSeeUnit (*v, map)) continue;
			observe (memory_[v->getId()], *v, false, turn);
		}
		for (const auto& b : player->getBuildings())
		{
			if (!self.canSeeUnit (*b, map)) continue;
			observe (memory_[b->getId()], *b, true, turn);
		}
	}

	// забыть то, чего больше нет: клетка видна, а юнита на ней не видно
	for (auto it = memory_.begin(); it != memory_.end();)
	{
		const sEnemyMemory& m = it->second;
		bool forget = false;
		if (!m.visible)
		{
			if (m.building)
			{
				const int s = m.big ? 2 : 1;
				for (int dy = 0; dy < s && !forget; ++dy)
					for (int dx = 0; dx < s && !forget; ++dx)
					{
						const cPosition c = m.pos + cPosition (dx, dy);
						if (map.isValidPosition (c) && self.canSeeAt (c)) forget = true;
					}
			}
			else
				forget = turn - m.seenTurn > kForgetVehicleTurns
					|| (map.isValidPosition (m.pos) && self.canSeeAt (m.pos));
		}
		if (forget)
			it = memory_.erase (it);
		else
			++it;
	}
}

//------------------------------------------------------------------------------
void cWorldModel::noteStolen (const cVehicle& vehicle, int turn)
{
	observe (memory_[vehicle.getId()], vehicle, false, turn);
}

//------------------------------------------------------------------------------
void cWorldModel::buildThreat (const cMap& map, int armorGround, int armorAir)
{
	w_ = map.getSize().x();
	h_ = map.getSize().y();
	ground_.assign (static_cast<size_t> (w_) * h_, 0);
	air_.assign (static_cast<size_t> (w_) * h_, 0);
	fire_.assign (static_cast<size_t> (w_) * h_, 0);
	strike_.assign (static_cast<size_t> (w_) * h_, 0);
	auto paint = [&] (std::vector<int>& grid, const sEnemyMemory& m, int value, bool rangeOnly = false, int radius = -1) {
		if (value <= 0) return;
		const int r = radius >= 0 ? radius : (rangeOnly ? m.range : m.reach());
		// здание 2×2 бьёт из любой своей клетки — считаем от центра с запасом в клетку
		const int rr = m.big ? r + 1 : r;
		for (int dy = -rr; dy <= rr; ++dy)
			for (int dx = -rr; dx <= rr; ++dx)
			{
				if (dx * dx + dy * dy > rr * rr) continue;
				const int x = m.pos.x() + dx;
				const int y = m.pos.y() + dy;
				if (x < 0 || y < 0 || x >= w_ || y >= h_) continue;
				grid[static_cast<size_t> (y) * w_ + x] += value;
			}
	};
	for (const auto& kv : memory_)
	{
		const sEnemyMemory& m = kv.second;
		// шаг 9: самолёты — не в наземных слоях: бомбардировщик достаёт за ход клетки на 22 вокруг,
		// от него не спрятаться — с ним стройка вставала (опыт ПВО: ни одной площадки у базы), а ответ
		// на него — ПВО (ai_roles), не обход
		if (m.plane)
		{
			if (m.hitsAir()) paint (air_, m, m.damageVs (armorAir));
			continue;
		}
		if (m.hitsGround()) paint (ground_, m, m.damageVs (armorGround));
		if (m.hitsAir()) paint (air_, m, m.damageVs (armorAir));
		// зона огня с места: видимые сейчас (или прошлым ходом) и здания; Р6 (правка 17): выключенный, видимый
		// выключенным сейчас, в наш ход не стреляет (ни в дозоре, ни ответным огнём)
		if (m.hitsGround() && (m.building || turn_ - m.seenTurn <= 1) && !(m.visible && m.disabled)) paint (fire_, m, m.damageVs (armorGround), true);
		// шаг 9: «удар с подъезда» — те же враги, дальность + сколько проедут с выстрелом
		if (m.hitsGround() && (m.building || turn_ - m.seenTurn <= 1)) paint (strike_, m, m.damageVs (armorGround), false, m.strike());
	}
}

//------------------------------------------------------------------------------
int cWorldModel::threatFor (const cVehicle& v, const cPosition& p, eThreatLayer layer) const
{
	if (layer == eThreatLayer::None) return 0;
	if (v.getStaticUnitData().factorAir > 0) return airThreat (p);
	if (layer == eThreatLayer::Strike) return groundStrike (p);
	return layer == eThreatLayer::Fire ? groundFire (p) : groundThreat (p);
}

//------------------------------------------------------------------------------
bool cWorldModel::pathUnsafe (const cVehicle& v, const std::vector<cPosition>& path, eThreatLayer layer) const
{
	if (layer == eThreatLayer::None) return false;
	for (const auto& p : path)
		if (threatFor (v, p, layer) > 0) return true;
	return false;
}

//------------------------------------------------------------------------------
// шаг 13: класс движения машины — биты: наземная 1, по воде 2, по берегу 4; самолёт — −1
int cWorldModel::moveClass (const cVehicle& v)
{
	const auto& sd = v.getStaticUnitData();
	if (sd.factorAir > 0) return -1;
	return (sd.factorGround > 0 ? 1 : 0) | (sd.factorSea > 0 ? 2 : 0) | (sd.factorCoast > 0 ? 4 : 0);
}

//------------------------------------------------------------------------------
// области проходимости класса cls (без машин): клетка годна, как в cMap::possiblePlaceVehicle, с
// запасом — у наземной машины вода и берег годны и с мостом, платформой, дорогой (любое здание с
// поверхностью AboveSea, Base, AboveBase на клетке); 8 соседей, как у A*
const cWorldModel::sRegions& cWorldModel::regions (int cls) const
{
	sRegions& r = regions_[cls];
	const size_t N = static_cast<size_t> (w_) * h_;
	// местность статична: пересчёт — только когда сменились мосты и платформы (у наземных классов)
	const long long sign = (cls & 1) != 0 ? bridgeSign_ : 0;
	if (r.bridges == sign && r.label.size() == N) return r;
	r.bridges = sign;
	r.label.assign (N, -1);
	r.sizes.clear();
	if (map_ == nullptr || N == 0) return r;
	const cMap& map = *map_;
	const bool ground = (cls & 1) != 0, sea = (cls & 2) != 0, coast = (cls & 4) != 0;
	auto passable = [&] (const cPosition& p) {
		if (map.isBlocked (p)) return false;
		const bool water = map.isWater (p);
		const bool isCoast = map.isCoast (p);
		if (ground)
		{
			if (!water && !isCoast) return true;
			if ((water && sea) || (isCoast && coast)) return true;
			for (const cBuilding* b : map.getField (p).getBuildings())
			{
				const auto sp = b->getStaticUnitData().surfacePosition;
				if (sp == eSurfacePosition::AboveSea || sp == eSurfacePosition::Base || sp == eSurfacePosition::AboveBase) return true;
			}
			return false;
		}
		if (sea) return water || (isCoast && coast);
		return false;
	};
	std::vector<char> ok (N, 0);
	for (int y = 0; y < h_; ++y)
		for (int x = 0; x < w_; ++x)
			ok[static_cast<size_t> (y) * w_ + x] = passable (cPosition (x, y)) ? 1 : 0;
	std::vector<int> queue;
	for (size_t s = 0; s < N; ++s)
	{
		if (!ok[s] || r.label[s] >= 0) continue;
		const int id = static_cast<int> (r.sizes.size());
		r.label[s] = id;
		queue.assign (1, static_cast<int> (s));
		for (size_t qi = 0; qi < queue.size(); ++qi)
		{
			const int cx = queue[qi] % w_;
			const int cy = queue[qi] / w_;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = cx + dx, ny = cy + dy;
					if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_) continue;
					const size_t o = static_cast<size_t> (ny) * w_ + nx;
					if (!ok[o] || r.label[o] >= 0) continue;
					r.label[o] = id;
					queue.push_back (static_cast<int> (o));
				}
		}
		r.sizes.push_back (static_cast<int> (queue.size()));
	}
	return r;
}

//------------------------------------------------------------------------------
int cWorldModel::regionOf (const cVehicle& v, const cPosition& p) const
{
	if (p.x() < 0 || p.y() < 0 || p.x() >= w_ || p.y() >= h_) return -1;
	const int cls = moveClass (v);
	if (cls < 0) return 0;
	return regions (cls).label[static_cast<size_t> (p.y()) * w_ + p.x()];
}

//------------------------------------------------------------------------------
int cWorldModel::regionSize (const cVehicle& v, int region) const
{
	const int cls = moveClass (v);
	if (cls < 0) return w_ * h_;
	const auto& r = regions (cls);
	return region >= 0 && region < static_cast<int> (r.sizes.size()) ? r.sizes[region] : 0;
}

//------------------------------------------------------------------------------
int cWorldModel::tankRegionOf (const cPosition& p) const
{
	if (p.x() < 0 || p.y() < 0 || p.x() >= w_ || p.y() >= h_) return -1;
	return regions (1).label[static_cast<size_t> (p.y()) * w_ + p.x()];
}

//------------------------------------------------------------------------------
int cWorldModel::tankRegionSize (int region) const
{
	const auto& r = regions (1);
	return region >= 0 && region < static_cast<int> (r.sizes.size()) ? r.sizes[region] : 0;
}

//------------------------------------------------------------------------------
// новый поиск: буферы — по размеру карты (заводятся раз), поколение — метка годных клеток
unsigned int cWorldModel::beginPathSearch (size_t n) const
{
	if (pathG_.size() != n)
	{
		pathG_.assign (n, 0);
		pathPrev_.assign (n, -1);
		pathSeen_.assign (n, 0);
		pathClosed_.assign (n, 0);
		pathGen_ = 0;
	}
	if (++pathGen_ == 0)
	{
		std::fill (pathSeen_.begin(), pathSeen_.end(), 0);
		std::fill (pathClosed_.begin(), pathClosed_.end(), 0);
		pathGen_ = 1;
	}
	pathHeap_.clear();
	return pathGen_;
}

//------------------------------------------------------------------------------
std::vector<cPosition> cWorldModel::safePath (const cVehicle& v, const cMapView& view, const cPosition& dest,
	eThreatLayer layer, int maxExpand) const
{
	const cPosition start = v.getPosition();
	if (!view.isValidPosition (dest) || dest == start) return {};
	if (!view.possiblePlace (v, dest)) return {};
	// шаг 13: разные области проходимости — пути нет (прежде A* обходил всю область впустую)
	{
		const int rs = regionOf (v, start);
		const int rd = regionOf (v, dest);
		if (rs >= 0 && rd >= 0 && rs != rd) return {};
	}
	const int W = view.getSize().x();
	const int H = view.getSize().y();
	const size_t N = static_cast<size_t> (W) * H;
	auto off = [W] (const cPosition& p) { return static_cast<size_t> (p.y()) * W + p.x(); };
	// минимальная цена шага — 2 (дорога вдвое дешевле суши): эвристика допустима
	auto heur = [&] (const cPosition& p) { return 2 * std::max (std::abs (p.x() - dest.x()), std::abs (p.y() - dest.y())); };

	constexpr int kInf = std::numeric_limits<int>::max();
	const unsigned int gen = beginPathSearch (N);
	auto gAt = [&] (size_t i) { return pathSeen_[i] == gen ? pathG_[i] : kInf; };
	using tNode = std::pair<int, int>; // (f, смещение клетки) — равные f по смещению: детерминированно
	auto push = [&] (int f, int cell) {
		pathHeap_.push_back ({f, cell});
		std::push_heap (pathHeap_.begin(), pathHeap_.end(), std::greater<tNode>());
	};
	pathSeen_[off (start)] = gen;
	pathG_[off (start)] = 0;
	pathPrev_[off (start)] = -1;
	push (heur (start), static_cast<int> (off (start)));
	int expanded = 0;
	while (!pathHeap_.empty())
	{
		std::pop_heap (pathHeap_.begin(), pathHeap_.end(), std::greater<tNode>());
		const int cur = pathHeap_.back().second;
		pathHeap_.pop_back();
		if (pathClosed_[cur] == gen) continue;
		pathClosed_[cur] = gen;
		const cPosition p (cur % W, cur / W);
		if (p == dest) break;
		if (maxExpand > 0 && ++expanded > maxExpand) return {};
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (n.x() < 0 || n.y() < 0 || n.x() >= W || n.y() >= H) continue;
				const size_t no = off (n);
				if (pathClosed_[no] == gen) continue;
				if (!view.possiblePlace (v, n)) continue;
				// зона огня с места — вдвое дороже прочей угрозы
				const int threat = threatFor (v, n, layer);
				const int fire = layer == eThreatLayer::None ? 0 : threatFor (v, n, eThreatLayer::Fire);
				const int step = cPathCalculator::calcNextCost (p, n, &v, &view) + (threat > 0 ? kThreatStepPenalty : 0)
					+ (fire > 0 ? kThreatStepPenalty : 0);
				const int ng = pathG_[cur] + step;
				if (ng < gAt (no))
				{
					pathSeen_[no] = gen;
					pathG_[no] = ng;
					pathPrev_[no] = cur;
					push (ng + heur (n), static_cast<int> (no));
				}
			}
	}
	if (gAt (off (dest)) == kInf) return {};
	std::vector<cPosition> path;
	for (int c = static_cast<int> (off (dest)); c != static_cast<int> (off (start)); c = pathPrev_[c])
		path.emplace_back (c % W, c / W);
	std::reverse (path.begin(), path.end());
	return path;
}

//------------------------------------------------------------------------------
std::vector<cPosition> cWorldModel::safePathNear (const cVehicle& v, const cMapView& view, const cPosition& target, int nearDist,
	eThreatLayer layer) const
{
	const cPosition start = v.getPosition();
	const long long near2 = static_cast<long long> (nearDist) * nearDist;
	if ((start - target).l2NormSquared() <= near2) return {};
	const int W = view.getSize().x();
	const int H = view.getSize().y();
	const size_t N = static_cast<size_t> (W) * H;
	// шаг 13: у цели нет ни одной клетки области старта — пути нет (не обходить всю область)
	if (const int rs = regionOf (v, start); rs >= 0)
	{
		bool any = false;
		for (int dy = -nearDist; dy <= nearDist && !any; ++dy)
			for (int dx = -nearDist; dx <= nearDist && !any; ++dx)
			{
				const cPosition c = target + cPosition (dx, dy);
				if (dx * dx + dy * dy > near2 || c == start) continue;
				if (regionOf (v, c) == rs) any = true;
			}
		if (!any) return {};
	}
	auto off = [W] (const cPosition& p) { return static_cast<size_t> (p.y()) * W + p.x(); };
	// до круга радиуса nearDist по Чебышёву не ближе cheb − nearDist шагов, шаг ≥ 2 — допустимо
	auto heur = [&] (const cPosition& p) {
		return 2 * std::max (0, std::max (std::abs (p.x() - target.x()), std::abs (p.y() - target.y())) - nearDist);
	};

	constexpr int kInf = std::numeric_limits<int>::max();
	const unsigned int gen = beginPathSearch (N);
	auto gAt = [&] (size_t i) { return pathSeen_[i] == gen ? pathG_[i] : kInf; };
	using tNode = std::pair<int, int>;
	auto push = [&] (int f, int cell) {
		pathHeap_.push_back ({f, cell});
		std::push_heap (pathHeap_.begin(), pathHeap_.end(), std::greater<tNode>());
	};
	pathSeen_[off (start)] = gen;
	pathG_[off (start)] = 0;
	pathPrev_[off (start)] = -1;
	push (heur (start), static_cast<int> (off (start)));
	int goal = -1;
	while (!pathHeap_.empty())
	{
		std::pop_heap (pathHeap_.begin(), pathHeap_.end(), std::greater<tNode>());
		const int cur = pathHeap_.back().second;
		pathHeap_.pop_back();
		if (pathClosed_[cur] == gen) continue;
		pathClosed_[cur] = gen;
		const cPosition p (cur % W, cur / W);
		if (p != start && (p - target).l2NormSquared() <= near2)
		{
			goal = cur;
			break;
		}
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (n.x() < 0 || n.y() < 0 || n.x() >= W || n.y() >= H) continue;
				const size_t no = off (n);
				if (pathClosed_[no] == gen) continue;
				if (!view.possiblePlace (v, n)) continue;
				const int threat = threatFor (v, n, layer);
				const int fire = layer == eThreatLayer::None ? 0 : threatFor (v, n, eThreatLayer::Fire);
				const int step = cPathCalculator::calcNextCost (p, n, &v, &view) + (threat > 0 ? kThreatStepPenalty : 0)
					+ (fire > 0 ? kThreatStepPenalty : 0);
				const int ng = pathG_[cur] + step;
				if (ng < gAt (no))
				{
					pathSeen_[no] = gen;
					pathG_[no] = ng;
					pathPrev_[no] = cur;
					push (ng + heur (n), static_cast<int> (no));
				}
			}
	}
	if (goal < 0) return {};
	std::vector<cPosition> path;
	for (int c = goal; c != static_cast<int> (off (start)); c = pathPrev_[c])
		path.emplace_back (c % W, c / W);
	std::reverse (path.begin(), path.end());
	return path;
}

//------------------------------------------------------------------------------
bool cWorldModel::nearestSafeCell (const cVehicle& v, const cMapView& view, const cPosition& near, int maxR,
	const cPosition& prefer, cPosition& out, eThreatLayer layer) const
{
	for (int r = 0; r <= maxR; ++r)
	{
		bool any = false;
		cPosition best;
		long long bestKey = std::numeric_limits<long long>::max();
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
				const cPosition p = near + cPosition (dx, dy);
				if (!view.isValidPosition (p)) continue;
				if (threatFor (v, p, layer) > 0) continue;
				if (p != v.getPosition() && !view.possiblePlace (v, p)) continue;
				// ближе к near (евклидово), затем ближе к prefer
				const long long key = static_cast<long long> ((p - near).l2NormSquared()) * 1000000LL + (p - prefer).l2NormSquared();
				if (key < bestKey)
				{
					bestKey = key;
					best = p;
					any = true;
				}
			}
		if (any)
		{
			out = best;
			return true;
		}
	}
	return false;
}

} // namespace ai
