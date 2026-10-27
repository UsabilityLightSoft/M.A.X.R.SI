/***************************************************************************
 * ai_commando.cpp — свои коммандос ИИИ (П2, П18; шаг 13, часть 3). Описание — в заголовке.
 ***************************************************************************/

#include "ai_commando.h"

#include "ai_defense.h"
#include "ai_roles.h"
#include "ai_world_model.h"

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

constexpr int kMaxCommandos = 4;     // коммандос всего — не больше
constexpr int kTrainIncome = 28;     // учебный центр ради чужих — с добычи металла (фабрика есть; как ур. 3 обороны)…
constexpr int kTrainTurn = 120;      // …или с этого хода при добыче ≥ kTrainLateIncome. Опыт quick против base3 (без авиации):
constexpr int kTrainLateIncome = 16; //  с 12 и хода 30 — 0.453; с 20 и 80 — 0.482; с 28 и 120 — 0.508; с 36 и 150 — 0.499
constexpr int kDeathMemory = 60;     // клетка гибели коммандос — опасна столько ходов (сыщик врага там, которого мы не видели)
constexpr int kDeathRadius = 8;
constexpr int kStealChance = 70;     // захват машины врага (с хозяином) — только при шансе не ниже, %
constexpr int kAlienReach = 90;      // чужие — не дальше от центра базы (Чебышёв): дальше — долгая дорога без пользы
constexpr int kWalkFar = 25;         // дальше — просить авиатранспорт (если есть)
constexpr int kSabotageRank = 2;     // диверсия у врага — с этого звания (опыт — на чужих)
constexpr int kSabotageChance = 75;  // и шанс выключить не ниже, % (неудача — тревога и огонь)
constexpr int kSabotageCooldown = 3; // после удачи — отойти и вернуться не раньше
constexpr int kStealthExpand = 12000; // предел узлов A* в обход сыщиков
constexpr int kRaidStale = 30;       // налёт без готовности коммандос — снять через столько ходов
constexpr int kHomeHold = 5;         // без дела — у центра базы (Чебышёв)
constexpr int kAlienEnemyGap = 15;   // чужие ближе к зданиям врага — не трогать: его пехота и армия найдут коммандос
constexpr int kStatusEvery = 10;

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

int rankOf (const cVehicle& v)
{
	return cCommandoData::getLevel (v.getCommandoData().getSuccessCount());
}

// юнит u рядом с клеткой p (любая его клетка — среди 8 соседей)
bool besideUnit (const cUnit& u, const cPosition& p)
{
	for (const auto& c : u.getPositions())
		if (cheb (c, p) == 1) return true;
	return false;
}

nlohmann::json posJ (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

// ценность здания врага для диверсии (П18): электростанция гасит сразу несколько зданий, шахта и завод — экономика
int sabotageValue (const cStaticUnitData& sd)
{
	if (sd.produceEnergy > 0 || sd.ID == sID (1, 7) || sd.ID == sID (1, 8)) return sd.buildingData.isBig ? 100 : 70;
	if (sd.buildingData.canMineMaxRes > 0) return 90;
	if (!sd.canBuild.empty()) return 75;
	if (sd.ID == sID (1, 26)) return 60; // радар — «ослепить» ПВО и ракетные пушки
	if ((sd.canAttack & eTerrainFlag::Air) != 0) return 55; // зенитка
	if (sd.buildingData.canResearch) return 45;
	if (sd.canAttack != 0) return 50; // орудие
	return 0;
}

} // namespace

//------------------------------------------------------------------------------
void cCommandos::event (const char* what, nlohmann::json extra)
{
	nlohmann::json e = {{"event", "commando"}, {"level", "operational"}, {"what", what}, {"turn", turn_}};
	if (extra.is_object())
		for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
	events_.push_back (std::move (e));
}

std::vector<nlohmann::json> cCommandos::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

nlohmann::json cCommandos::toJson() const
{
	nlohmann::json tasks = nlohmann::json::array();
	static constexpr const char* names[5] = {"idle", "escape", "raid", "alien", "sabotage"};
	for (const auto& [id, t] : tasks_)
		tasks.push_back ({id, names[static_cast<int> (t.task)], t.target == kNoUnit ? -1 : static_cast<long long> (t.target)});
	return {{"commandos", counts_.commandos}, {"wanted", wanted_}, {"training", wantTraining_}, {"aliensKnown", counts_.aliensKnown},
		{"aliensFree", counts_.aliensFree}, {"disables", counts_.disables}, {"captures", counts_.captures}, {"fails", counts_.fails},
		{"detected", counts_.detected}, {"lost", counts_.lost}, {"sabotage", counts_.sabotage}, {"maxRank", counts_.maxRank},
		{"tasks", tasks}, {"raidTarget", raid_.target == kNoUnit ? -1 : static_cast<long long> (raid_.target)}, {"why", why_}};
}

//------------------------------------------------------------------------------
bool cCommandos::inDetectorZone (const cPosition& p) const
{
	for (const auto& d : detectors_)
		if (dist2 (p, d.pos) <= static_cast<long long> (d.radius) * d.radius) return true;
	return false;
}

