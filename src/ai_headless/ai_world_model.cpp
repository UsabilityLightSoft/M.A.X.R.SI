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
	for (auto& kv : memory_)
		kv.second.visible = false;

	// какие клетки видны сейчас (шаг 8: разведка идёт к невиданным)
	w_ = map.getSize().x();
	h_ = map.getSize().y();
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
void cWorldModel::buildThreat (const cMap& map, int armorGround, int armorAir)
{
	w_ = map.getSize().x();
	h_ = map.getSize().y();
	ground_.assign (static_cast<size_t> (w_) * h_, 0);
	air_.assign (static_cast<size_t> (w_) * h_, 0);
	fire_.assign (static_cast<size_t> (w_) * h_, 0);
	auto paint = [&] (std::vector<int>& grid, const sEnemyMemory& m, int value, bool rangeOnly = false) {
		if (value <= 0) return;
		const int r = rangeOnly ? m.range : m.reach();
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
		if (m.hitsGround()) paint (ground_, m, m.damageVs (armorGround));
		if (m.hitsAir()) paint (air_, m, m.damageVs (armorAir));
		// зона огня с места: видимые сейчас (или прошлым ходом) и здания
		if (m.hitsGround() && (m.building || turn_ - m.seenTurn <= 1)) paint (fire_, m, m.damageVs (armorGround), true);
	}
}

//------------------------------------------------------------------------------
int cWorldModel::threatFor (const cVehicle& v, const cPosition& p, eThreatLayer layer) const
{
	if (layer == eThreatLayer::None) return 0;
	if (v.getStaticUnitData().factorAir > 0) return airThreat (p);
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
std::vector<cPosition> cWorldModel::safePath (const cVehicle& v, const cMapView& view, const cPosition& dest,
	eThreatLayer layer) const
{
	const cPosition start = v.getPosition();
	if (!view.isValidPosition (dest) || dest == start) return {};
	if (!view.possiblePlace (v, dest)) return {};
	const int W = view.getSize().x();
	const int H = view.getSize().y();
	const size_t N = static_cast<size_t> (W) * H;
	auto off = [W] (const cPosition& p) { return static_cast<size_t> (p.y()) * W + p.x(); };
	// минимальная цена шага — 2 (дорога вдвое дешевле суши): эвристика допустима
	auto heur = [&] (const cPosition& p) { return 2 * std::max (std::abs (p.x() - dest.x()), std::abs (p.y() - dest.y())); };

	constexpr int kInf = std::numeric_limits<int>::max();
	std::vector<int> g (N, kInf);
	std::vector<int> prev (N, -1);
	std::vector<char> closed (N, 0);
	using tNode = std::pair<int, int>; // (f, смещение клетки) — равные f по смещению: детерминированно
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	g[off (start)] = 0;
	open.push ({heur (start), static_cast<int> (off (start))});
	while (!open.empty())
	{
		const int cur = open.top().second;
		open.pop();
		if (closed[cur]) continue;
		closed[cur] = 1;
		const cPosition p (cur % W, cur / W);
		if (p == dest) break;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (n.x() < 0 || n.y() < 0 || n.x() >= W || n.y() >= H) continue;
				const size_t no = off (n);
				if (closed[no]) continue;
				if (!view.possiblePlace (v, n)) continue;
				// зона огня с места — вдвое дороже прочей угрозы
				const int threat = threatFor (v, n, layer);
				const int fire = layer == eThreatLayer::None ? 0 : threatFor (v, n, eThreatLayer::Fire);
				const int step = cPathCalculator::calcNextCost (p, n, &v, &view) + (threat > 0 ? kThreatStepPenalty : 0)
					+ (fire > 0 ? kThreatStepPenalty : 0);
				const int ng = g[cur] + step;
				if (ng < g[no])
				{
					g[no] = ng;
					prev[no] = cur;
					open.push ({ng + heur (n), static_cast<int> (no)});
				}
			}
	}
	if (g[off (dest)] == kInf) return {};
	std::vector<cPosition> path;
	for (int c = static_cast<int> (off (dest)); c != static_cast<int> (off (start)); c = prev[c])
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
	auto off = [W] (const cPosition& p) { return static_cast<size_t> (p.y()) * W + p.x(); };
	// до круга радиуса nearDist по Чебышёву не ближе cheb − nearDist шагов, шаг ≥ 2 — допустимо
	auto heur = [&] (const cPosition& p) {
		return 2 * std::max (0, std::max (std::abs (p.x() - target.x()), std::abs (p.y() - target.y())) - nearDist);
	};

	constexpr int kInf = std::numeric_limits<int>::max();
	std::vector<int> g (N, kInf);
	std::vector<int> prev (N, -1);
	std::vector<char> closed (N, 0);
	using tNode = std::pair<int, int>;
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	g[off (start)] = 0;
	open.push ({heur (start), static_cast<int> (off (start))});
	int goal = -1;
	while (!open.empty())
	{
		const int cur = open.top().second;
		open.pop();
		if (closed[cur]) continue;
		closed[cur] = 1;
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
				if (closed[no]) continue;
				if (!view.possiblePlace (v, n)) continue;
				const int threat = threatFor (v, n, layer);
				const int fire = layer == eThreatLayer::None ? 0 : threatFor (v, n, eThreatLayer::Fire);
				const int step = cPathCalculator::calcNextCost (p, n, &v, &view) + (threat > 0 ? kThreatStepPenalty : 0)
					+ (fire > 0 ? kThreatStepPenalty : 0);
				const int ng = g[cur] + step;
				if (ng < g[no])
				{
					g[no] = ng;
					prev[no] = cur;
					open.push ({ng + heur (n), static_cast<int> (no)});
				}
			}
	}
	if (goal < 0) return {};
	std::vector<cPosition> path;
	for (int c = goal; c != static_cast<int> (off (start)); c = prev[c])
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
