/***************************************************************************
 * ai_survey.cpp — умный геолог (Р7). Описание — в заголовке.
 ***************************************************************************/

#include "ai_survey.h"

#include "ai_world_model.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <tuple>

namespace ai
{

namespace
{

constexpr int kRingStep = 3;       // полоса кольца: геолог видит 3×3 — кольца через 3 клетки
constexpr int kMaxRing = 8;        // колец (до 24 клеток от центра базы); дальше — прежняя разведка
constexpr int kStride = 6;         // по кольцу — до стольких клеток вперёд за ход (ход геолога — 7)
constexpr int kCloseEnemy = 24;    // база врага (или точка напротив) ближе — кольца не нужны: прежняя разведка
constexpr int kVeinR = 24;         // жилы — у базы (Чебышёв от центра)
constexpr int kVeinMin = 2;        // жила — от клетки с ресурсом не меньше
constexpr int kEnemyGap = 16;      // к зданиям врага не ближе (ракетомёты, скауты)
constexpr int kGuessGap = 22;      // к предполагаемой базе врага (точка напротив) не ближе
constexpr int kTakenGap = 3;       // цели двух геологов — не ближе друг к другу
constexpr int kTries = 4;          // целей без пути — столько пробовать за ход
constexpr int kStatusEvery = 50;
constexpr double kPi = 3.14159265358979323846;

int cheb (const cPosition& a, const cPosition& b)
{
	return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
}

long long dist2 (const cPosition& a, const cPosition& b)
{
	return (a - b).l2NormSquared();
}

std::vector<cPosition> toVec (const std::forward_list<cPosition>& l)
{
	return std::vector<cPosition> (l.begin(), l.end());
}

// путь помещается в один ход машины (её текущие очки хода)
bool fitsTurn (const cVehicle& v, const cMapView& view, const std::vector<cPosition>& path)
{
	int budget = v.data.getSpeed();
	cPosition prev = v.getPosition();
	for (const auto& p : path)
	{
		const int c = cPathCalculator::calcNextCost (prev, p, &v, &view);
		if (c > budget) return false;
		budget -= c;
		prev = p;
	}
	return true;
}

} // namespace

//------------------------------------------------------------------------------
void cSurvey::event (const char* what, nlohmann::json extra)
{
	nlohmann::json e = {{"event", "survey"}, {"level", "micro"}, {"what", what}, {"turn", turn_}};
	if (extra.is_object())
		for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
	events_.push_back (std::move (e));
}

std::vector<nlohmann::json> cSurvey::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

int cSurvey::angleKey (const cPosition& c) const
{
	if (c == center_) return 0;
	double a = std::atan2 (static_cast<double> (c.y() - center_.y()), static_cast<double> (c.x() - center_.x())) - dirAngle_;
	while (a < 0) a += 2 * kPi;
	while (a >= 2 * kPi) a -= 2 * kPi;
	return static_cast<int> (a * 180.0 / kPi);
}

bool cSurvey::mineCell (const cModel& model, const cPosition& c) const
{
	const cMap& map = *model.getMap();
	return map.isValidPosition (c) && !map.isWater (c) && !map.isCoast (c) && !map.isBlocked (c);
}

bool cSurvey::nearEnemy (const cPosition& p) const
{
	for (const auto& b : enemyBuildings_)
		if (cheb (b, p) <= kEnemyGap) return true;
	return guess_.x() >= 0 && cheb (guess_, p) <= kGuessGap;
}

//------------------------------------------------------------------------------
void cSurvey::update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cPosition& baseCenter, int turn)
{
	turn_ = turn;
	const char* off = std::getenv ("MAXR_SURVEY");
	enabled_ = !(off != nullptr && off[0] == '0');
	const cMap& map = *model.getMap();
	W_ = map.getSize().x();
	H_ = map.getSize().y();
	center_ = baseCenter.x() >= 0 ? baseCenter : player.getLandingPos();
	const cPosition mid (W_ / 2, H_ / 2);
	dirAngle_ = mid == center_ ? 0.0 : std::atan2 (static_cast<double> (mid.y() - center_.y()), static_cast<double> (mid.x() - center_.x()));
	enemyBuildings_.clear();
	for (const auto& kv : world.enemies())
		if (kv.second.building && kv.second.owner >= 0) enemyBuildings_.push_back (kv.second.pos);
	guess_ = enemyBuildings_.empty() ? cPosition (W_ - 1 - center_.x(), H_ - 1 - center_.y()) : cPosition (-1, -1);
	taken_.clear();

	// жилы: неразведанные соседи разведанных клеток с ресурсом у базы
	veinTargets_.clear();
	for (int y = std::max (0, center_.y() - kVeinR); y <= std::min (H_ - 1, center_.y() + kVeinR); ++y)
		for (int x = std::max (0, center_.x() - kVeinR); x <= std::min (W_ - 1, center_.x() + kVeinR); ++x)
		{
			const cPosition c (x, y);
			if (!player.hasResourceExplored (c)) continue;
			const auto& r = map.getResource (c);
			if (r.typ == eResourceType::None || r.value < kVeinMin) continue; // жила — от значимого ресурса (шахта берёт от 3)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const cPosition n = c + cPosition (dx, dy);
					if ((dx == 0 && dy == 0) || !map.isValidPosition (n) || player.hasResourceExplored (n)) continue;
					if (std::find (veinTargets_.begin(), veinTargets_.end(), n) == veinTargets_.end()) veinTargets_.push_back (n);
				}
		}

	// счётчики: разведано, шахты, геологи (гибель — в журнал)
	const int lost = counts_.lost, flights = counts_.flights, lastStand = counts_.lastStand, veins = counts_.veins;
	counts_ = {};
	counts_.lost = lost;
	counts_.flights = flights;
	counts_.lastStand = lastStand;
	counts_.veins = veins;
	for (int y = 0; y < H_; ++y)
		for (int x = 0; x < W_; ++x)
		{
			const cPosition c (x, y);
			if (!player.hasResourceExplored (c)) continue;
			++counts_.explored;
			if (cheb (c, center_) <= 20) ++counts_.explored20;
		}
	for (const auto& b : player.getBuildings())
		if (b->getStaticData().canMineMaxRes > 0) ++counts_.mines;
	std::set<unsigned int> alive;
	for (const auto& v : player.getVehicles())
		if (v->getStaticUnitData().vehicleData.canSurvey)
		{
			alive.insert (v->getId());
			++counts_.surveyors;
		}
	for (unsigned int id : known_)
		if (alive.count (id) == 0)
		{
			++counts_.lost;
			cPosition at (-1, -1);
			for (const auto& lp : lastPos_)
				if (lp.first == id) at = lp.second;
			event ("lost", {{"unitId", id}, {"pos", {{"x", at.x()}, {"y", at.y()}}}});
		}
	known_ = alive;
	lastPos_.clear();
	for (const auto& v : player.getVehicles())
		if (alive.count (v->getId()) != 0 && !v->isUnitLoaded()) lastPos_.emplace_back (v->getId(), v->getPosition());
	if (turn_ % kStatusEvery == 0 || turn_ == 100)
		event ("status", {{"explored20", counts_.explored20}, {"explored", counts_.explored}, {"mines", counts_.mines},
			{"surveyors", counts_.surveyors}, {"lost", counts_.lost}, {"flights", counts_.flights}, {"lastStand", counts_.lastStand},
			{"veins", counts_.veins}, {"enabled", enabled_}});
}