//------------------------------------------------------------------------------
void cCommandos::update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, int income, int factories, int turn)
{
	turn_ = turn;
	playerId_ = player.getId();
	const cMap& map = *model.getMap();
	W_ = map.getSize().x();
	H_ = map.getSize().y();
	home_ = defense.baseCenter();
	homeRegion_ = defense.map().landRegion;
	const auto& ud = *model.getUnitsData();

	// чужие: что видим сейчас — в память (стоят на месте: место, выключены ли); клетка видна, а их там нет — забыть
	for (const auto& v : model.getNeutralVehicles())
	{
		if (!player.canSeeUnit (*v, map)) continue;
		sAlien& a = aliens_[v->getId()];
		const bool fresh = a.seenTurn < 0;
		a.id = v->getId();
		a.type = v->getStaticUnitData().ID;
		a.pos = v->getPosition();
		a.disabled = v->isDisabled();
		a.cost = v->data.getBuildCost();
		a.plane = v->getStaticUnitData().factorAir > 0;
		a.ship = v->getStaticUnitData().factorSea > 0 && v->getStaticUnitData().factorGround <= 0;
		a.seenTurn = turn;
		if (fresh)
			event ("alien seen", {{"unitId", a.id}, {"type", v->getStaticUnitData().getDefaultName()}, {"pos", posJ (a.pos)},
				{"distance", home_.x() >= 0 ? cheb (a.pos, home_) : -1}});
	}
	for (auto it = aliens_.begin(); it != aliens_.end();)
	{
		if (it->second.seenTurn != turn && map.isValidPosition (it->second.pos) && player.canSeeAt (it->second.pos))
			it = aliens_.erase (it);
		else
			++it;
	}
	// здания врага (память) — у них чужих не трогать
	enemyBuildings_.clear();
	for (const auto& kv : world.enemies())
		if (kv.second.building && kv.second.owner >= 0) enemyBuildings_.push_back (kv.second.pos);
	// сыщики врага (пехота, коммандос) из памяти — радиус обзора + 1
	detectors_.clear();
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (m.building) continue;
		const sID id (m.typeFirst, m.typeSecond);
		if (!ud.isValidId (id)) continue;
		if ((ud.getStaticUnitData (id).canDetectStealthOn & eTerrainFlag::Ground) == 0) continue;
		detectors_.push_back ({m.pos, std::max (1, m.scan) + 1});
	}

	// свои коммандос и БМП; погибшие — в журнал
	const int captures = counts_.captures, disables = counts_.disables, fails = counts_.fails, detected = counts_.detected,
		lost = counts_.lost, sabotage = counts_.sabotage, apcLandings = counts_.apcLandings;
	counts_ = {};
	counts_.captures = captures;
	counts_.disables = disables;
	counts_.fails = fails;
	counts_.detected = detected;
	counts_.lost = lost;
	counts_.sabotage = sabotage;
	counts_.apcLandings = apcLandings;
	std::set<unsigned int> alive;
	for (const auto& v : player.getVehicles())
	{
		const eRole r = roles.roleOf (*v);
		if (r == eRole::Apc) ++counts_.apcs;
		if (r != eRole::Commando) continue;
		++counts_.commandos;
		alive.insert (v->getId());
		if (!v->isUnitLoaded()) lastPos_[v->getId()] = v->getPosition();
		const int rank = rankOf (*v);
		counts_.maxRank = std::max (counts_.maxRank, rank);
		auto& seen = rankSeen_[v->getId()];
		if (rank > seen && known_.count (v->getId()) != 0)
			event ("rank", {{"unitId", v->getId()}, {"from", seen}, {"to", rank}, {"successes", v->getCommandoData().getSuccessCount()}});
		seen = rank;
	}
	for (unsigned int id : known_)
		if (alive.count (id) == 0)
		{
			++counts_.lost;
			const auto lp = lastPos_.find (id);
			if (lp != lastPos_.end()) deaths_.emplace_back (lp->second, turn);
			event ("lost", {{"unitId", id}, {"task", tasks_.count (id) ? static_cast<int> (tasks_[id].task) : 0},
				{"pos", lp != lastPos_.end() ? posJ (lp->second) : posJ (cPosition (-1, -1))}});
			lastPos_.erase (id);
			tasks_.erase (id);
			rankSeen_.erase (id);
			if (raid_.commando == id) raid_.commando = kNoUnit;
		}
	known_ = alive;
	for (auto it = ferried_.begin(); it != ferried_.end();)
		it = alive.count (*it) ? std::next (it) : ferried_.erase (it);

	// программа: чужие рядом (своя суша) — учебный центр и коммандос для тренировки и захвата; враг найден и оборона
	// ур. 3 — ещё один на диверсию и налёт
	int reachable = 0, overWater = 0;
	for (const auto& [id, a] : aliens_)
	{
		++counts_.aliensKnown;
		if (!a.disabled) ++counts_.aliensFree;
		if (home_.x() < 0 || cheb (a.pos, home_) > kAlienReach || nearEnemy (a.pos)) continue;
		bool onOurLand = !a.ship && world.tankRegionOf (a.pos) == homeRegion_;
		for (int dy = -1; dy <= 1 && !onOurLand && a.ship; ++dy)
			for (int dx = -1; dx <= 1 && !onOurLand; ++dx)
				onOurLand = world.tankRegionOf (a.pos + cPosition (dx, dy)) == homeRegion_;
		if (onOurLand && !a.ship) ++reachable; // корабль чужих ценен на воде — без флота его не берём в счёт
		else if (!a.ship) ++overWater;
	}
	bool enemyKnown = false;
	for (const auto& kv : world.enemies())
		if (kv.second.building && kv.second.owner >= 0) enemyKnown = true;
	int want = 0;
	std::string why;
	wantOverWater_ = overWater > 0;
	if (reachable > 0)
	{
		want = std::clamp (1 + reachable / 3, 1, 3);
		why = std::to_string (reachable) + " alien vehicles on our land: train (disable) and capture without alarm";
	}
	else if (overWater > 0 && (ferryAvailable_ || turn >= kTrainTurn))
	{
		want = std::clamp (1 + overWater / 4, 1, 2);
		why = std::to_string (overWater) + " alien vehicles beyond water: commandos go by air transport or APC (under water)";
	}
	if (enemyKnown && defense.level() >= 3)
	{
		want = std::max (want, 1) + (raid_.target != kNoUnit ? 1 : 0);
		why += std::string (why.empty() ? "" : "; ") + "enemy base known, defence level 3: sabotage and raids";
	}
	else if (raid_.target != kNoUnit)
	{
		want = std::max (want, 1);
		why += std::string (why.empty() ? "" : "; ") + "air raid asks for a commando";
	}
	want = std::min (want, kMaxCommandos);
	// опыт «вариант против варианта»: MAXR_COMMANDO=0 — своих коммандос нет (учебный центр — только оборона)
	if (const char* off = std::getenv ("MAXR_COMMANDO"); off != nullptr && off[0] == '0')
	{
		want = 0;
		why = "MAXR_COMMANDO=0";
	}
	wanted_ = want;
	// пороги (опыт «вариант против варианта»: MAXR_CMD_TRAIN — добыча, MAXR_CMD_TURN — ход позднего порога); раз включился —
	// держится, пока добыча не упала ниже 2/3 порога (иначе жилой блок строился, а учебный центр — нет)
	int trainIncome = kTrainIncome, trainTurn = kTrainTurn;
	if (const char* e = std::getenv ("MAXR_CMD_TRAIN")) trainIncome = std::atoi (e);
	if (const char* e = std::getenv ("MAXR_CMD_TURN")) trainTurn = std::atoi (e);
	const bool on = income >= trainIncome || (turn >= trainTurn && income >= kTrainLateIncome);
	const bool keep = wantTraining_ && income * 3 >= trainIncome * 2;
	wantTraining_ = want > 0 && factories >= 1 && (on || keep);
	why_ = why.empty() ? std::string ("no aliens seen on our land, enemy base unknown or defence below level 3") : why;
	if (wanted_ != lastWanted_ || (why_ != lastWhy_ && wanted_ > 0))
	{
		event ("program", {{"wanted", wanted_}, {"training", wantTraining_}, {"why", why_}, {"aliensReachable", reachable},
			{"aliensKnown", counts_.aliensKnown}, {"income", income}});
		lastWanted_ = wanted_;
		lastWhy_ = why_;
	}
	// БМП (под водой её видит только корвет): цель коммандос на другой суше, а авиатранспорта нет
	enemyOverWater_ = defense.map().enemyLand == 0 && enemyKnown;
	apcWanted_ = (((wantOverWater_ && !ferryAvailableAir_) || (enemyOverWater_ && counts_.maxRank >= kSabotageRank)) && counts_.commandos > 0) ? 1 : 0;
	if (turn_ % kStatusEvery == 0 && (counts_.commandos > 0 || wanted_ > 0)) event ("status", toJson());
}

//------------------------------------------------------------------------------
void cCommandos::checkTries (const cModel& model, const cPlayer& player)
{
	for (const auto& t : std::exchange (tries_, {}))
	{
		const cVehicle* c = model.getVehicleFromID (t.commando);
		const cUnit* u = model.getUnitFromID (t.target);
		bool ok = false;
		if (t.steal)
			ok = u != nullptr && u->getOwner() == &player;
		else
			ok = c != nullptr && static_cast<int> (c->getCommandoData().getSuccessCount()) > t.successBefore; // опыт — только от удачи
		const bool detected = c != nullptr && c->isDetectedByAnyPlayer();
		if (ok)
		{
			if (t.steal) ++counts_.captures;
			else ++counts_.disables;
			if (!t.steal && !t.neutral) ++counts_.sabotage;
		}
		else
			++counts_.fails;
		if (detected) ++counts_.detected;
		event (t.steal ? (ok ? "captured" : "capture failed") : (ok ? "disabled" : "disable failed"),
			{{"unitId", t.commando}, {"target", t.target}, {"type", t.type}, {"chance", t.chance}, {"neutral", t.neutral},
				{"detected", detected}, {"alive", c != nullptr}, {"rank", c ? rankOf (*c) : -1}});
		if (ok && t.steal)
			aliens_.erase (t.target);
		if (ok && !t.steal && t.neutral)
			if (auto it = aliens_.find (t.target); it != aliens_.end()) it->second.disabled = true;
		if (ok && !t.steal && !t.neutral)
		{
			// диверсия удалась — отойти (враг обыщет клетки у здания) и вернуться не раньше kSabotageCooldown ходов
			if (auto it = tasks_.find (t.commando); it != tasks_.end())
			{
				it->second.task = eTask::Sabotage;
				it->second.since = turn_;
				it->second.why = "done";
			}
		}
	}
}

//------------------------------------------------------------------------------
cPosition cCommandos::besideCell (const cMapView& view, const cVehicle& v, const cUnit& u, const cPosition& from) const
{
	cPosition best (-1, -1);
	long long bestKey = std::numeric_limits<long long>::max();
	for (const auto& q : u.getPositions())
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition c = q + cPosition (dx, dy);
				if (!view.isValidPosition (c) || !besideUnit (u, c)) continue;
				bool inside = false;
				for (const auto& q2 : u.getPositions())
					if (q2 == c) inside = true;
				if (inside) continue;
				if (c != v.getPosition() && !view.possiblePlace (v, c)) continue;
				if (inDetectorZone (c)) continue;
				if (std::find (reserved_.begin(), reserved_.end(), c) != reserved_.end() && c != v.getPosition()) continue;
				const long long key = dist2 (c, from) * 100000 + static_cast<long long> (c.y()) * W_ + c.x();
				if (key < bestKey)
				{
					bestKey = key;
					best = c;
				}
			}
	return best;
}

//------------------------------------------------------------------------------
// клетка у цели — та же, что в прошлый ход, пока она годна (рядом с целью, свободна, вне радиуса сыщиков): иначе коммандос, считая
// ближайшую клетку от себя заново, ходил туда-обратно (налёт s13p3s, Gen_Archipelago 57: 30 ходов между двумя клетками)
cPosition cCommandos::stickyBeside (const cMapView& view, const cVehicle& v, const cUnit& u, sTask& t) const
{
	const cPosition g = t.goal;
	if (g.x() >= 0 && view.isValidPosition (g) && besideUnit (u, g) && !inDetectorZone (g) && (g == v.getPosition() || view.possiblePlace (v, g)))
	{
		bool inside = false;
		for (const auto& q : u.getPositions())
			if (q == g) inside = true;
		if (!inside) return g;
	}
	const cPosition b = besideCell (view, v, u, v.getPosition());
	t.goal = b;
	return b;
}

//------------------------------------------------------------------------------
std::vector<cPosition> cCommandos::routeTo (const cModel& model, const cMapView& view, const cVehicle& c, const cPosition& dest, sTask& t,
	bool stealth) const
{
	const cPosition at = c.getPosition();
	if (!t.route.empty() && t.route.back() == dest)
	{
		// где коммандос на маршруте (или маршрут ещё не начат)
		size_t i = 0;
		bool on = false;
		for (size_t k = 0; k < t.route.size(); ++k)
			if (t.route[k] == at)
			{
				i = k + 1;
				on = true;
				break;
			}
		if (!on && cheb (at, t.route.front()) <= 1) on = true; // старт маршрута — соседняя клетка
		if (on && i < t.route.size())
		{
			std::vector<cPosition> rest (t.route.begin() + static_cast<long> (i), t.route.end());
			bool ok = true;
			for (size_t k = 0; k < rest.size() && k < 6 && ok; ++k)
			{
				ok = view.possiblePlace (c, rest[k]) && !inDetectorZone (rest[k]);
				// следующий шаг занят (машина в тумане, которую видно только вблизи) — запомнить клетку на 10 ходов, иначе новый путь
				// из соседней клетки снова поведёт через неё (Gen_Archipelago 57: туда-обратно 30 ходов)
				if (!ok && k == 0) blocked_[static_cast<long long> (rest[k].y()) * W_ + rest[k].x()] = turn_;
			}
			if (ok) return rest;
		}
	}
	auto path = stealth ? stealthPath (model, view, c, dest, kStealthExpand) : std::vector<cPosition>();
	if (path.empty() && !stealth) path = travelPath (model, view, c, dest);
	t.route = path;
	return path;
}

//------------------------------------------------------------------------------
// A* по клеткам, как движок (цена шага — calcNextCost), но клетки в радиусе сыщиков врага запрещены (кроме старта);
// окно — прямоугольник старта и цели с запасом 12; предел узлов
std::vector<cPosition> cCommandos::stealthPath (const cModel& model, const cMapView& view, const cVehicle& v, const cPosition& dest,
	int maxExpand) const
{
	const cPosition start = v.getPosition();
	if (start == dest || !view.isValidPosition (dest) || inDetectorZone (dest)) return {};
	(void) model;
	const int x0 = std::max (0, std::min (start.x(), dest.x()) - 12), x1 = std::min (W_ - 1, std::max (start.x(), dest.x()) + 12);
	const int y0 = std::max (0, std::min (start.y(), dest.y()) - 12), y1 = std::min (H_ - 1, std::max (start.y(), dest.y()) + 12);
	const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
	const size_t n = static_cast<size_t> (bw) * bh;
	std::vector<int> g (n, std::numeric_limits<int>::max());
	std::vector<int> prev (n, -1);
	std::vector<char> closed (n, 0);
	auto idx = [&] (const cPosition& p) { return static_cast<size_t> (p.y() - y0) * bw + (p.x() - x0); };
	auto posOf = [&] (int i) { return cPosition (x0 + i % bw, y0 + i / bw); };
	using tNode = std::tuple<int, int, int>; // f, порядок клетки (детерминизм), индекс
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	const int si = static_cast<int> (idx (start));
	g[si] = 0;
	open.emplace (2 * cheb (start, dest), si, si);
	int expanded = 0;
	const int di = static_cast<int> (idx (dest));
	while (!open.empty())
	{
		const auto [f, ord, i] = open.top();
		open.pop();
		(void) f;
		(void) ord;
		if (closed[i]) continue;
		closed[i] = 1;
		if (i == di) break;
		if (++expanded > maxExpand) return {};
		const cPosition p = posOf (i);
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition q = p + cPosition (dx, dy);
				if (q.x() < x0 || q.y() < y0 || q.x() > x1 || q.y() > y1) continue;
				const int qi = static_cast<int> (idx (q));
				if (closed[qi]) continue;
				if (!view.possiblePlace (v, q)) continue;
				if (inDetectorZone (q)) continue;
				if (const auto bl = blocked_.find (static_cast<long long> (q.y()) * W_ + q.x()); bl != blocked_.end() && turn_ - bl->second <= 10) continue;
				const int c = cPathCalculator::calcNextCost (p, q, &v, &view);
				if (c <= 0) continue;
				const int ng = g[i] + c;
				if (ng >= g[qi]) continue;
				g[qi] = ng;
				prev[qi] = i;
				open.emplace (ng + 2 * cheb (q, dest), qi, qi);
			}
	}
	if (prev[di] < 0) return {};
	std::vector<cPosition> path;
	for (int i = di; i != si; i = prev[i]) path.push_back (posOf (i));
	std::reverse (path.begin(), path.end());
	return path;
}

//------------------------------------------------------------------------------
bool cCommandos::nearEnemy (const cPosition& p) const
{
	for (const auto& b : enemyBuildings_)
		if (cheb (b, p) <= kAlienEnemyGap) return true;
	// там погиб свой коммандос — сыщик врага, которого мы не видели
	for (const auto& d : deaths_)
		if (turn_ - d.second <= kDeathMemory && cheb (d.first, p) <= kDeathRadius) return true;
	return false;
}

// путь коммандос к dest: сыщики врага (память) у маршрута — в обход (A*), нет обхода — до края их радиуса; иначе — путь
// движка (дешевле на больших картах)
std::vector<cPosition> cCommandos::travelPath (const cModel& model, const cMapView& view, const cVehicle& c, const cPosition& dest) const
{
	const cPosition s = c.getPosition();
	const int x0 = std::min (s.x(), dest.x()) - 14, x1 = std::max (s.x(), dest.x()) + 14;
	const int y0 = std::min (s.y(), dest.y()) - 14, y1 = std::max (s.y(), dest.y()) + 14;
	bool risky = false;
	for (const auto& d : detectors_)
		if (d.pos.x() >= x0 && d.pos.x() <= x1 && d.pos.y() >= y0 && d.pos.y() <= y1) risky = true;
	if (risky)
	{
		auto path = stealthPath (model, view, c, dest, 4 * kStealthExpand);
		if (!path.empty()) return path;
	}
	auto path = toVec (cPathCalculator (c, view, dest, false).calcPath());
	if (risky)
		for (size_t i = 0; i < path.size(); ++i)
			if (inDetectorZone (path[i]))
			{
				path.resize (i);
				break;
			}
	return path;
}