//------------------------------------------------------------------------------
std::vector<cPosition> cSurvey::pathFor (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
	const cVehicle& v, std::string& why)
{
	const cMap& map = *model.getMap();
	const cPosition at = v.getPosition();
	// порядок кандидатов: ключ (дистанция, угол), затем клетка по обходу — детерминированно
	const auto byKey = [this] (const std::tuple<long long, int, cPosition>& a, const std::tuple<long long, int, cPosition>& b) {
		if (std::get<0> (a) != std::get<0> (b)) return std::get<0> (a) < std::get<0> (b);
		if (std::get<1> (a) != std::get<1> (b)) return std::get<1> (a) < std::get<1> (b);
		return std::get<2> (a).y() * W_ + std::get<2> (a).x() < std::get<2> (b).y() * W_ + std::get<2> (b).x();
	};
	// путь к цели — только безопасный (как прежняя разведка: путь движка, иначе в обход угроз; через угрозу — не цель)
	auto pathTo = [&] (const cPosition& dest) {
		auto path = toVec (cPathCalculator (v, view, dest, false).calcPath());
		if (!path.empty() && world.pathUnsafe (v, path, eThreatLayer::Reach)) path = world.safePath (v, view, dest, eThreatLayer::Reach);
		if (!path.empty() && world.pathUnsafe (v, path, eThreatLayer::Reach)) path.clear();
		return path;
	};
	auto takenNear = [&] (const cPosition& c) {
		for (const auto& t : taken_)
			if (cheb (t, c) <= kTakenGap) return true;
		return false;
	};
	auto unexploredMineAround = [&] (const cPosition& c) {
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition n = c + cPosition (dx, dy);
				if (map.isValidPosition (n) && !player.hasResourceExplored (n) && mineCell (model, n)) return true;
			}
		return false;
	};

	// 4) под огнём (в зоне огня видимого врага или его уже обстреливают): в безопасную клетку, если успевает за ход (вода лучше:
	//    скаут по воде втрое медленнее, танк не пройдёт); не успевает — разведка нового рядом (последние ходы — с пользой). Угроза
	//    «подъедет и выстрелит» (не под огнём) — прежний уход в ближайшую безопасную клетку (ai_rule_brain): last stand при ней
	//    губил геологов (Donuts, базы в 13 клетках: потеряно 22 против 12)
	if (world.threatFor (v, at, eThreatLayer::Reach) > 0 && !(world.groundFire (at) > 0 || v.isBeingAttacked()))
		return {}; // прежний уход (ai_rule_brain: nearestSafeCell)
	if (world.groundFire (at) > 0 || v.isBeingAttacked())
	{
		const int R = std::max (1, v.data.getSpeedMax() / 4) + 1;
		std::vector<std::tuple<long long, int, cPosition>> safe;
		for (int dy = -R; dy <= R; ++dy)
			for (int dx = -R; dx <= R; ++dx)
			{
				const cPosition c = at + cPosition (dx, dy);
				if (c == at || !view.isValidPosition (c) || !view.possiblePlace (v, c)) continue;
				if (world.threatFor (v, c, eThreatLayer::Reach) > 0) continue;
				const bool water = map.isWater (c);
				safe.emplace_back (dist2 (at, c) - (water ? 9 : 0), (static_cast<int> (c.y()) * W_ + c.x()), c);
			}
		std::sort (safe.begin(), safe.end(), byKey);
		for (size_t i = 0; i < safe.size() && i < 6; ++i)
		{
			const cPosition c = std::get<2> (safe[i]);
			auto path = world.safePath (v, view, c, eThreatLayer::Reach);
			if (path.empty() || !fitsTurn (v, view, path)) continue;
			++counts_.flights;
			why = std::string ("surveyor: away from the enemy within one turn") + (map.isWater (c) ? " (over water: a scout is three times slower there)" : "");
			event ("flight", {{"unitId", v.getId()}, {"from", {{"x", at.x()}, {"y", at.y()}}}, {"to", {{"x", c.x()}, {"y", c.y()}}}, {"water", map.isWater (c)}});
			return path;
		}
		// не успевает: ближайшая неразведанная клетка под шахту в пределах хода
		std::vector<std::tuple<long long, int, cPosition>> fresh;
		for (int dy = -R; dy <= R; ++dy)
			for (int dx = -R; dx <= R; ++dx)
			{
				const cPosition c = at + cPosition (dx, dy);
				if (c == at || !view.isValidPosition (c) || !view.possiblePlace (v, c) || !unexploredMineAround (c)) continue;
				fresh.emplace_back (dist2 (at, c), angleKey (c), c);
			}
		std::sort (fresh.begin(), fresh.end(), byKey);
		for (size_t i = 0; i < fresh.size() && i < 4; ++i)
		{
			auto path = toVec (cPathCalculator (v, view, std::get<2> (fresh[i]), false).calcPath());
			if (path.empty() || !fitsTurn (v, view, path)) continue;
			++counts_.lastStand;
			why = "surveyor: cannot get away in one turn: survey new cells nearby (last stand)";
			event ("last stand", {{"unitId", v.getId()}, {"at", {{"x", at.x()}, {"y", at.y()}}}});
			return path;
		}
		return {}; // ни того, ни другого — прежнее поведение (ближайшая безопасная клетка)
	}

	// враг совсем рядом (база или точка напротив ближе kCloseEnemy) — кольца и жилы не нужны (своя земля мала): прежняя разведка.
	// Donuts (базы в 13 клетках): кольцевая разведка — D 0.40 против 0.535 у прежней на тех же сидах; механизм не установлен
	{
		int toEnemy = std::numeric_limits<int>::max();
		for (const auto& b : enemyBuildings_) toEnemy = std::min (toEnemy, cheb (b, center_));
		if (guess_.x() >= 0) toEnemy = std::min (toEnemy, cheb (guess_, center_));
		if (toEnemy < kCloseEnemy)
		{
			why = "surveyor: the enemy base is close, rings are not needed";
			return {};
		}
	}
	// 2) жила: неразведанный сосед найденного ресурса (ближе к геологу; ничья — по углу)
	{
		std::vector<std::tuple<long long, int, cPosition>> cands;
		for (const auto& c : veinTargets_)
		{
			if (takenNear (c) || nearEnemy (c) || world.threatFor (v, c, eThreatLayer::Reach) > 0) continue;
			cands.emplace_back (dist2 (at, c), angleKey (c), c);
		}
		std::sort (cands.begin(), cands.end(), byKey);
		for (size_t i = 0; i < cands.size() && static_cast<int> (i) < kTries; ++i)
		{
			const cPosition c = std::get<2> (cands[i]);
			auto path = pathTo (c);
			if (path.empty()) continue;
			taken_.push_back (c);
			++counts_.veins;
			why = "surveyor: follow the vein (resource found next to it)";
			return path;
		}
	}

	// 1) кольца вокруг центра базы полосой 3: геолог идёт по средней линии кольца (Чебышёв 3k) — клетки, у которых в 3×3 есть
	//    неразведанная суша; по кольцу — по часовой стрелке на полный ход (до kStride клеток вперёд: по пути открывается вся полоса
	//    3 клетки шириной), а не к соседней клетке (Donuts: шаг в 1 клетку за ход — разведано вдвое меньше, геологов гибло вдвое
	//    больше); кольцо кончилось — следующее
	const int myAngle = angleKey (at);
	// кольца — не дальше половины пути до врага (его здания или точка напротив): базы бывают в 13 клетках (Donuts)
	int maxRing = kMaxRing;
	{
		int toEnemy = std::numeric_limits<int>::max();
		for (const auto& b : enemyBuildings_) toEnemy = std::min (toEnemy, cheb (b, center_));
		if (guess_.x() >= 0) toEnemy = std::min (toEnemy, cheb (guess_, center_));
		if (toEnemy < std::numeric_limits<int>::max()) maxRing = std::clamp ((toEnemy / 2 + 2) / kRingStep, 1, kMaxRing);
	}
	for (int k = 1; k <= maxRing; ++k)
	{
		const int d = k * kRingStep;
		std::vector<std::tuple<long long, int, cPosition>> cands;
		for (int dy = -d; dy <= d; ++dy)
			for (int dx = -d; dx <= d; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != d) continue;
				const cPosition c = center_ + cPosition (dx, dy);
				if (!view.isValidPosition (c) || takenNear (c) || nearEnemy (c)) continue;
				if (c != at && !view.possiblePlace (v, c)) continue;
				if (world.threatFor (v, c, eThreatLayer::Reach) > 0 || !unexploredMineAround (c)) continue;
				// в пределах хода и впереди по часовой (сдвиг угла 1…179°) — дальше вперёд; иначе — ближе к геологу
				const int ahead = (angleKey (c) - myAngle + 360) % 360;
				const bool inStride = cheb (c, at) <= kStride && ahead > 0 && ahead < 180;
				cands.emplace_back (inStride ? -1000000LL - ahead : dist2 (at, c), angleKey (c), c);
			}
		if (cands.empty()) continue;
		std::sort (cands.begin(), cands.end(), byKey);
		for (size_t i = 0; i < cands.size() && static_cast<int> (i) < kTries; ++i)
		{
			const cPosition c = std::get<2> (cands[i]);
			if (c == at) continue;
			auto path = pathTo (c);
			if (path.empty()) continue;
			taken_.push_back (c);
			why = "surveyor: ring " + std::to_string (k) + " around the base (band of 3)";
			return path;
		}
	}
	why = "surveyor: rings around the base done";
	return {};
}

} // namespace ai