//------------------------------------------------------------------------------
bool cCommandos::pushPath (const cVehicle& v, const cMapView& view, std::vector<cPosition> path, const std::string& reason,
	std::vector<cOrder>& out)
{
	cutToTurn (v, view, path);
	while (!path.empty() && std::find (reserved_.begin(), reserved_.end(), path.back()) != reserved_.end()) path.pop_back();
	if (path.empty()) return false;
	cOrder o;
	o.type = eOrderType::Move;
	o.playerId = playerId_;
	o.unitId = v.getId();
	o.path = std::move (path);
	o.position = o.path.back();
	o.level = "micro";
	o.reason = "commando: " + reason;
	reserved_.push_back (o.position);
	out.push_back (std::move (o));
	return true;
}

//------------------------------------------------------------------------------
bool cCommandos::act (const cModel& model, const cPlayer& player, const cVehicle& c, const cUnit& target, bool steal, const std::string& why,
	std::vector<cOrder>& out, int pass)
{
	(void) model;
	if (!cCommandoData::canDoAction (c, &target, steal)) return false;
	const int chance = c.getCommandoData().computeChance (&target, steal);
	cOrder o;
	o.type = steal ? eOrderType::Steal : eOrderType::Disable;
	o.playerId = player.getId();
	o.unitId = c.getId();
	o.unitId2 = target.getId();
	o.level = "micro";
	o.reason = std::string ("commando: ") + (steal ? "capture " : "disable ") + target.getStaticUnitData().getDefaultName() + " (" + why
		+ ", chance " + std::to_string (chance) + "%)";
	out.push_back (std::move (o));
	sTry t;
	t.commando = c.getId();
	t.target = target.getId();
	t.steal = steal;
	t.chance = chance;
	t.successBefore = static_cast<int> (c.getCommandoData().getSuccessCount());
	t.neutral = target.getOwner() == nullptr;
	t.type = target.getStaticUnitData().getDefaultName();
	t.turn = turn_;
	tries_.push_back (t);
	event ("try", {{"unitId", c.getId()}, {"target", target.getId()}, {"type", t.type}, {"action", steal ? "capture" : "disable"},
		{"chance", chance}, {"rank", rankOf (c)}, {"neutral", t.neutral}, {"why", why}, {"pass", pass}});
	wantPass_ = true; // итог — в следующем заходе
	return true;
}

//------------------------------------------------------------------------------
// работа у чужих: сначала выключить невыключенную машину (опыт, тревоги нет), потом захватывать выключенные —
// дороже и с лучшим шансом раньше; своя цель держится, пока она есть; одна машина — одному коммандос
unsigned int cCommandos::pickAlien (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& c, bool& steal,
	bool ferry) const
{
	(void) view;
	const int myRegion = world.regionOf (c, c.getPosition());
	std::set<unsigned int> taken;
	for (const auto& [id, t] : tasks_)
		if (id != c.getId() && t.task == eTask::Alien && t.target != kNoUnit) taken.insert (t.target);
	unsigned int best = kNoUnit;
	long long bestKey = std::numeric_limits<long long>::max();
	bool bestSteal = false;
	const auto cur = tasks_.find (c.getId());
	for (const auto& [id, a] : aliens_)
	{
		if (taken.count (id) != 0 || badTargets_.count (id) != 0) continue;
		if (a.ship) continue; // корабль чужих — только при флоте (без него ценность мала, а подход — с берега)
		if (home_.x() < 0 || cheb (a.pos, home_) > kAlienReach || nearEnemy (a.pos)) continue;
		const cUnit* u = model.getUnitFromID (id);
		if (u == nullptr || u->getOwner() != nullptr) continue;
		// пешком — только своя суша (область коммандос); другая — только если есть авиатранспорт
		if (!ferry && world.regionOf (c, a.pos) != myRegion) continue;
		const bool st = a.disabled; // выключена — захватывать; иначе — выключить (опыт)
		const int chance = c.getCommandoData().computeChance (u, st);
		if (chance <= 0) continue;
		// дорога (Чебышёв) — главное; среди близких — выключить раньше захвата (опыт поднимает шанс захвата), захват —
		// по ценности × шанс
		const long long d = cheb (c.getPosition(), a.pos);
		const long long value = st ? static_cast<long long> (a.cost) * chance : 1000000;
		long long key = d * 10 * 1000000LL - value;
		if (cur != tasks_.end() && cur->second.task == eTask::Alien && cur->second.target == id) key -= 50LL * 1000000; // держаться своей цели
		if (key < bestKey)
		{
			bestKey = key;
			best = id;
			bestSteal = st;
		}
	}
	steal = bestSteal;
	return best;
}

//------------------------------------------------------------------------------
// диверсия (П18): здание врага из памяти, которое можно выключить, с клеткой рядом вне радиуса сыщиков и высоким шансом
unsigned int cCommandos::pickSabotage (const cModel& model, const cPlayer& player, const cWorldModel& world, const cVehicle& c) const
{
	if (rankOf (c) < kSabotageRank) return kNoUnit;
	const auto& ud = *model.getUnitsData();
	const cMap& map = *model.getMap();
	unsigned int best = kNoUnit;
	long long bestKey = std::numeric_limits<long long>::max();
	std::set<unsigned int> taken;
	for (const auto& [id, t] : tasks_)
		if (id != c.getId() && t.task == eTask::Sabotage && t.target != kNoUnit) taken.insert (t.target);
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.building || m.owner < 0 || taken.count (m.id) != 0 || badTargets_.count (m.id) != 0) continue;
		const sID id (m.typeFirst, m.typeSecond);
		if (!ud.isValidId (id)) continue;
		const auto& sd = ud.getStaticUnitData (id);
		if (!sd.canBeDisabled) continue;
		const int value = sabotageValue (sd);
		if (value <= 0) continue;
		bool deadly = false; // там погиб свой коммандос — не туда
		for (const auto& d : deaths_)
			if (turn_ - d.second <= kDeathMemory && cheb (d.first, m.pos) <= kDeathRadius) deadly = true;
		if (deadly) continue;
		// пешком — только своя суша; за водой — если есть БМП (под водой её видит только корвет)
		if (world.regionOf (c, m.pos) != world.regionOf (c, c.getPosition()) && (counts_.apcs == 0 || turn_ < apcStuckUntil_)) continue;
		const cUnit* u = model.getUnitFromID (m.id);
		if (u == nullptr) continue;
		if (player.canSeeUnit (*u, map) && u->isDisabled()) continue; // уже выключено (видно)
		const int chance = c.getCommandoData().computeChance (u, false);
		if (chance < kSabotageChance) continue;
		// клетки у цели вне радиуса сыщиков (по памяти) — есть хоть одна
		bool free = false;
		for (const auto& q : u->getPositions())
			for (int dy = -1; dy <= 1 && !free; ++dy)
				for (int dx = -1; dx <= 1 && !free; ++dx)
				{
					const cPosition p = q + cPosition (dx, dy);
					free = map.isValidPosition (p) && besideUnit (*u, p) && !inDetectorZone (p);
				}
		if (!free) continue;
		const long long key = static_cast<long long> (cheb (c.getPosition(), m.pos)) * 1000 - value * 10 - chance;
		if (key < bestKey)
		{
			bestKey = key;
			best = m.id;
		}
	}
	return best;
}

//------------------------------------------------------------------------------
// обнаружен: уйти из зоны огня (с места и с подъезда), подальше от врагов и сыщиков, ближе к дому
bool cCommandos::escape (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& c,
	std::vector<cOrder>& out)
{
	(void) player;
	const int budget = c.data.getSpeed();
	if (budget <= 0) return false;
	const cPosition start = c.getPosition();
	const int R = std::max (1, budget / 4 + 1);
	const int bw = 2 * R + 1;
	std::vector<int> g (static_cast<size_t> (bw) * bw, std::numeric_limits<int>::max());
	std::vector<int> prev (g.size(), -1);
	auto idx = [&] (const cPosition& p) { return (p.y() - start.y() + R) * bw + (p.x() - start.x() + R); };
	auto posOf = [&] (int i) { return cPosition (start.x() - R + i % bw, start.y() - R + i / bw); };
	using tNode = std::pair<int, int>;
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	g[idx (start)] = 0;
	open.emplace (0, idx (start));
	while (!open.empty())
	{
		const auto [d, i] = open.top();
		open.pop();
		if (d > g[i]) continue;
		const cPosition p = posOf (i);
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition q = p + cPosition (dx, dy);
				if (cheb (q, start) > R || !view.isValidPosition (q) || !view.possiblePlace (c, q)) continue;
				const int cost = cPathCalculator::calcNextCost (p, q, &c, &view);
				if (cost <= 0 || d + cost > budget) continue;
				const int qi = idx (q);
				if (d + cost >= g[qi]) continue;
				g[qi] = d + cost;
				prev[qi] = i;
				open.emplace (d + cost, qi);
			}
	}
	// враги рядом (память, машины — свежие) — от них дальше
	std::vector<cPosition> foes;
	for (const auto& kv : world.enemies())
		if ((kv.second.building || world.age (kv.second) <= 2) && dist2 (kv.second.pos, start) <= 20 * 20) foes.push_back (kv.second.pos);
	long long bestKey = std::numeric_limits<long long>::min();
	int bestI = -1;
	for (int i = 0; i < static_cast<int> (g.size()); ++i)
	{
		if (g[i] == std::numeric_limits<int>::max()) continue;
		const cPosition p = posOf (i);
		if (std::find (reserved_.begin(), reserved_.end(), p) != reserved_.end() && p != start) continue;
		long long minFoe = 400;
		for (const auto& f : foes) minFoe = std::min (minFoe, dist2 (p, f));
		const int danger = world.groundFire (p) > 0 ? 2 : (world.groundStrike (p) > 0 ? 1 : 0);
		const long long key = -static_cast<long long> (danger) * 100000000LL - (inDetectorZone (p) ? 10000000LL : 0) + minFoe * 1000
			- (home_.x() >= 0 ? cheb (p, home_) : 0) * 10 - (p.y() * W_ + p.x()) % 7;
		if (key > bestKey)
		{
			bestKey = key;
			bestI = i;
		}
	}
	if (bestI < 0 || posOf (bestI) == start) return false;
	std::vector<cPosition> path;
	for (int i = bestI; i != idx (start); i = prev[i]) path.push_back (posOf (i));
	std::reverse (path.begin(), path.end());
	(void) model;
	return pushPath (c, view, std::move (path), "detected: away from the enemy (vanish next turn: step from a cell no infantry sees)", out);
}

//------------------------------------------------------------------------------
void cCommandos::noteFerried (unsigned int commando, bool on)
{
	if (on) ferried_.insert (commando);
	else if (ferried_.erase (commando) != 0) freshDrop_.insert (commando);
}

//------------------------------------------------------------------------------
void cCommandos::requestRaid (unsigned int target, const cPosition& pos, bool big, int turn)
{
	if (raid_.target == target) return;
	raid_ = {};
	raid_.target = target;
	raid_.pos = pos;
	raid_.big = big;
	raid_.since = turn;
	event ("raid request", {{"target", target}, {"pos", posJ (pos)}});
}

void cCommandos::cancelRaid (const char* why)
{
	if (raid_.target == kNoUnit) return;
	event ("raid cancelled", {{"target", raid_.target}, {"commando", raid_.commando == kNoUnit ? -1 : static_cast<long long> (raid_.commando)},
		{"why", why}});
	if (raid_.commando != kNoUnit)
		if (auto it = tasks_.find (raid_.commando); it != tasks_.end()) it->second.task = eTask::Idle;
	raid_ = {};
}

int cCommandos::nearestFree (const cModel& model, const cWorldModel& world, const cPosition& p) const
{
	int best = std::numeric_limits<int>::max();
	for (const auto& [id, pos] : lastPos_)
	{
		if (ferried_.count (id) != 0) continue;
		const auto t = tasks_.find (id);
		if (t != tasks_.end() && t->second.task == eTask::Escape) continue;
		// пешком: рядом с p есть клетка той же области, что у коммандос (налёт s13p3r — коммандос на другом острове 30 ходов «шёл»)
		const cVehicle* v = model.getVehicleFromID (id);
		if (v == nullptr) continue;
		const int region = world.regionOf (*v, pos);
		bool same = false;
		for (int dy = -1; dy <= 1 && !same; ++dy)
			for (int dx = -1; dx <= 1 && !same; ++dx)
				same = (dx != 0 || dy != 0) && world.regionOf (*v, p + cPosition (dx, dy)) == region;
		if (!same) continue;
		best = std::min (best, cheb (pos, p));
	}
	return best;
}

bool cCommandos::raidReady (const cModel& model) const
{
	if (raid_.target == kNoUnit || raid_.commando == kNoUnit) return false;
	const cVehicle* c = model.getVehicleFromID (raid_.commando);
	const cUnit* u = model.getUnitFromID (raid_.target);
	if (c == nullptr || u == nullptr || c->isUnitLoaded() || c->isDisabled() || c->isDetectedByAnyPlayer()) return false;
	if (u->isDisabled() || c->data.getShots() <= 0) return false;
	return besideUnit (*u, c->getPosition());
}

bool cCommandos::raidDisable (const cModel& model, const cPlayer& player, std::vector<cOrder>& out, int pass)
{
	if (!raidReady (model)) return false;
	const cVehicle* c = model.getVehicleFromID (raid_.commando);
	const cUnit* u = model.getUnitFromID (raid_.target);
	return act (model, player, *c, *u, false, "air raid: silence the AA before the bombers", out, pass);
}

//------------------------------------------------------------------------------
void cCommandos::plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, std::vector<cOrder>& out)
{
	(void) defense;
	claimed_.clear();
	reserved_.clear();
	pending_.clear();
	pendingSteal_.clear();
	ferry_.clear();
	wantPass_ = false;
	checkTries (model, player);
	const cMap& map = *model.getMap();

	// налёт: цели нет (уничтожена, наша) — снять; коммандос налёта нет — ближайший свободный
	if (raid_.target != kNoUnit)
	{
		const cUnit* u = model.getUnitFromID (raid_.target);
		if (u == nullptr || u->getOwner() == &player || u->getOwner() == nullptr) cancelRaid ("target gone");
		else if (turn_ - raid_.since > kRaidStale && !raidReady (model)) cancelRaid ("commando could not get next to the target in time");
	}
	std::vector<const cVehicle*> mine;
	for (const auto& v : player.getVehicles())
		if (roles.roleOf (*v) == eRole::Commando) mine.push_back (v.get());
	for (const cVehicle* c : mine) claimed_.insert (c->getId());
	if (raid_.target != kNoUnit && raid_.commando == kNoUnit)
	{
		long long best = std::numeric_limits<long long>::max();
		for (const cVehicle* c : mine)
		{
			if (c->isDisabled() || c->isDetectedByAnyPlayer() || ferried_.count (c->getId()) != 0) continue;
			// дойдёт пешком: у зенитки есть клетка его области
			const int region = world.regionOf (*c, c->getPosition());
			bool same = false;
			for (int dy = -1; dy <= 1 && !same; ++dy)
				for (int dx = -1; dx <= 1 && !same; ++dx)
					same = (dx != 0 || dy != 0) && world.regionOf (*c, raid_.pos + cPosition (dx, dy)) == region;
			if (!same) continue;
			const long long d = dist2 (c->getPosition(), raid_.pos) * 100 + rankOf (*c) * -10;
			if (d < best)
			{
				best = d;
				raid_.commando = c->getId();
			}
		}
		if (raid_.commando != kNoUnit)
		{
			tasks_[raid_.commando] = {eTask::Raid, raid_.target, raid_.pos, turn_, "air raid"};
			event ("raid commando", {{"unitId", raid_.commando}, {"target", raid_.target}});
		}
	}

	for (const cVehicle* cp : mine) planOne (model, player, view, world, *cp, out, 1);
	apcPlan (model, player, view, world, roles, out);
}

//------------------------------------------------------------------------------
// один коммандос: задачи по приоритету (см. заголовок); pass — заход (после высадки с транспорта — тот же ход)
void cCommandos::planOne (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& c,
	std::vector<cOrder>& out, int pass)
{
	const cMap& map = *model.getMap();
	const unsigned int id = c.getId();
	if (c.isUnitLoaded() || c.isDisabled() || ferried_.count (id) != 0) return;
	if (c.isUnitMoving() || c.getMoveJob() != nullptr) return;
	sTask& t = tasks_[id];
	// 1) обнаружен — уйти
	if (c.isDetectedByAnyPlayer())
	{
		if (t.task != eTask::Escape)
		{
			event ("escape", {{"unitId", id}, {"pos", posJ (c.getPosition())}, {"from", static_cast<int> (t.task)}});
			if (raid_.commando == id) cancelRaid ("raid commando detected");
			t = {eTask::Escape, kNoUnit, {-1, -1}, turn_, "detected"};
		}
		escape (model, player, view, world, c, out);
		return;
	}
	if (t.task == eTask::Escape)
	{
		event ("vanished", {{"unitId", id}, {"pos", posJ (c.getPosition())}});
		t = {};
	}
	// 2) налёт
	if (raid_.commando == id && raid_.target != kNoUnit)
	{
		const cUnit* u = model.getUnitFromID (raid_.target);
		if (u != nullptr && besideUnit (*u, c.getPosition()))
			return; // рядом — ждать хода удара (выключит по сигналу ai_air)
		if (u != nullptr)
		{
			const cPosition dest = stickyBeside (view, c, *u, t);
			if (dest.x() < 0)
			{
				badTargets_.insert (raid_.target);
				cancelRaid ("no cell next to the target outside enemy infantry sight");
				t = {};
			}
			else if (cheb (c.getPosition(), dest) > kWalkFar)
			{
				// к зенитке налёта — только пешком (авиатранспорт с коммандос над базой врага сбивают: Gen_Straits 44 — 2 из 3)
				auto path = routeTo (model, view, c, dest, t, false);
				if (!path.empty()) pushPath (c, view, std::move (path), "to the raid target (far)", out);
			}
			else
			{
				auto path = routeTo (model, view, c, dest, t, true);
				if (path.empty() || !pushPath (c, view, std::move (path), "sneaks to the AA of the raid", out))
					if (turn_ - raid_.since > 6) cancelRaid ("no stealthy path to the target");
			}
			return;
		}
	}
	// 2½) рядом машина врага, захватить которую шанс высок (обученный коммандос): захват — по шансу и цене неудачи (П2: «захват,
	// если есть выбор, — только прокачанным»); неудача у машины с хозяином — тревога, поэтому только при шансе ≥ kStealChance
	if (c.data.getShots() > 0)
	{
		const cUnit* best = nullptr;
		int bestValue = 0;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition q = c.getPosition() + cPosition (dx, dy);
				if ((dx == 0 && dy == 0) || !map.isValidPosition (q)) continue;
				const cVehicle* u = map.getField (q).getVehicle();
				if (u == nullptr || u->getOwner() == nullptr || u->getOwner() == &player || !player.canSeeUnit (*u, map)) continue;
				if (!cCommandoData::canDoAction (c, u, true)) continue;
				const int chance = c.getCommandoData().computeChance (u, true);
				if (chance < kStealChance) continue;
				const int value = u->data.getBuildCost() * chance;
				if (value > bestValue)
				{
					bestValue = value;
					best = u;
				}
			}
		if (best != nullptr && act (model, player, c, *best, true, "capture an enemy vehicle next to us (high chance)", out, pass)) return;
	}
	// 3) после удачной диверсии — отойти и переждать
	if (t.task == eTask::Sabotage && t.why == "done")
	{
		if (turn_ - t.since >= kSabotageCooldown)
			t = {};
		else
		{
			if (turn_ == t.since + 1 || inDetectorZone (c.getPosition()))
			{
				// на 4–6 клеток от здания, вне радиуса сыщиков (враг обыщет клетки у выключенного)
				cPosition best (-1, -1);
				long long bestKey = std::numeric_limits<long long>::max();
				for (int dy = -6; dy <= 6; ++dy)
					for (int dx = -6; dx <= 6; ++dx)
					{
						const cPosition q = c.getPosition() + cPosition (dx, dy);
						const int r = std::max (std::abs (dx), std::abs (dy));
						if (r < 4 || !view.isValidPosition (q) || !view.possiblePlace (c, q) || inDetectorZone (q)) continue;
						if (world.groundStrike (q) > 0) continue;
						const long long key = (home_.x() >= 0 ? cheb (q, home_) : 0) * 1000LL + static_cast<long long> (q.y()) * W_ + q.x();
						if (key < bestKey)
						{
							bestKey = key;
							best = q;
						}
					}
				if (best.x() >= 0)
				{
					auto path = stealthPath (model, view, c, best, kStealthExpand);
					if (!path.empty()) pushPath (c, view, std::move (path), "sabotage done: step away before the enemy sweeps", out);
				}
			}
			return;
		}
	}
	// 4) чужие: выключить (опыт), потом захватить
	{
		bool steal = false;
		const unsigned int target = pickAlien (model, view, world, c, steal, ferryAvailable_);
		const cUnit* u = target != kNoUnit ? model.getUnitFromID (target) : nullptr;
		if (u != nullptr)
		{
			if (t.task != eTask::Alien || t.target != target)
			{
				event ("alien target", {{"unitId", id}, {"target", target}, {"type", u->getStaticUnitData().getDefaultName()},
					{"action", steal ? "capture" : "disable"}, {"chance", c.getCommandoData().computeChance (u, steal)},
					{"distance", cheb (c.getPosition(), u->getPosition())}, {"rank", rankOf (c)}});
				t = {eTask::Alien, target, u->getPosition(), turn_, steal ? "capture" : "train"};
			}
			const char* why = steal ? "alien: capture (no alarm, no owner)" : "alien: train on it (no alarm, rank grows)";
			if (besideUnit (*u, c.getPosition()))
			{
				if (c.data.getShots() > 0) act (model, player, c, *u, steal, why, out, pass);
				return;
			}
			const cPosition dest = stickyBeside (view, c, *u, t);
			if (dest.x() < 0)
			{
				badTargets_.insert (target);
				event ("alien unreachable", {{"unitId", id}, {"target", target}, {"why", "no free cell next to it"}});
				t = {};
				return;
			}
			// за водой: пешком не дойти — ждать перевозки (авиатранспорт или БМП), её нет — цель не годна
			if (world.regionOf (c, dest) != world.regionOf (c, c.getPosition()))
			{
				if (ferryAvailable_)
				{
					ferry_.push_back ({id, c.getPosition(), dest, "alien site over water"});
					return;
				}
				badTargets_.insert (target);
				t = {};
				return;
			}
			std::vector<cPosition> path;
			if (cheb (c.getPosition(), dest) > kWalkFar)
			{
				ferry_.push_back ({id, c.getPosition(), dest, "alien site far to walk"});
				path = routeTo (model, view, c, dest, t, false);
			}
			else
			{
				path = routeTo (model, view, c, dest, t, true);
				if (path.empty()) path = routeTo (model, view, c, dest, t, false);
			}
			if (path.empty())
			{
				badTargets_.insert (target);
				event ("alien unreachable", {{"unitId", id}, {"target", target}, {"why", "no path"}});
				t = {};
				return;
			}
			const size_t full = path.size();
			std::vector<cPosition> cut = path;
			cutToTurn (c, view, cut);
			if (pushPath (c, view, path, std::string ("to ") + u->getStaticUnitData().getDefaultName() + " (" + why + ")", out)
				&& cut.size() == full)
			{
				pending_[id] = target; // дошёл — действовать в следующем заходе (стреляет на ходу)
				pendingSteal_[id] = steal;
				wantPass_ = true;
			}
			return;
		}
	}
	// 5) диверсия у врага (обученный, шанс высок, рядом с целью нет известной пехоты)
	{
		const unsigned int target = pickSabotage (model, player, world, c);
		const cUnit* u = target != kNoUnit ? model.getUnitFromID (target) : nullptr;
		if (u != nullptr)
		{
			if (t.task != eTask::Sabotage || t.target != target)
			{
				event ("sabotage target", {{"unitId", id}, {"target", target}, {"type", u->getStaticUnitData().getDefaultName()},
					{"chance", c.getCommandoData().computeChance (u, false)}, {"rank", rankOf (c)},
					{"distance", cheb (c.getPosition(), u->getPosition())}});
				t = {eTask::Sabotage, target, u->getPosition(), turn_, "go"};
			}
			if (besideUnit (*u, c.getPosition()))
			{
				if (c.data.getShots() > 0 && !u->isDisabled() && player.canSeeUnit (*u, map))
					act (model, player, c, *u, false, "sabotage (П18)", out, pass);
				return;
			}
			const cPosition dest = stickyBeside (view, c, *u, t);
			std::vector<cPosition> path;
			if (dest.x() >= 0 && world.regionOf (c, dest) != world.regionOf (c, c.getPosition()))
			{
				// за водой — ждать БМП (только она: авиатранспорт над базой врага сбивают)
				ferry_.push_back ({id, c.getPosition(), dest, "sabotage target beyond water: APC under water", true});
				return;
			}
			if (dest.x() >= 0)
			{
				if (cheb (c.getPosition(), dest) > kWalkFar)
				{
					ferry_.push_back ({id, c.getPosition(), dest, "sabotage target far to walk", true});
					path = routeTo (model, view, c, dest, t, false);
				}
				else
					path = routeTo (model, view, c, dest, t, true);
			}
			if (path.empty())
			{
				badTargets_.insert (target);
				t = {};
			}
			else
			{
				const size_t full = path.size();
				std::vector<cPosition> cut = path;
				cutToTurn (c, view, cut);
				if (pushPath (c, view, path, std::string ("sneaks to ") + u->getStaticUnitData().getDefaultName() + " (sabotage)", out)
					&& cut.size() == full && cheb (path.back(), dest) == 0)
				{
					pending_[id] = target;
					pendingSteal_[id] = false;
					wantPass_ = true;
				}
				return;
			}
		}
	}
	// 6) без дела — у центра базы
	if (t.task != eTask::Idle) t = {};
	if (home_.x() >= 0 && cheb (c.getPosition(), home_) > kHomeHold)
	{
		cPosition dest (-1, -1);
		for (int r = 0; r <= kHomeHold && dest.x() < 0; ++r)
			for (int dy = -r; dy <= r && dest.x() < 0; ++dy)
				for (int dx = -r; dx <= r && dest.x() < 0; ++dx)
				{
					if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
					const cPosition q = home_ + cPosition (dx, dy);
					if (view.isValidPosition (q) && view.possiblePlace (c, q) && map.getField (q).getBuildings().empty()) dest = q;
				}
		if (dest.x() >= 0)
		{
			auto path = toVec (cPathCalculator (c, view, dest, false).calcPath());
			if (!path.empty()) pushPath (c, view, std::move (path), "idle: wait at the base centre", out);
		}
	}
}

//------------------------------------------------------------------------------
std::vector<cOrder> cCommandos::planPass (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, int pass)
{
	std::vector<cOrder> out;
	wantPass_ = false;
	checkTries (model, player);
	for (const auto& [id, target] : std::exchange (pending_, {}))
	{
		const cVehicle* c = model.getVehicleFromID (id);
		const cUnit* u = model.getUnitFromID (target);
		if (c == nullptr || u == nullptr || c->isUnitMoving() || c->isDetectedByAnyPlayer()) continue;
		if (!besideUnit (*u, c->getPosition()) || c->data.getShots() <= 0) continue;
		const bool steal = pendingSteal_[id];
		if (!steal && u->isDisabled()) continue;
		act (model, player, *c, *u, steal, u->getOwner() == nullptr ? (steal ? "alien: capture" : "alien: train") : "sabotage (П18)", out, pass);
	}
	pendingSteal_.clear();
	// высажен с авиатранспорта в этом ходу — ход у него цел: к цели и действовать
	for (unsigned int id : std::exchange (freshDrop_, {}))
		if (const cVehicle* c = model.getVehicleFromID (id); c != nullptr && claimed_.count (id) != 0 && !c->isUnitLoaded() && !c->isUnitMoving())
			planOne (model, player, view, world, *c, out, pass);
	return out;
}

//------------------------------------------------------------------------------
void cCommandos::apcPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	std::vector<cOrder>& out)
{
	// БМП (П2: «коммандос дёшевы: возить в БМП под водой — её видит только корвет; высаживать на берег, БМП оставлять под
	// водой»): цель коммандос — за водой, авиатранспорта нет. Задание — первая просьба о перевозке на другую сушу
	const cMap& map = *model.getMap();
	const cVehicle* apc = apc_.apc != kNoUnit ? model.getVehicleFromID (apc_.apc) : nullptr;
	if (apc == nullptr)
	{
		if (apc_.commando != kNoUnit) noteFerried (apc_.commando, false);
		apc_ = {};
		for (const auto& v : player.getVehicles())
			if (roles.roleOf (*v) == eRole::Apc && !v->isUnitLoaded() && !v->isDisabled())
			{
				apc = v.get();
				apc_.apc = v->getId();
				break;
			}
		if (apc == nullptr) return;
	}
	claimed_.insert (apc->getId());
	if (apc->isUnitMoving() || apc->getMoveJob() != nullptr || apc->isDisabled()) return;
	const cVehicle* c = apc_.commando != kNoUnit ? model.getVehicleFromID (apc_.commando) : nullptr;
	if (apc_.commando != kNoUnit && c == nullptr)
	{
		apc_.commando = kNoUnit;
		apc_.state = 0;
	}
	if (apc_.commando == kNoUnit)
	{
		for (const auto& r : ferry_)
		{
			if (ferryAvailableAir_ && !r.stealthOnly) continue; // к чужим — авиатранспорт (быстрее)
			const cVehicle* rc = model.getVehicleFromID (r.commando);
			if (rc == nullptr) continue;
			// только за воду: пешком до цели не дойти, а БМП доплывёт (одна область для класса БМП)
			if (world.regionOf (*rc, r.dest) == world.regionOf (*rc, rc->getPosition())) continue;
			if (world.regionOf (*apc, r.dest) != world.regionOf (*apc, apc->getPosition())) continue;
			apc_.commando = r.commando;
			apc_.dest = r.dest;
			apc_.state = 0;
			apc_.since = turn_;
			noteFerried (r.commando, true);
			c = rc;
			event ("apc ferry", {{"apc", apc->getId()}, {"commando", r.commando}, {"dest", posJ (r.dest)}, {"why", r.why}});
			break;
		}
		if (apc_.commando == kNoUnit) return;
	}
	const bool inside = c->isUnitLoaded() && std::find (apc->storedUnits.begin(), apc->storedUnits.end(), c) != apc->storedUnits.end();
	if (apc_.state == 0)
	{
		if (inside)
			apc_.state = 1;
		else if (turn_ - apc_.since > 15)
		{
			event ("apc ferry dropped", {{"apc", apc->getId()}, {"commando", apc_.commando},
				{"why", "could not pick up in 15 turns (APC boxed in by our units?): no APC trips for 30 turns"}});
			noteFerried (apc_.commando, false);
			apc_.commando = kNoUnit;
			apc_.meet = cPosition (-1, -1);
			apcStuckUntil_ = turn_ + 30;
			return;
		}
		else if (apc->canLoad (c))
		{
			cOrder o;
			o.type = eOrderType::Load;
			o.playerId = playerId_;
			o.unitId = apc->getId();
			o.unitId2 = c->getId();
			o.level = "micro";
			o.reason = "commando: APC takes the commando (under water only corvettes see it)";
			out.push_back (std::move (o));
			wantPass_ = true;
			return;
		}
		else
		{
			// встреча: клетка q для БМП (не здание; ближе к коммандос, до 12 клеток) и соседняя с ней клетка для коммандос —
			// БМП в тесноте базы к нему не подойти (выезды фабрик заставлены армией)
			const cPosition cp = c->getPosition();
			if (apc_.meet.x() < 0 || (apc_.meet != apc->getPosition() && !view.possiblePlace (*apc, apc_.meet)))
			{
				apc_.meet = cPosition (-1, -1);
				for (int r = 1; r <= 12 && apc_.meet.x() < 0; ++r)
				{
					long long bk = std::numeric_limits<long long>::max();
					for (int dy = -r; dy <= r; ++dy)
						for (int dx = -r; dx <= r; ++dx)
						{
							if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
							const cPosition q = cp + cPosition (dx, dy);
							if (!view.isValidPosition (q) || !map.getField (q).getBuildings().empty()) continue;
							if (q != apc->getPosition() && !view.possiblePlace (*apc, q)) continue;
							bool side = false;
							for (int ey = -1; ey <= 1 && !side; ++ey)
								for (int ex = -1; ex <= 1 && !side; ++ex)
								{
									const cPosition rr = q + cPosition (ex, ey);
									side = (ex != 0 || ey != 0) && view.isValidPosition (rr) && (rr == cp || view.possiblePlace (*c, rr));
								}
							if (!side) continue;
							const long long key = dist2 (q, cp) * 100000 + static_cast<long long> (q.y()) * W_ + q.x();
							if (key < bk)
							{
								bk = key;
								apc_.meet = q;
							}
						}
				}
			}
			if (apc_.meet.x() >= 0 && apc->getPosition() != apc_.meet)
			{
				auto path = toVec (cPathCalculator (*apc, view, apc_.meet, false).calcPath());
				if (!path.empty()) pushPath (*apc, view, std::move (path), "APC to the meeting point with the commando", out);
			}
			// коммандос — к клетке рядом со встречей (БМП уже там — войти в неё)
			if (apc_.meet.x() >= 0 && cheb (cp, apc_.meet) > 1 && !c->isUnitMoving() && c->data.getSpeed() > 0)
			{
				cPosition side (-1, -1);
				long long sk = std::numeric_limits<long long>::max();
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const cPosition q = apc_.meet + cPosition (dx, dy);
						if ((dx == 0 && dy == 0) || !view.isValidPosition (q)) continue;
						if (q != cp && !view.possiblePlace (*c, q)) continue;
						const long long d = dist2 (q, cp);
						if (d < sk)
						{
							sk = d;
							side = q;
						}
					}
				if (side.x() >= 0 && side != cp)
				{
					auto full = toVec (cPathCalculator (*c, view, side, false).calcPath());
					const size_t n = full.size();
					auto path = full;
					cutToTurn (*c, view, path);
					if (!path.empty())
					{
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId_;
						o.unitId = c->getId();
						o.path = path;
						o.position = path.back();
						if (path.size() == n && apc->getPosition() == apc_.meet)
						{
							o.unitId2 = apc->getId();
							o.moveEnd = eMoveEnd::GetIn;
						}
						o.level = "micro";
						o.reason = "commando: to the APC meeting point";
						out.push_back (std::move (o));
						wantPass_ = true;
					}
				}
			}
			else if (apc_.meet.x() >= 0 && apc->getPosition() == apc_.meet && c->data.getSpeed() > 0 && !c->isUnitMoving())
			{
				// рядом — войти (шаг на месте не нужен: БМП берёт соседа приказом Load в следующем заходе)
				wantPass_ = true;
			}
			return;
		}
	}
	if (apc_.state >= 1 && inside)
	{
		// берег высадки: клетка воды или берега, соседняя с сушей области цели, ближе к цели; БМП там встанет
		if (apc_.landing.x() < 0)
		{
			const int targetRegion = world.regionOf (*c, apc_.dest);
			long long bk = std::numeric_limits<long long>::max();
			for (int dy = -30; dy <= 30; ++dy) // базы врага бывают в 18 клетках от моря (карты генератора)
				for (int dx = -30; dx <= 30; ++dx)
				{
					const cPosition q = apc_.dest + cPosition (dx, dy);
					if (!map.isValidPosition (q) || (!map.isWater (q) && !map.isCoast (q))) continue;
					if (q != apc->getPosition() && !view.possiblePlace (*apc, q)) continue;
					if (inDetectorZone (q)) continue;
					bool land = false;
					for (int ey = -1; ey <= 1 && !land; ++ey)
						for (int ex = -1; ex <= 1 && !land; ++ex)
						{
							const cPosition l = q + cPosition (ex, ey);
							land = map.isValidPosition (l) && world.regionOf (*c, l) == targetRegion && !map.isWater (l);
						}
					if (!land) continue;
					const long long key = dist2 (q, apc_.dest) * 10 + (map.isWater (q) ? 0 : 5); // вода — скрыта
					if (key < bk)
					{
						bk = key;
						apc_.landing = q;
					}
				}
			if (apc_.landing.x() < 0)
			{
				event ("apc ferry dropped", {{"apc", apc->getId()}, {"commando", apc_.commando}, {"why", "no landing cell next to the target land"}});
				apc_.state = 2;
				apc_.landing = apc->getPosition();
			}
		}
		if (apc->getPosition() == apc_.landing || apc_.state == 2)
		{
			// выгрузить на сушу рядом (не тратит ход коммандос)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const cPosition l = apc->getPosition() + cPosition (dx, dy);
					if ((dx == 0 && dy == 0) || !map.isValidPosition (l) || !apc->canExitTo (l, map, c->getStaticUnitData())) continue;
					if (inDetectorZone (l)) continue;
					cOrder o;
					o.type = eOrderType::ActivateUnit;
					o.playerId = playerId_;
					o.unitId = apc->getId();
					o.unitId2 = c->getId();
					o.position = l;
					o.level = "micro";
					o.reason = "commando: APC lands the commando on the shore (the APC stays under water)";
					out.push_back (std::move (o));
					event ("apc landed", {{"apc", apc->getId()}, {"commando", c->getId()}, {"at", posJ (l)}});
					++counts_.apcLandings;
					noteFerried (c->getId(), false);
					apc_.commando = kNoUnit;
					apc_.state = 0;
					apc_.landing = cPosition (-1, -1);
					wantPass_ = true;
					return;
				}
			return;
		}
		auto path = toVec (cPathCalculator (*apc, view, apc_.landing, false).calcPath());
		if (!path.empty()) pushPath (*apc, view, std::move (path), "APC carries the commando under water to the shore", out);
		else apc_.landing = cPosition (-1, -1);
	}
}

} // namespace ai
