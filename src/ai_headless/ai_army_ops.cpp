/***************************************************************************
 * ai_army_ops.cpp — операции армии (шаг 8 PLAN.md). Описание — в заголовке.
 ***************************************************************************/

#include "ai_army_ops.h"

#include "ai_roles.h"
#include "ai_world_model.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/id.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <tuple>
#include <utility>

namespace ai
{

namespace
{

// --- константы (подобраны турнирами против base: отчёт шага 8, §4) ----------
constexpr int kMarginPct = 150;     // нужно = sqrt(1.5 × урон за ход × hp защитников)
constexpr int kRetreatPct = 70;     // отход, когда сила < 0.7 × нужного
constexpr int kMinLaunch = 10;      // машин в группе — не меньше для выхода (подмога — половина)
constexpr int kTotalPct = 70;       // для выхода — и не слабее 0.7 × всей известной армии врага
constexpr int kPeakDecay = 990;     // пик известной армии врага тает за ход до 99% (‰)
constexpr int kArmyHome = 20;       // цель «армия» — только ближе к базе
constexpr int kDefend = 12;         // в ожидании: враг ближе к точке сбора — к краю его зоны огня
constexpr int kScouts = 2;          // разведчиков, когда цели нет
constexpr int kScoutsKnown = 1;     // разведчиков, когда цель есть
constexpr int kReserve = 2;         // резерв обороны у базы
constexpr int kGatherTurns = 6;     // ждать отставших не дольше
constexpr int kGatherPct = 80;      // собрались — эта доля группы у точки сбора
constexpr int kPacePct = 70;        // лидер идёт на эту долю своего хода
constexpr int kCohesionPct = 70;    // лидер шагает, когда рядом эта доля группы
constexpr int kCohesionCap = 40;    // шаг 13: доля — от стольких машин (не от всей огромной группы)
constexpr int kEngage = 10;         // ближе к цели — бить
constexpr int kDefRadius = 15;      // защитники цели — враги ближе к ней
constexpr int kSense = 12;          // враги у группы — ближе к лидеру
constexpr int kCooldown = 4;        // после отхода — ходов до нового выхода (если сил не больше)
constexpr int kMaxCommitted = 2;    // операций в походе одновременно
constexpr int kRallyGap = 3;        // точка сбора — не ближе к своим зданиям (Чебышёв)
constexpr int kRallyMax = 24;       // и не дальше от базы по пути (клеток)
constexpr int kSearchTurns = 4;     // Search: осматриваться не дольше
constexpr int kRetreatTurns = 12;   // отход: не дольше — потом распустить

constexpr int kSearchRadius = 15;  // Search: другая цель — не дальше от группы
constexpr int kFormationSearchMax = 6000; // шаг 13: строй — A* машины, которую не достало поле, не больше стольких клеток
constexpr int kMergeDist = 8;      // подмога сливается с операцией, когда лидеры ближе
constexpr int kReserveRadius = 8;  // резерв стоит, пока не дальше от базы
constexpr int kHealthyPct = 60;    // лидер — машина не ниже этой доли hp
constexpr int kStatusEvery = 10;   // снимок всех операций в журнал — раз в столько ходов

const sID kIdTank (0, 32);

int cheb (const cPosition& a, const cPosition& b)
{
	return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
}

long long dist2 (const cPosition& a, const cPosition& b)
{
	return (a - b).l2NormSquared();
}

long long isqrtll (long long v)
{
	if (v <= 0) return 0;
	long long r = static_cast<long long> (std::sqrt (static_cast<double> (v)));
	while (r > 0 && r * r > v) --r;
	while ((r + 1) * (r + 1) <= v) ++r;
	return r;
}

bool contains (const std::vector<unsigned int>& v, unsigned int id)
{
	return std::find (v.begin(), v.end(), id) != v.end();
}

bool containsPos (const std::vector<cPosition>& v, const cPosition& p)
{
	return std::find (v.begin(), v.end(), p) != v.end();
}

nlohmann::json posJson (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

const char* targetKindName (eOpTarget k)
{
	switch (k)
	{
		case eOpTarget::Building: return "building";
		case eOpTarget::Army: return "army";
		case eOpTarget::Point: return "point";
		default: return "none";
	}
}

// местность проходима для танка (машины и здания под туманом не видны — не учитываем)
bool tankTerrain (const cMap& map, const cPosition& p)
{
	if (!map.isValidPosition (p)) return false;
	return !map.isBlocked (p) && !map.isWater (p) && !map.isCoast (p);
}

// путь до хода: столько клеток, сколько машина пройдёт за ход (pct — доля её хода)
void truncateToTurn (const cVehicle& v, const cMapView& view, std::vector<cPosition>& path, int pct)
{
	int budget = v.data.getSpeed() * pct / 100;
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

// правило шага 7 для одиночки: сближаться только до края зоны огня врага
void cutAtFireEdge (const cWorldModel& world, const cVehicle& v, std::vector<cPosition>& path)
{
	if (world.groundFire (v.getPosition()) > 0) return; // уже под огнём — не держать
	size_t cut = 0;
	while (cut < path.size() && world.groundFire (path[cut]) == 0) ++cut;
	path.resize (cut);
}

// конец пути не на клетке, занятой другим приказом этого хода
void avoidReservedEnd (std::vector<cPosition>& path, const std::vector<cPosition>& reserved)
{
	while (!path.empty() && containsPos (reserved, path.back()))
		path.pop_back();
}

std::vector<cPosition> enginePath (const cVehicle& v, const cMapView& view, const cPosition& dest, bool attack)
{
	cPathCalculator pc (v, view, dest, attack);
	std::vector<cPosition> res;
	for (const auto& p : pc.calcPath())
		res.push_back (p);
	return res;
}

// урон по броне armor за выстрел (мин. 1 — как cUnit::calcHealth)
int effDamage (int damage, int armor)
{
	return std::max (1, damage - armor);
}

// класс здания-цели: 0 шахта, 1 ключевое (фабрика, энергия, исследования), 2 прочее; −1 — не цель
int buildingClass (const cModel& model, const sEnemyMemory& m)
{
	const sID id (m.typeFirst, m.typeSecond);
	const auto& ud = *model.getUnitsData();
	if (!ud.isValidId (id)) return -1;
	const auto& sd = ud.getStaticUnitData (id);
	if (sd.buildingData.explodesOnContact) return -1;                // мины
	if (ud.getDynamicUnitData (id).getBuildCost() <= 2) return -1; // соединители, дороги, мосты, блоки
	if (sd.buildingData.canMineMaxRes > 0) return 0;
	if (!sd.canBuild.empty() || sd.produceEnergy > 0 || sd.buildingData.canResearch) return 1;
	return 2;
}

constexpr int kClassPenalty[5] = {0, 12, 30, 20, 40}; // к расстоянию до цели, клеток

// шаг 9
constexpr int kRearPenalty = 30;   // поддержка в строю: за клетку впереди центра (ближе к цели)
// опасность клетки поддержки (findStandoff): 0 — вне «удара с подъезда» врага, 1 — в нём, но вне зоны огня
constexpr int kAssaultDanger = 1;  // штурмовое орудие бьёт ненамного дальше танка — ему можно стоять в «ударе»
constexpr int kSupportDanger = 0;  // артиллерии и ракетам — нет
constexpr int kSupportSense = 6;   // поддержка в бою: цели — у группы (kSense + это) или у цели операции

// кассетный удар задевает клетки ближе 2 (по сторонам) к цели — и свои машины, и здания
bool ownNear (const cModel& model, const cPlayer* owner, const cPosition& p)
{
	const cMap& map = *model.getMap();
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
		{
			if (std::abs (dx) + std::abs (dy) > 2) continue;
			const cPosition c = p + cPosition (dx, dy);
			if (!map.isValidPosition (c)) continue;
			const auto& field = map.getField (c);
			if (const cVehicle* u = field.getVehicle(); u != nullptr && u->getOwner() == owner) return true;
			for (const cBuilding* b : field.getBuildings())
				if (b->getOwner() == owner) return true;
		}
	return false;
}

// квадрат расстояния от клетки до юнита из памяти (здание 2×2 — до ближней клетки)
long long distToMemory (const cPosition& c, const sEnemyMemory& m)
{
	long long best = dist2 (c, m.pos);
	if (m.big)
		for (int dy = 0; dy <= 1; ++dy)
			for (int dx = 0; dx <= 1; ++dx)
				best = std::min (best, dist2 (c, m.pos + cPosition (dx, dy)));
	return best;
}

// лучшая цель выстрела с клетки c: в дальности, вооружённые раньше, затем меньше hp, затем id
const sEnemyMemory* bestShotFrom (const cModel& model, const cVehicle& v, const cPosition& c,
	const std::vector<const sEnemyMemory*>& targets)
{
	const long long r2 = static_cast<long long> (v.data.getRange()) * v.data.getRange();
	const bool splash = v.getStaticUnitData().muzzleType == eMuzzleType::RocketCluster;
	const sEnemyMemory* best = nullptr;
	for (const sEnemyMemory* m : targets)
	{
		if (distToMemory (c, *m) > r2) continue;
		if (splash && ownNear (model, v.getOwner(), m->pos)) continue;
		if (best == nullptr)
		{
			best = m;
			continue;
		}
		const bool am = m->armed(), ab = best->armed();
		if (am != ab)
		{
			if (am) best = m;
			continue;
		}
		if (m->hp != best->hp)
		{
			if (m->hp < best->hp) best = m;
			continue;
		}
		if (m->id < best->id) best = m;
	}
	return best;
}

} // namespace

//------------------------------------------------------------------------------
int shotsAfterMove (const cVehicle& v, int cost)
{
	const int shots = std::min (v.data.getShots(), v.data.getAmmo());
	if (shots <= 0) return 0;
	if (v.getStaticUnitData().vehicleData.canDriveAndFire) return shots;
	const int left = v.data.getSpeed() - cost;
	if (left <= 0 || v.data.getSpeedMax() <= 0) return 0;
	return std::min (shots, left * v.data.getShotsMax() / v.data.getSpeedMax());
}

//------------------------------------------------------------------------------
int supportMaxDanger (eRole r)
{
	return r == eRole::Assault ? kAssaultDanger : kSupportDanger;
}

//------------------------------------------------------------------------------
std::vector<const sEnemyMemory*> attackableTargets (const cModel& model, const cWorldModel& world, const cVehicle& v)
{
	std::vector<const sEnemyMemory*> res;
	const auto& sd = v.getStaticUnitData();
	const auto& ud = *model.getUnitsData();
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.visible) continue;
		const int need = m.plane ? eTerrainFlag::Air : (m.ship ? eTerrainFlag::Sea : eTerrainFlag::Ground);
		if ((sd.canAttack & need) == 0) continue;
		if (m.building)
		{
			const sID id (m.typeFirst, m.typeSecond);
			if (!ud.isValidId (id)) continue;
			if (ud.getStaticUnitData (id).buildingData.explodesOnContact) continue;
			if (ud.getDynamicUnitData (id).getBuildCost() <= 2) continue; // соединители, дороги, площадки — не цель
		}
		res.push_back (&m);
	}
	return res;
}

//------------------------------------------------------------------------------
unsigned int endMoveTarget (const cModel& model, const cVehicle& v, const std::vector<cPosition>& path,
	const std::vector<const sEnemyMemory*>& targets)
{
	if (path.empty() || targets.empty()) return kNoUnit;
	int cost = 0;
	cPosition prev = v.getPosition();
	for (const auto& p : path)
	{
		cost += cPathCalculator::calcNextCost (prev, p, &v, model.getMap().get());
		prev = p;
	}
	if (shotsAfterMove (v, cost) < 1) return kNoUnit;
	const sEnemyMemory* t = bestShotFrom (model, v, path.back(), targets);
	return t != nullptr ? t->id : kNoUnit;
}

//------------------------------------------------------------------------------
sStandoff findStandoff (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& v,
	const std::vector<const sEnemyMemory*>& targets, int ring, bool fire, int maxDanger, const std::vector<cPosition>& reserved)
{
	sStandoff res;
	const cPosition start = v.getPosition();
	res.cell = start;
	const int budget = std::max (0, v.data.getSpeed());
	// окно хода: дорога вдвое дешевле суши (шаг 2) — не дальше budget / 2 клеток
	const int B = budget / 2 + 1;
	const int side = 2 * B + 1;
	auto inWin = [&] (const cPosition& p) { return std::abs (p.x() - start.x()) <= B && std::abs (p.y() - start.y()) <= B; };
	auto li = [&] (const cPosition& p) { return static_cast<size_t> (p.y() - start.y() + B) * side + (p.x() - start.x() + B); };
	constexpr int kInf = std::numeric_limits<int>::max();
	std::vector<int> g (static_cast<size_t> (side) * side, kInf);
	std::vector<int> prev (static_cast<size_t> (side) * side, -1);
	using tNode = std::pair<int, int>;
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	g[li (start)] = 0;
	open.push ({0, static_cast<int> (li (start))});
	while (!open.empty())
	{
		const auto [d, cur] = open.top();
		open.pop();
		if (d != g[cur]) continue;
		const cPosition p (start.x() - B + cur % side, start.y() - B + cur / side);
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (!inWin (n) || !view.isValidPosition (n) || !view.possiblePlace (v, n)) continue;
				const int nd = d + cPathCalculator::calcNextCost (p, n, &v, &view);
				if (nd > budget) continue;
				const size_t ni = li (n);
				if (nd < g[ni])
				{
					g[ni] = nd;
					prev[ni] = cur;
					open.push ({nd, static_cast<int> (ni)});
				}
			}
	}
	// цели — только те, до которых можно дотянуться из окна
	std::vector<const sEnemyMemory*> near;
	const long long reach2 = static_cast<long long> (B + ring + 3) * (B + ring + 3);
	for (const sEnemyMemory* m : targets)
		if (dist2 (m->pos, start) <= reach2) near.push_back (m);
	const long long ring2 = static_cast<long long> (ring) * ring;
	const long long range2 = static_cast<long long> (v.data.getRange()) * v.data.getRange();
	const int shotsNow = std::min (v.data.getShots(), v.data.getAmmo());
	using tKey = std::tuple<int, int, int, long long, int, int, size_t>;
	tKey bestKey {std::numeric_limits<int>::max(), 0, 0, 0, 0, 0, 0};
	size_t bestI = li (start);
	unsigned int bestShot = kNoUnit;
	bool bestIn = false;
	int bestDanger = 0;
	for (int y = -B; y <= B; ++y)
		for (int x = -B; x <= B; ++x)
		{
			const cPosition c = start + cPosition (x, y);
			const size_t i = li (c);
			if (g[i] == kInf) continue;
			if (c != start && containsPos (reserved, c)) continue;
			const int danger = world.groundFire (c) > 0 ? 2 : (world.groundStrike (c) > 0 ? 1 : 0);
			long long minD2 = std::numeric_limits<long long>::max();
			for (const sEnemyMemory* m : near)
				minD2 = std::min (minD2, distToMemory (c, *m));
			const bool inRing = minD2 <= ring2;
			unsigned int shot = kNoUnit;
			bool fireNow = false;
			if (fire && minD2 <= range2)
			{
				if (c == start)
					fireNow = shotsNow > 0;
				else if (shotsAfterMove (v, g[i]) >= 1)
					if (const sEnemyMemory* t = bestShotFrom (model, v, c, near))
					{
						shot = t->id;
						fireNow = true;
					}
			}
			const long long gap = inRing ? 0 : (minD2 == std::numeric_limits<long long>::max() ? std::numeric_limits<long long>::max() / 4 : minD2 - ring2);
			const tKey key {std::max (0, danger - maxDanger), fireNow ? 0 : 1, inRing ? 0 : 1, gap, danger, g[i], i};
			if (key < bestKey)
			{
				bestKey = key;
				bestI = i;
				bestShot = shot;
				bestIn = inRing;
				bestDanger = danger;
			}
		}
	res.cell = cPosition (start.x() - B + static_cast<int> (bestI % side), start.y() - B + static_cast<int> (bestI / side));
	res.danger = bestDanger;
	res.inRange = bestIn;
	if (res.cell != start)
	{
		for (int c = static_cast<int> (bestI); c >= 0 && c != static_cast<int> (li (start)); c = prev[c])
			res.path.emplace_back (start.x() - B + c % side, start.y() - B + c / side);
		std::reverse (res.path.begin(), res.path.end());
		res.attackId = bestShot;
	}
	return res;
}

//------------------------------------------------------------------------------
eRole cArmyOps::roleOfId (unsigned int id) const
{
	const auto it = roleById_.find (id);
	return it == roleById_.end() ? eRole::None : static_cast<eRole> (it->second);
}

//------------------------------------------------------------------------------
bool cArmyOps::isSupport (unsigned int id) const
{
	return isSupportRole (roleOfId (id));
}

//------------------------------------------------------------------------------
cPosition cArmyOps::escortAnchor (bool* withArmy) const
{
	if (withArmy) *withArmy = false;
	for (const auto& op : ops_)
	{
		if (op.members.empty() || (op.state != eOpState::Advance && op.state != eOpState::Attack && op.state != eOpState::Search)) continue;
		if (const auto it = fighterById_.find (op.leader); it != fighterById_.end())
		{
			if (withArmy) *withArmy = true;
			return it->second->getPosition();
		}
	}
	return rallyPoint();
}

//------------------------------------------------------------------------------
cPosition cArmyOps::rallyPoint() const
{
	for (const auto& op : ops_)
		if ((op.state == eOpState::Wait || op.state == eOpState::Gather) && op.rally.x() >= 0) return op.rally;
	return home_;
}

//------------------------------------------------------------------------------
const char* opStateName (eOpState s)
{
	switch (s)
	{
		case eOpState::Wait: return "wait";
		case eOpState::Gather: return "gather";
		case eOpState::Advance: return "advance";
		case eOpState::Search: return "search";
		case eOpState::Attack: return "attack";
		case eOpState::Retreat: return "retreat";
	}
	return "?";
}

//------------------------------------------------------------------------------
nlohmann::json sOperation::toJson() const
{
	nlohmann::json j;
	j["op"] = id;
	j["state"] = opStateName (state);
	j["since"] = since;
	j["created"] = created;
	j["target"] = {{"kind", targetKindName (targetKind)}, {"id", targetId}, {"class", targetClass}, {"x", target.x()}, {"y", target.y()}};
	j["rally"] = posJson (rally);
	j["leader"] = leader;
	j["members"] = members.size();
	j["force"] = force;
	j["need"] = need;
	j["launchMembers"] = launchMembers;
	j["launchForce"] = launchForce;
	return j;
}

//------------------------------------------------------------------------------
int cArmyOps::sForce::value() const
{
	return static_cast<int> (isqrtll (dmg * hp));
}

//------------------------------------------------------------------------------
void cArmyOps::setDirective (const sArmyDirective& d)
{
	directive_ = d;
	nlohmann::json e;
	e["event"] = "operation";
	e["level"] = "operational";
	e["what"] = "directive";
	e["kind"] = d.kind == sArmyDirective::eKind::Target ? "target" : d.kind == sArmyDirective::eKind::Gather ? "gather" : d.kind == sArmyDirective::eKind::Retreat ? "retreat" : "none";
	e["pos"] = posJson (d.pos);
	events_.push_back (std::move (e));
}

//------------------------------------------------------------------------------
std::vector<nlohmann::json> cArmyOps::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

//------------------------------------------------------------------------------
nlohmann::json cArmyOps::toJson() const
{
	nlohmann::json j;
	nlohmann::json ops = nlohmann::json::array();
	for (const auto& op : ops_)
		ops.push_back (op.toJson());
	j["ops"] = std::move (ops);
	j["scouts"] = scouts_;
	j["spotters"] = spotters_;
	j["reserve"] = reserve_;
	j["cooldownUntil"] = cooldownUntil_;
	j["enemyArmy"] = {{"count", totalDef_.count}, {"force", needOf (totalDef_)}};
	return j;
}

//------------------------------------------------------------------------------
void cArmyOps::event (const char* what, const sOperation& op, nlohmann::json extra)
{
	nlohmann::json e = op.toJson();
	e["event"] = "operation";
	e["level"] = "operational";
	e["what"] = what;
	if (extra.is_object())
		for (auto& [k, v] : extra.items())
			e[k] = v;
	events_.push_back (std::move (e));
}

//------------------------------------------------------------------------------
void cArmyOps::setState (sOperation& op, eOpState s, int turn, const char* why)
{
	if (op.state == s) return;
	const eOpState from = op.state;
	op.state = s;
	op.since = turn;
	event ("state", op, {{"from", opStateName (from)}, {"why", why}});
}

//------------------------------------------------------------------------------
bool cArmyOps::committed (unsigned int id) const
{
	for (const auto& op : ops_)
		if (op.state != eOpState::Wait && op.state != eOpState::Gather && contains (op.members, id)) return true;
	return false;
}

//------------------------------------------------------------------------------
sOperation* cArmyOps::forming()
{
	for (auto& op : ops_)
		if (op.state == eOpState::Wait || op.state == eOpState::Gather) return &op;
	return nullptr;
}

//------------------------------------------------------------------------------
void cArmyOps::buildLandDist (const sContext& ctx)
{
	const cMap& map = *ctx.model->getMap();
	W_ = map.getSize().x();
	H_ = map.getSize().y();
	const size_t N = static_cast<size_t> (W_) * H_;
	landDist_.assign (N, -1);
	landPrev_.assign (N, -1);
	home_ = ctx.player->getLandingPos();
	// старт — ближайшая к высадке проходимая клетка
	cPosition start (-1, -1);
	for (int r = 0; r <= 4 && start.x() < 0; ++r)
		for (int dy = -r; dy <= r && start.x() < 0; ++dy)
			for (int dx = -r; dx <= r && start.x() < 0; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
				const cPosition p = home_ + cPosition (dx, dy);
				if (tankTerrain (map, p)) start = p;
			}
	if (start.x() < 0) return;
	auto off = [this] (const cPosition& p) { return static_cast<size_t> (p.y()) * W_ + p.x(); };
	using tNode = std::pair<int, int>;
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	landDist_[off (start)] = 0;
	open.push ({0, static_cast<int> (off (start))});
	while (!open.empty())
	{
		const auto [d, cur] = open.top();
		open.pop();
		if (d != landDist_[cur]) continue;
		const cPosition p (cur % W_, cur / W_);
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (!tankTerrain (map, n)) continue;
				const int nd = d + ((dx != 0 && dy != 0) ? 3 : 2);
				const size_t no = off (n);
				if (landDist_[no] < 0 || nd < landDist_[no])
				{
					landDist_[no] = nd;
					landPrev_[no] = cur;
					open.push ({nd, static_cast<int> (no)});
				}
			}
	}
}

//------------------------------------------------------------------------------
int cArmyOps::landDist (const cPosition& p) const
{
	if (p.x() < 0 || p.y() < 0 || p.x() >= W_ || p.y() >= H_ || landDist_.empty()) return -1;
	return landDist_[static_cast<size_t> (p.y()) * W_ + p.x()];
}

//------------------------------------------------------------------------------
// до юнита (здание big — 2×2) можно дойти: соседняя клетка достижима по суше
bool cArmyOps::reachableNear (const cPosition& p, bool big, int* dist) const
{
	const int s = big ? 2 : 1;
	int best = -1;
	for (int dy = -1; dy <= s; ++dy)
		for (int dx = -1; dx <= s; ++dx)
		{
			const int d = landDist (p + cPosition (dx, dy));
			if (d >= 0 && (best < 0 || d < best)) best = d;
		}
	if (dist) *dist = best;
	return best >= 0;
}

//------------------------------------------------------------------------------
// оборона у клетки: вооружённые (по земле) враги из памяти ближе radius, здания — в своей дальности + 2
cArmyOps::sForce cArmyOps::defenseAt (const sContext& ctx, const cPosition& p, int radius) const
{
	sForce f;
	for (const auto& kv : ctx.world->enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.hitsGround()) continue;
		const int r = m.building ? m.range + 2 : radius;
		if (dist2 (m.pos, p) > static_cast<long long> (r) * r) continue;
		f.dmg += m.damageVs (ourArmor_);
		f.hp += m.hp;
		f.armor = std::max (f.armor, m.armor);
		++f.count;
	}
	return f;
}

//------------------------------------------------------------------------------
cArmyOps::sForce cArmyOps::forceOf (const sContext&, const std::vector<unsigned int>& ids, int enemyArmor) const
{
	sForce f;
	for (unsigned int id : ids)
	{
		const auto it = fighterById_.find (id);
		if (it == fighterById_.end()) continue;
		const cVehicle& v = *it->second;
		// шаг 9: без боезапаса не стреляет (у ракет он на 6 выстрелов) — в силу не входит
		f.dmg += static_cast<long long> (effDamage (v.data.getDamage(), enemyArmor)) * std::min (v.data.getShotsMax(), v.data.getAmmo());
		f.hp += v.data.getHitpoints();
		f.armor = std::max (f.armor, v.data.getArmor());
		++f.count;
	}
	return f;
}

//------------------------------------------------------------------------------
int cArmyOps::needOf (const sForce& def) const
{
	if (def.count == 0) return 0;
	return static_cast<int> (isqrtll (def.dmg * def.hp * kMarginPct / 100));
}

//------------------------------------------------------------------------------
// цель ещё есть: здание — в памяти; армия — машина цели в памяти (клетка — последняя известная)
bool cArmyOps::targetValid (const sContext& ctx, sOperation& op) const
{
	switch (op.targetKind)
	{
		case eOpTarget::Building:
		case eOpTarget::Army:
		{
			const auto& mem = ctx.world->enemies();
			const auto it = mem.find (op.targetId);
			if (it == mem.end()) return false;
			op.target = it->second.pos;
			return true;
		}
		case eOpTarget::Point:
			return op.target.x() >= 0;
		default:
			return false;
	}
}

//------------------------------------------------------------------------------
// сила и нужное на цель: защитники у цели и (в походе) враги у группы
void cArmyOps::evaluate (const sContext& ctx, sOperation& op, bool withTotal)
{
	sForce def;
	if (op.target.x() >= 0)
	{
		// защитники у цели и у лидера (без двойного счёта: объединение по id); только у
		// группы в походе — хуже (Donuts, отчёт шага 8 §4)
			cPosition lp (-1, -1);
		if (op.state != eOpState::Wait && op.state != eOpState::Gather)
			if (const auto it = fighterById_.find (op.leader); it != fighterById_.end()) lp = it->second->getPosition();
		for (const auto& kv : ctx.world->enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (!m.hitsGround()) continue;
			const int r = m.building ? m.range + 2 : kDefRadius;
			bool near = dist2 (m.pos, op.target) <= static_cast<long long> (r) * r;
			if (!near && lp.x() >= 0)
			{
				const int rs = m.building ? m.range + 2 : kSense;
				near = dist2 (m.pos, lp) <= static_cast<long long> (rs) * rs;
			}
			if (!near) continue;
			def.dmg += m.damageVs (ourArmor_);
			def.hp += m.hp;
			def.armor = std::max (def.armor, m.armor);
			++def.count;
		}
	}
	int armor = def.count > 0 ? def.armor : ourArmor_;
	if (def.count == 0 && op.targetKind == eOpTarget::Building)
		if (const auto it = ctx.world->enemies().find (op.targetId); it != ctx.world->enemies().end()) armor = it->second.armor;
	op.force = forceOf (ctx, op.members, armor).value();
	op.need = needOf (def);
	if (withTotal && op.target.x() >= 0) op.need = std::max (op.need, needOf (totalDef_) * kTotalPct / 100);
}

//------------------------------------------------------------------------------
// Лучшая цель из памяти: шахты, ключевые здания, прочие здания, иначе скопление
// армии врага. Оценка — расстояние по суше от базы + штраф класса; сначала те,
// на которые сил хватает (feasibleOnly — только такие). near — искать только у
// клетки (Search). extra — сила, которая уже идёт на ту же цель (подмога).
bool cArmyOps::chooseTarget (const sContext& ctx, sOperation& op, const sForce* extra, bool feasibleOnly,
	const cPosition* near, int nearRadius)
{
	struct sCand
	{
		eOpTarget kind;
		unsigned int id;
		cPosition pos;
		int cls;
		long long score;
		bool feasible;
	};
	std::vector<sCand> cands;
	auto feasibleAt = [&] (const cPosition& p, int armorFallback) {
		const sForce def = defenseAt (ctx, p, kDefRadius);
		sForce own = forceOf (ctx, op.members, def.count > 0 ? def.armor : armorFallback);
		if (extra)
		{
			own.dmg += extra->dmg;
			own.hp += extra->hp;
			own.count += extra->count;
		}
		return own.count > 0 && own.value() >= std::max (needOf (def), needOf (totalDef_) * kTotalPct / 100);
	};
	for (const auto& kv : ctx.world->enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.building) continue;
		if (near && dist2 (m.pos, *near) > static_cast<long long> (nearRadius) * nearRadius) continue;
		const int cls = buildingClass (*ctx.model, m);
		if (cls < 0) continue;
		int d = 0;
		if (!reachableNear (m.pos, m.big, &d)) continue;
		const long long score = (near ? dist2 (m.pos, *near) / 4 : d / 2) + kClassPenalty[cls];
		cands.push_back ({eOpTarget::Building, m.id, m.pos, cls, score, feasibleAt (m.pos, m.armor)});
	}
	if (cands.empty())
	{
		// скопления вооружённых машин врага (ближе 8 клеток друг к другу), берём самое близкое
		std::vector<const sEnemyMemory*> armed;
		for (const auto& kv : ctx.world->enemies())
			if (!kv.second.building && kv.second.hitsGround() && !kv.second.plane && !kv.second.ship) armed.push_back (&kv.second);
		std::vector<char> used (armed.size(), 0);
		for (size_t i = 0; i < armed.size(); ++i)
		{
			if (used[i]) continue;
			std::vector<size_t> cl {i};
			used[i] = 1;
			for (size_t q = 0; q < cl.size(); ++q)
				for (size_t j = 0; j < armed.size(); ++j)
					if (!used[j] && dist2 (armed[cl[q]]->pos, armed[j]->pos) <= 64)
					{
						used[j] = 1;
						cl.push_back (j);
					}
			// представитель — ближайший к центру скопления
			long long sx = 0, sy = 0;
			for (size_t c : cl)
			{
				sx += armed[c]->pos.x();
				sy += armed[c]->pos.y();
			}
			const cPosition centre (static_cast<int> (sx / static_cast<long long> (cl.size())), static_cast<int> (sy / static_cast<long long> (cl.size())));
			const sEnemyMemory* rep = armed[cl.front()];
			for (size_t c : cl)
				if (dist2 (armed[c]->pos, centre) < dist2 (rep->pos, centre)) rep = armed[c];
			if (near && dist2 (rep->pos, *near) > static_cast<long long> (nearRadius) * nearRadius) continue;
			if (dist2 (rep->pos, home_) > static_cast<long long> (kArmyHome) * kArmyHome) continue; // армию врага — только у себя
			int d = 0;
			if (!reachableNear (rep->pos, false, &d)) continue;
			const long long score = (near ? dist2 (rep->pos, *near) / 4 : d / 2) + kClassPenalty[3];
			cands.push_back ({eOpTarget::Army, rep->id, rep->pos, 3, score, feasibleAt (rep->pos, rep->armor)});
		}
	}
	// a лучше b: сначала те, на которые сил хватает, затем меньшая оценка, затем меньший id
	auto better = [] (const sCand& a, const sCand& b) {
		if (a.feasible != b.feasible) return a.feasible;
		if (a.score != b.score) return a.score < b.score;
		return a.id < b.id;
	};
	const sCand* best = nullptr;
	const sCand* current = nullptr;
	for (const auto& c : cands)
	{
		if (feasibleOnly && !c.feasible) continue;
		if (c.kind == op.targetKind && c.id == op.targetId) current = &c;
		if (best == nullptr || better (c, *best)) best = &c;
	}
	if (best == nullptr) return false;
	// прежняя цель остаётся, пока новая не лучше заметно (цель не прыгает каждый ход)
	if (current != nullptr && current != best && current->feasible == best->feasible && current->score <= best->score + 5)
		best = current;
	const bool changed = op.targetKind != best->kind || op.targetId != best->id;
	op.targetKind = best->kind;
	op.targetId = best->id;
	op.target = best->pos;
	op.targetClass = best->cls;
	if (changed) event ("target", op, {{"feasible", best->feasible}});
	return true;
}

//------------------------------------------------------------------------------
// Точка сбора: на пути по суше от базы к цели — первая клетка не ближе rallyGap к
// своим зданиям (край базы), не дальше rallyMax клеток пути и вне угроз
// («дальность + ход»; если под угрозой уже сама база — вне зоны огня).
cPosition cArmyOps::computeRally (const sContext& ctx, const cPosition& destIn) const
{
	if (directive_.kind == sArmyDirective::eKind::Gather && directive_.pos.x() >= 0) return directive_.pos;
	if (landDist_.empty()) return home_;
	cPosition dest = destIn;
	if (dest.x() < 0) dest = cPosition (W_ / 2, H_ / 2); // цели нет — к середине карты
	// ближайшая к dest достижимая клетка
	cPosition goal (-1, -1);
	for (int r = 0; r <= 12 && goal.x() < 0; ++r)
	{
		long long bestD = std::numeric_limits<long long>::max();
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
				const cPosition p = dest + cPosition (dx, dy);
				if (landDist (p) < 0) continue;
				const long long d = dist2 (p, dest);
				if (d < bestD)
				{
					bestD = d;
					goal = p;
				}
			}
	}
	if (goal.x() < 0) return home_;
	std::vector<cPosition> chain; // от goal к базе
	for (int c = static_cast<int> (static_cast<size_t> (goal.y()) * W_ + goal.x()); c >= 0; c = landPrev_[c])
		chain.emplace_back (c % W_, c / W_);
	std::reverse (chain.begin(), chain.end()); // от базы
	// свои здания (без соединителей и дорог)
	std::vector<std::pair<cPosition, bool>> own;
	for (const auto& b : ctx.player->getBuildings())
	{
		if (b->getStaticUnitData().surfacePosition != eSurfacePosition::Ground) continue;
		own.emplace_back (b->getPosition(), b->getIsBig());
	}
	auto gapTo = [&] (const cPosition& c) {
		int g = std::numeric_limits<int>::max();
		for (const auto& [q, big] : own)
		{
			const int s = big ? 2 : 1;
			const int dx = c.x() < q.x() ? q.x() - c.x() : (c.x() >= q.x() + s ? c.x() - q.x() - s + 1 : 0);
			const int dy = c.y() < q.y() ? q.y() - c.y() : (c.y() >= q.y() + s ? c.y() - q.y() - s + 1 : 0);
			g = std::min (g, std::max (dx, dy));
		}
		return g;
	};
	const bool homeThreat = ctx.world->groundThreat (chain.front()) > 0;
	auto threatened = [&] (const cPosition& c) { return homeThreat ? ctx.world->groundFire (c) > 0 : ctx.world->groundThreat (c) > 0; };
	cPosition rally = chain.front();
	for (size_t i = 0; i < chain.size() && static_cast<int> (i) <= kRallyMax; ++i)
	{
		const cPosition& c = chain[i];
		if (threatened (c)) break;
		rally = c;
		if (gapTo (c) >= kRallyGap) break;
	}
	return rally;
}

//------------------------------------------------------------------------------
unsigned int cArmyOps::chooseLeader (const sContext&, const sOperation& op) const
{
	// прежний лидер — если жив, в группе и цел
	auto healthy = [] (const cVehicle& v) {
		return v.data.getHitpointsMax() > 0 && v.data.getHitpoints() * 100 >= v.data.getHitpointsMax() * kHealthyPct;
	};
	// шаг 9: лидер — машина строя (танк), не поддержка: та идёт в хвосте
	const bool anyLine = std::any_of (op.members.begin(), op.members.end(), [this] (unsigned int id) {
		return fighterById_.count (id) != 0 && !isSupport (id);
	});
	if (const auto it = fighterById_.find (op.leader); it != fighterById_.end() && contains (op.members, op.leader) && healthy (*it->second)
		&& (!anyLine || !isSupport (op.leader)))
		return op.leader;
	const cVehicle* best = nullptr;
	for (unsigned int id : op.members)
	{
		const auto it = fighterById_.find (id);
		if (it == fighterById_.end()) continue;
		const cVehicle* v = it->second;
		if (best == nullptr)
		{
			best = v;
			continue;
		}
		const bool lv = !isSupport (v->getId()), lb = !isSupport (best->getId());
		if (lv != lb)
		{
			if (lv) best = v;
			continue;
		}
		const bool hv = healthy (*v), hb = healthy (*best);
		if (hv != hb)
		{
			if (hv) best = v;
			continue;
		}
		const int sv = v->data.getSpeedMax(), sb = best->data.getSpeedMax();
		if (sv != sb)
		{
			if (sv < sb) best = v; // самый медленный
			continue;
		}
		const long long dv = dist2 (v->getPosition(), op.target), db = dist2 (best->getPosition(), op.target);
		if (dv < db || (dv == db && v->getId() < best->getId())) best = v; // ближе к цели
	}
	return best ? best->getId() : 0;
}

//------------------------------------------------------------------------------
// роли вне операций: резерв обороны у базы (ближайшие к ней), разведчики (самые дальние)
void cArmyOps::updateRoles (const sContext& ctx, std::vector<unsigned int>& pool)
{
	const sOperation* f = forming();
	bool known = (f && f->target.x() >= 0);
	for (const auto& op : ops_)
		if (op.state != eOpState::Wait && op.state != eOpState::Gather) known = true;
	const int wantScouts = known ? kScoutsKnown : kScouts;
	const int wantReserve = static_cast<int> (pool.size()) >= kReserve + kMinLaunch ? kReserve : 0;

	auto keep = [&] (std::vector<unsigned int>& role, int want) {
		std::vector<unsigned int> kept;
		for (unsigned int id : role)
			if (contains (pool, id) && static_cast<int> (kept.size()) < want) kept.push_back (id);
		role.swap (kept);
	};
	keep (reserve_, wantReserve);
	// шаг 9: есть разведчики малой фабрики — разведка ими, танки в разведку не идут;
	// разведчики сверх нужных — корректировщики при армии
	const bool scoutVehicles = !scoutById_.empty();
	spotters_.clear();
	if (scoutVehicles)
	{
		std::vector<unsigned int> kept;
		for (unsigned int id : scouts_)
			if (scoutById_.count (id) && static_cast<int> (kept.size()) < wantScouts) kept.push_back (id);
		for (const auto& kv : scoutById_)
			if (static_cast<int> (kept.size()) < wantScouts && !contains (kept, kv.first)) kept.push_back (kv.first);
		scouts_.swap (kept);
		for (const auto& kv : scoutById_)
			if (!contains (scouts_, kv.first)) spotters_.push_back (kv.first);
	}
	else
		keep (scouts_, wantScouts);
	auto take = [&] (std::vector<unsigned int>& role, int want, bool nearest, bool lineFirst) {
		while (static_cast<int> (role.size()) < want)
		{
			unsigned int best = 0;
			int bestD = 0;
			bool bestLine = false;
			bool any = false;
			for (unsigned int id : pool)
			{
				if (contains (reserve_, id) || contains (scouts_, id)) continue;
				const auto it = fighterById_.find (id);
				if (it == fighterById_.end()) continue;
				int d = landDist (it->second->getPosition());
				if (d < 0) d = 1 << 20;
				const bool line = !isSupport (id); // резерв и разведка — строем (танками), поддержка — в армию
				if (!any || (lineFirst && line != bestLine ? line : (nearest ? d < bestD : d > bestD)))
				{
					any = true;
					best = id;
					bestD = d;
					bestLine = line;
				}
			}
			if (!any) break;
			role.push_back (best);
		}
	};
	take (reserve_, wantReserve, true, true);
	// разведчики — только если в группе останется кто-то (первая машина — сразу в разведку)
	if (!scoutVehicles)
		take (scouts_, std::min (wantScouts, std::max (0, static_cast<int> (pool.size()) - static_cast<int> (reserve_.size()))), false, true);
	std::vector<unsigned int> rest;
	for (unsigned int id : pool)
		if (!contains (reserve_, id) && !contains (scouts_, id)) rest.push_back (id);
	pool.swap (rest);
}

//------------------------------------------------------------------------------
// отход по силе — в журнал: сколько машин группы потеряно с выхода
void cArmyOps::noteRetreat (const sOperation& op)
{
	event ("retreat", op, {{"lost", op.launchMembers - static_cast<int> (op.members.size())}});
}

//------------------------------------------------------------------------------
void cArmyOps::refresh (const sContext& ctx)
{
	const int turn = ctx.turn;
	fighterById_.clear();
	roleById_.clear();
	scoutById_.clear();
	for (const cVehicle* v : ctx.fighters)
	{
		fighterById_[v->getId()] = v;
		if (ctx.roles) roleById_[v->getId()] = static_cast<int> (ctx.roles->roleOf (*v));
	}
	for (const cVehicle* v : ctx.scoutUnits)
	{
		scoutById_[v->getId()] = v;
		roleById_[v->getId()] = static_cast<int> (eRole::Scout);
	}
	{
		const auto* tank = ctx.player->getLastUnitData (kIdTank);
		ourArmor_ = tank ? tank->getArmor() : 0;
	}
	buildLandDist (ctx);
	// у своих зданий (кроме соединителей и дорог) и у идущей стройки строю не стоять: выезд фабрик, площадки
	avoid_.assign (static_cast<size_t> (W_) * H_, 0);
	{
		auto mark = [this] (const cPosition& q, int s, int ring) {
			for (int dy = -ring; dy < s + ring; ++dy)
				for (int dx = -ring; dx < s + ring; ++dx)
				{
					const cPosition c = q + cPosition (dx, dy);
					if (c.x() < 0 || c.y() < 0 || c.x() >= W_ || c.y() >= H_) continue;
					avoid_[static_cast<size_t> (c.y()) * W_ + c.x()] = 1;
				}
		};
		for (const auto& b : ctx.player->getBuildings())
			if (b->getStaticUnitData().surfacePosition == eSurfacePosition::Ground) mark (b->getPosition(), b->getIsBig() ? 2 : 1, 1);
		for (const auto& v : ctx.player->getVehicles())
			if (v->isUnitBuildingABuilding()) mark (v->getPosition(), v->getIsBig() ? 2 : 1, 1);
	}
	totalDef_ = {};
	for (const auto& kv : ctx.world->enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (m.building || !m.hitsGround() || m.plane || m.ship) continue;
		totalDef_.dmg += m.damageVs (ourArmor_);
		totalDef_.hp += m.hp;
		totalDef_.armor = std::max (totalDef_.armor, m.armor);
		++totalDef_.count;
	}
	// армия за туманом не исчезает: держим пик известной (тает на 1% за ход)
	{
		peakDmg_ = peakDmg_ * kPeakDecay / 1000.0;
		peakHp_ = peakHp_ * kPeakDecay / 1000.0;
		if (static_cast<double> (totalDef_.dmg) * totalDef_.hp >= peakDmg_ * peakHp_)
		{
			peakDmg_ = static_cast<double> (totalDef_.dmg);
			peakHp_ = static_cast<double> (totalDef_.hp);
		}
		else
		{
			totalDef_.dmg = static_cast<long long> (peakDmg_);
			totalDef_.hp = static_cast<long long> (peakHp_);
			if (totalDef_.count == 0) totalDef_.count = 1;
		}
	}

	// 1) погибшие — из состава
	for (auto& op : ops_)
	{
		std::vector<unsigned int> alive;
		for (unsigned int id : op.members)
			if (fighterById_.count (id)) alive.push_back (id);
		op.members.swap (alive);
	}

	// 2) операции в походе, бою и отходе — переходы
	const bool retreatAll = directive_.kind == sArmyDirective::eKind::Retreat;
	for (auto& op : ops_)
	{
		if (op.state == eOpState::Wait || op.state == eOpState::Gather) continue;
		if (static_cast<int> (op.members.size()) < std::max (2, kMinLaunch / 2) && op.state != eOpState::Retreat)
		{
			setState (op, eOpState::Retreat, turn, "too few members");
			continue;
		}
		op.leader = chooseLeader (ctx, op);
		const bool tv = targetValid (ctx, op);
		evaluate (ctx, op);
		const cVehicle* leader = fighterById_.count (op.leader) ? fighterById_.at (op.leader) : nullptr;
		const cPosition lp = leader ? leader->getPosition() : op.rally;
		bool armedNear = false;
		for (const auto& kv : ctx.world->enemies())
			if (kv.second.visible && kv.second.hitsGround() && dist2 (kv.second.pos, lp) <= static_cast<long long> (kSense) * kSense) armedNear = true;
		// подмога в походе отходит по своей силе: со счётом вместе с операцией, к которой
		// идёт, — хуже (Donuts, отчёт шага 8 §4)
		const bool weak = op.need > 0 && static_cast<long long> (op.force) * 100 < static_cast<long long> (op.need) * kRetreatPct;
		if (retreatAll && op.state != eOpState::Retreat)
		{
			setState (op, eOpState::Retreat, turn, "directive");
			continue;
		}
		switch (op.state)
		{
			case eOpState::Advance:
				if (!tv)
				{
					op.searchFrom = op.target;
					setState (op, eOpState::Search, turn, "target gone");
				}
				else if (weak)
				{
					noteRetreat (op);
					setState (op, eOpState::Retreat, turn, "force below need");
				}
				else if (dist2 (lp, op.target) <= static_cast<long long> (kEngage) * kEngage || armedNear)
					setState (op, eOpState::Attack, turn, armedNear ? "enemy near" : "target near");
				break;
			case eOpState::Attack:
				if (!tv)
				{
					op.searchFrom = op.target;
					setState (op, eOpState::Search, turn, "target gone");
				}
				else if (weak)
				{
					noteRetreat (op);
					setState (op, eOpState::Retreat, turn, "force below need");
				}
				else if (!armedNear && dist2 (lp, op.target) > static_cast<long long> (kEngage + 6) * (kEngage + 6))
					setState (op, eOpState::Advance, turn, "target moved away");
				break;
			case eOpState::Search:
			{
				const cPosition from = lp;
				if (chooseTarget (ctx, op, nullptr, true, &from, kSearchRadius))
				{
					evaluate (ctx, op);
					setState (op, dist2 (lp, op.target) <= static_cast<long long> (kEngage) * kEngage ? eOpState::Attack : eOpState::Advance, turn, "next target near");
				}
				else if (chooseTarget (ctx, op, nullptr, true, nullptr, 0))
				{
					evaluate (ctx, op);
					setState (op, eOpState::Advance, turn, "next target");
				}
				else if (turn - op.since >= kSearchTurns)
					setState (op, eOpState::Retreat, turn, "nothing found");
				else
				{
					// осмотреться: ближайшая невиданная достижимая клетка у места цели
					op.targetKind = eOpTarget::Point;
					op.targetId = 0;
					op.targetClass = 4;
					cPosition best (-1, -1);
					long long bestD = std::numeric_limits<long long>::max();
					const cPosition c0 = op.searchFrom.x() >= 0 ? op.searchFrom : lp;
					for (int dy = -kSearchRadius; dy <= kSearchRadius; ++dy)
						for (int dx = -kSearchRadius; dx <= kSearchRadius; ++dx)
						{
							const cPosition p = c0 + cPosition (dx, dy);
							if (landDist (p) < 0) continue;
							if (ctx.world->seenTurn (p) >= turn - 2) continue; // видели только что
							const long long d = dist2 (p, lp);
							if (d < bestD)
							{
								bestD = d;
								best = p;
							}
						}
					op.target = best.x() >= 0 ? best : c0;
				}
				break;
			}
			case eOpState::Retreat:
			{
				const int R = 3 + static_cast<int> (std::ceil (std::sqrt (static_cast<double> (std::max<size_t> (1, op.members.size())))));
				int nearRally = 0;
				for (unsigned int id : op.members)
					if (fighterById_.count (id) && cheb (fighterById_.at (id)->getPosition(), op.rally) <= R) ++nearRally;
				if (op.members.empty() || nearRally * 100 >= 70 * static_cast<int> (op.members.size()) || turn - op.since >= kRetreatTurns)
				{
					event ("dissolved", op, {{"why", op.members.empty() ? "no members" : "back at rally"}});
					cooldownUntil_ = turn + kCooldown;
					cooldownForce_ = op.launchForce;
					op.members.clear();
				}
				break;
			}
			default:
				break;
		}
	}
	if (retreatAll) directive_.kind = sArmyDirective::eKind::None; // отход — разово

	// слияние: подмога догнала операцию с той же целью
	auto marching = [] (eOpState s) { return s == eOpState::Advance || s == eOpState::Attack || s == eOpState::Search; };
	for (auto& a : ops_)
		for (auto& b : ops_)
		{
			if (&a == &b || a.id > b.id || a.members.empty() || b.members.empty()) continue;
			if (!marching (a.state) || !marching (b.state) || a.targetId != b.targetId || a.targetKind != b.targetKind) continue;
			const auto la = fighterById_.find (a.leader), lb = fighterById_.find (b.leader);
			if (la == fighterById_.end() || lb == fighterById_.end()) continue;
			if (cheb (la->second->getPosition(), lb->second->getPosition()) > kMergeDist) continue;
			event ("merged", b, {{"into", a.id}});
			for (unsigned int id : b.members)
				a.members.push_back (id);
			std::sort (a.members.begin(), a.members.end());
			a.launchMembers += static_cast<int> (b.members.size());
			b.members.clear();
		}
	// пустые операции (кроме формирующейся) — убрать
	ops_.erase (std::remove_if (ops_.begin(), ops_.end(), [] (const sOperation& op) {
		return op.members.empty() && op.state != eOpState::Wait && op.state != eOpState::Gather;
	}), ops_.end());

	// 3) пул: машины вне операций в походе — резерв, разведка и формирующаяся операция
	std::vector<unsigned int> pool;
	for (const cVehicle* v : ctx.fighters)
		if (!committed (v->getId())) pool.push_back (v->getId());
	sOperation* f = forming();
	if (f == nullptr)
	{
		sOperation op;
		op.id = nextId_++;
		op.state = eOpState::Wait;
		op.since = turn;
		op.created = turn;
		ops_.push_back (op);
		f = &ops_.back();
		event ("created", *f);
	}
	updateRoles (ctx, pool);
	f = forming();
	f->members = pool;
	std::sort (f->members.begin(), f->members.end());

	// 4) формирующаяся операция: цель, точка сбора, выход
	int committedCount = 0;
	sForce extra; // подмога: сила операции в походе на ту же цель
	const sOperation* active = nullptr;
	for (const auto& op : ops_)
		if (op.state != eOpState::Wait && op.state != eOpState::Gather)
		{
			++committedCount;
			if (active == nullptr && (op.state == eOpState::Advance || op.state == eOpState::Attack)) active = &op;
		}
	bool haveTarget = false;
	if (directive_.kind == sArmyDirective::eKind::Target && directive_.pos.x() >= 0)
	{
		const cPosition p = directive_.pos;
		haveTarget = chooseTarget (ctx, *f, nullptr, false, &p, 6);
		if (!haveTarget)
		{
			f->targetKind = eOpTarget::Point;
			f->targetId = 0;
			f->targetClass = 4;
			f->target = p;
			haveTarget = true;
		}
	}
	else if (active != nullptr)
	{
		// подмога: цель — та же, сила считается вместе с идущей операцией
		f->targetKind = active->targetKind;
		f->targetId = active->targetId;
		f->target = active->target;
		f->targetClass = active->targetClass;
		haveTarget = f->target.x() >= 0;
		for (unsigned int id : active->members)
			if (const auto it = fighterById_.find (id); it != fighterById_.end())
			{
				extra.dmg += static_cast<long long> (effDamage (it->second->data.getDamage(), ourArmor_)) * it->second->data.getShotsMax();
				extra.hp += it->second->data.getHitpoints();
				++extra.count;
			}
	}
	else
		haveTarget = chooseTarget (ctx, *f, nullptr, false, nullptr, 0);
	if (!haveTarget)
	{
		f->targetKind = eOpTarget::None;
		f->targetId = 0;
		f->targetClass = -1;
		f->target = cPosition (-1, -1);
	}
	evaluate (ctx, *f, true);
	int forceWith = f->force;
	if (extra.count > 0)
	{
		const sForce own = forceOf (ctx, f->members, ourArmor_);
		sForce sum = own;
		sum.dmg += extra.dmg;
		sum.hp += extra.hp;
		forceWith = sum.value();
	}
	{
		// точка сбора не скачет: сдвиг меньше 5 клеток — оставить прежнюю
		const cPosition r = computeRally (ctx, f->target);
		if (f->rally.x() < 0 || cheb (r, f->rally) > 4) f->rally = r;
	}
	const bool cooled = turn >= cooldownUntil_ || f->force * 4 >= cooldownForce_ * 5;
	const bool enough = haveTarget && static_cast<int> (f->members.size()) >= (extra.count > 0 ? std::max (2, kMinLaunch / 2) : kMinLaunch)
		&& forceWith >= f->need && cooled && committedCount < kMaxCommitted;
	if (f->state == eOpState::Wait)
	{
		if (enough) setState (*f, eOpState::Gather, turn, extra.count > 0 ? "reinforce" : "force enough");
	}
	else if (f->state == eOpState::Gather)
	{
		if (!haveTarget || forceWith < f->need)
			setState (*f, eOpState::Wait, turn, "force short");
		else
		{
			const int N = static_cast<int> (f->members.size());
			const int R = 1 + static_cast<int> (std::ceil (std::sqrt (static_cast<double> (std::max (1, N)))));
			int gathered = 0;
			for (unsigned int id : f->members)
				if (cheb (fighterById_.at (id)->getPosition(), f->rally) <= R + 1) ++gathered;
			if (gathered * 100 >= kGatherPct * N || turn - f->since >= kGatherTurns)
			{
				// отставшие (дальше двух ходов от точки сбора) остаются ждать следующую волну
				std::vector<unsigned int> go;
				for (unsigned int id : f->members)
				{
					const cVehicle* v = fighterById_.at (id);
					const int cellsPerTurn = std::max (1, v->data.getSpeedMax() / 4);
					if (cheb (v->getPosition(), f->rally) <= R + 2 * cellsPerTurn) go.push_back (id);
				}
				if (static_cast<int> (go.size()) >= (extra.count > 0 ? std::max (2, kMinLaunch / 2) : kMinLaunch))
				{
					f->members = go;
					f->launchMembers = static_cast<int> (go.size());
					evaluate (ctx, *f, true);
					f->launchForce = f->force;
					f->leader = chooseLeader (ctx, *f);
					setState (*f, eOpState::Advance, turn, extra.count > 0 ? "reinforcement launched" : "launched");
				}
			}
		}
	}

	if (turn % kStatusEvery == 0)
	{
		nlohmann::json e = toJson();
		e["event"] = "operation";
		e["level"] = "operational";
		e["what"] = "status";
		events_.push_back (std::move (e));
	}
}

//------------------------------------------------------------------------------
void cArmyOps::planFormation (const sContext& ctx, const sOperation& op, const cPosition& center, bool holdNear,
	const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks,
	const std::string& reason, bool fireEdge, int pacePct)
{
	const cMapView& view = *ctx.view;
	const int N = static_cast<int> (op.members.size());
	const int R = 1 + static_cast<int> (std::ceil (std::sqrt (static_cast<double> (std::max (1, N)))));
	// расстояния по суше в окне вокруг центра (без машин): клетка за водой рядом — не «рядом»
	const int B = R + 3;
	const int side = 2 * B + 1;
	std::vector<int> local (static_cast<size_t> (side) * side, -1);
	auto li = [&] (const cPosition& p) { return static_cast<size_t> (p.y() - center.y() + B) * side + (p.x() - center.x() + B); };
	const cMap& map = *ctx.model->getMap();
	if (tankTerrain (map, center))
	{
		std::vector<cPosition> q {center};
		local[li (center)] = 0;
		for (size_t h = 0; h < q.size(); ++h)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const cPosition n = q[h] + cPosition (dx, dy);
					if (cheb (n, center) > B || !tankTerrain (map, n) || local[li (n)] >= 0) continue;
					local[li (n)] = local[li (q[h])] + 1;
					q.push_back (n);
				}
	}
	// ближние к центру — первыми (внутренние клетки), при равенстве — по id
	std::vector<std::tuple<long long, unsigned int, const cVehicle*>> order;
	for (unsigned int id : op.members)
	{
		if (contains (busy, id) || tasks.count (id)) continue;
		const cVehicle* u = nullptr;
		if (const auto it = fighterById_.find (id); it != fighterById_.end()) u = it->second;
		else if (const auto st = scoutById_.find (id); st != scoutById_.end()) u = st->second; // шаг 9: корректировщики
		if (u == nullptr) continue;
		order.emplace_back (dist2 (u->getPosition(), center), id, u);
	}
	std::sort (order.begin(), order.end());
	// шаг 13 (п. 77): дальние от центра машины — по одному полю расстояний на группу (прежде — A* на
	// каждую машину по всей карте, а за ним, если пути нет, ещё и поиск движка)
	const int nearWindow = B + 2;   // ближе — короткий поиск в окне у центра
	const int localWindow = B + 4;  // окно короткого поиска
	{
		std::vector<const cVehicle*> far;
		for (const auto& [d, id, v] : order)
			if (cheb (v->getPosition(), center) > nearWindow) far.push_back (v);
		if (!far.empty()) buildFormationField (ctx, center, far);
	}
	for (const auto& [d, id, v] : order)
	{
		const cPosition up = v->getPosition();
		// шаг 9: поддержка и корректировщики — в хвосте строя (дальше от цели, чем центр)
		const bool rear = op.target.x() >= 0 && (isSupport (id) || contains (spotters_, id));
		const int centerToTarget = rear ? cheb (center, op.target) : 0;
		if (holdNear && cheb (up, center) <= R && !containsPos (reserved, up) && !avoided (up)
			&& (!rear || cheb (up, op.target) > centerToTarget))
		{
			reserved.push_back (up);
			tasks[id] = {{}, reason + " (hold)"};
			continue;
		}
		// клетка строя: 50·(шагов до центра) + (до машины), занятые и обещанные — мимо
		cPosition cell (-1, -1);
		long long bestKey = std::numeric_limits<long long>::max();
		for (int dy = -B; dy <= B; ++dy)
			for (int dx = -B; dx <= B; ++dx)
			{
				const cPosition c = center + cPosition (dx, dy);
				if (!view.isValidPosition (c)) continue;
				const int ld = local[li (c)];
				if (ld < 0) continue;
				if (avoided (c)) continue;
				if (c != up && !view.possiblePlace (*v, c)) continue;
				if (containsPos (reserved, c)) continue;
				long long key = 50LL * ld + cheb (c, up);
				if (rear)
				{
					const int ahead = centerToTarget + 1 - cheb (c, op.target);
					if (ahead > 0) key += static_cast<long long> (kRearPenalty) * ahead;
				}
				if (key < bestKey)
				{
					bestKey = key;
					cell = c;
				}
			}
		if (cell.x() < 0 || cell == up)
		{
			if (!containsPos (reserved, up)) reserved.push_back (up);
			tasks[id] = {{}, reason + " (hold)"};
			continue;
		}
		std::vector<cPosition> path;
		if (cheb (up, center) > nearWindow)
		{
			// по полю до окна у центра, оттуда — коротким поиском до своей клетки строя
			path = descendFormationField (ctx, *v, center, nearWindow);
			if (!path.empty() && cheb (path.back(), center) <= nearWindow && path.back() != cell)
			{
				auto tail = localPath (ctx, *v, path.back(), cell, center, localWindow);
				path.insert (path.end(), tail.begin(), tail.end());
			}
			// поле машину не достало (амфибия на берегу, долгий обход) — прежний поиск A*, но с пределом
			// клеток и без поиска движка следом: у того та же проходимость, он не находит того, чего не
			// нашёл A*, а запертую машину оба искали по всей области (64% времени на Gen_Straits, шаг 12)
			if (path.empty() && ctx.world->regionOf (*v, up) >= 0 && ctx.world->regionOf (*v, up) == ctx.world->regionOf (*v, cell))
				path = ctx.world->safePath (*v, view, cell, eThreatLayer::None, kFormationSearchMax);
		}
		else
			path = localPath (ctx, *v, up, cell, center, localWindow);
		truncateToTurn (*v, view, path, pacePct);
		if (fireEdge) cutAtFireEdge (*ctx.world, *v, path);
		avoidReservedEnd (path, reserved);
		reserved.push_back (path.empty() ? up : path.back());
		tasks[id] = {std::move (path), reason};
	}
}

//------------------------------------------------------------------------------
void cArmyOps::buildFormationField (const sContext& ctx, const cPosition& center, const std::vector<const cVehicle*>& far)
{
	const cMapView& view = *ctx.view;
	const size_t N = static_cast<size_t> (W_) * H_;
	if (fieldDist_.size() != N)
	{
		fieldDist_.assign (N, 0);
		fieldNext_.assign (N, -1);
		fieldSeen_.assign (N, 0);
		fieldClosed_.assign (N, 0);
		fieldGen_ = 0;
	}
	if (++fieldGen_ == 0)
	{
		std::fill (fieldSeen_.begin(), fieldSeen_.end(), 0);
		std::fill (fieldClosed_.begin(), fieldClosed_.end(), 0);
		fieldGen_ = 1;
	}
	const unsigned int gen = fieldGen_;
	if (!view.isValidPosition (center) || far.empty()) return;
	// образец — машина с самой узкой проходимостью (меньше битов «вода» и «берег»), при равенстве — первая
	const auto classOf = [] (const cVehicle& v) {
		const auto& sd = v.getStaticUnitData();
		return (sd.factorGround > 0 ? 1 : 0) | (sd.factorSea > 0 ? 2 : 0) | (sd.factorCoast > 0 ? 4 : 0);
	};
	const auto width = [] (int cls) { return ((cls & 2) ? 2 : 0) + ((cls & 4) ? 1 : 0); };
	const cVehicle* rep = far.front();
	for (const cVehicle* v : far)
		if (width (classOf (*v)) < width (classOf (*rep))) rep = v;
	fieldClass_ = classOf (*rep);
	// машины, которые поле должно достать (того же класса, что образец, и в той же области
	// проходимости): клетка машины — рядом с посчитанной клеткой. Машины других классов поле
	// берёт попутно, не достало — у них свой поиск (planFormation)
	std::map<int, int> waiting; // смещение клетки машины → 1
	const int region = ctx.world->regionOf (*rep, center);
	for (const cVehicle* v : far)
	{
		if (classOf (*v) != fieldClass_) continue;
		const cPosition p = v->getPosition();
		const int r = ctx.world->regionOf (*v, p);
		if (region >= 0 && r >= 0 && r != region) continue; // другая область — поле её не достанет
		waiting[p.y() * W_ + p.x()] = 1;
	}
	if (waiting.empty()) return;
	constexpr int kSlack = 16; // после последней достанной машины — ещё столько цены (лучший сосед — посчитан)
	int limit = std::numeric_limits<int>::max();
	// и не дальше полутора прямых путей до самой дальней машины (шаг по суше — 4, по диагонали — 6):
	// машину, запертую машинами или зданиями, поле не ищет по всей области
	int cap = 0;
	for (const auto& [o, one] : waiting)
		cap = std::max (cap, 8 * cheb (cPosition (o % W_, o / W_), center) + 80);
	using tNode = std::pair<int, int>;
	std::vector<tNode> heap;
	auto push = [&] (int d, int cell) {
		heap.push_back ({d, cell});
		std::push_heap (heap.begin(), heap.end(), std::greater<tNode>());
	};
	const int c0 = center.y() * W_ + center.x();
	fieldSeen_[c0] = gen;
	fieldDist_[c0] = 0;
	fieldNext_[c0] = -1;
	push (0, c0);
	while (!heap.empty())
	{
		std::pop_heap (heap.begin(), heap.end(), std::greater<tNode>());
		const auto [d, cur] = heap.back();
		heap.pop_back();
		if (fieldClosed_[cur] == gen) continue;
		if (d > limit || d > cap) break;
		fieldClosed_[cur] = gen;
		const cPosition p (cur % W_, cur / W_);
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (n.x() < 0 || n.y() < 0 || n.x() >= W_ || n.y() >= H_) continue;
				const int no = n.y() * W_ + n.x();
				if (!waiting.empty())
					if (const auto it = waiting.find (no); it != waiting.end())
					{
						waiting.erase (it);
						if (waiting.empty()) limit = d + kSlack;
					}
				if (fieldClosed_[no] == gen) continue;
				if (!view.possiblePlace (*rep, n)) continue;
				// шаг к центру — из n в p: цена по клетке p (как у движка — по клетке, куда въезжают)
				const int nd = d + cPathCalculator::calcNextCost (n, p, rep, &view);
				if (fieldSeen_[no] != gen || nd < fieldDist_[no])
				{
					fieldSeen_[no] = gen;
					fieldDist_[no] = nd;
					fieldNext_[no] = cur;
					push (nd, no);
				}
			}
	}
}

//------------------------------------------------------------------------------
std::vector<cPosition> cArmyOps::descendFormationField (const sContext& ctx, const cVehicle& v, const cPosition& center, int window) const
{
	std::vector<cPosition> path;
	if (fieldDist_.empty()) return path;
	const cMapView& view = *ctx.view;
	const unsigned int gen = fieldGen_;
	const cPosition up = v.getPosition();
	// первый шаг: в соседнюю посчитанную клетку — меньше (цена шага + расстояние оттуда)
	int best = -1;
	long long bestKey = std::numeric_limits<long long>::max();
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
		{
			if (dx == 0 && dy == 0) continue;
			const cPosition n = up + cPosition (dx, dy);
			if (n.x() < 0 || n.y() < 0 || n.x() >= W_ || n.y() >= H_) continue;
			const int no = n.y() * W_ + n.x();
			if (fieldClosed_[no] != gen) continue;
			if (n != center && !view.possiblePlace (v, n)) continue;
			const long long key = static_cast<long long> (fieldDist_[no] + cPathCalculator::calcNextCost (up, n, &v, &view)) * 1000000LL + no;
			if (key < bestKey)
			{
				bestKey = key;
				best = no;
			}
		}
	if (best < 0) return path;
	// дальше — по полю к центру, пока не войдёт в окно или не уйдёт за ход с запасом
	const int budget = v.data.getSpeedMax() + 16;
	int cost = 0;
	cPosition prev = up;
	for (int c = best; c >= 0; c = fieldNext_[c])
	{
		const cPosition p (c % W_, c / W_);
		cost += cPathCalculator::calcNextCost (prev, p, &v, &view);
		path.push_back (p);
		prev = p;
		if (cheb (p, center) <= window || cost > budget) break;
	}
	// центр (клетка лидера) может быть занят — не вставать на него
	while (!path.empty() && path.back() == center && !view.possiblePlace (v, center)) path.pop_back();
	return path;
}

//------------------------------------------------------------------------------
std::vector<cPosition> cArmyOps::localPath (const sContext& ctx, const cVehicle& v, const cPosition& from, const cPosition& to,
	const cPosition& center, int window) const
{
	if (from == to) return {};
	const cMapView& view = *ctx.view;
	const int side = 2 * window + 1;
	auto inWin = [&] (const cPosition& p) { return cheb (p, center) <= window; };
	if (!inWin (from) || !inWin (to) || !view.possiblePlace (v, to)) return {};
	auto li = [&] (const cPosition& p) { return static_cast<size_t> (p.y() - center.y() + window) * side + (p.x() - center.x() + window); };
	constexpr int kInf = std::numeric_limits<int>::max();
	std::vector<int> g (static_cast<size_t> (side) * side, kInf);
	std::vector<int> prev (static_cast<size_t> (side) * side, -1);
	using tNode = std::pair<int, int>;
	std::priority_queue<tNode, std::vector<tNode>, std::greater<tNode>> open;
	auto heur = [&] (const cPosition& p) { return 2 * cheb (p, to); };
	g[li (from)] = 0;
	open.push ({heur (from), static_cast<int> (li (from))});
	const auto posOf = [&] (int i) { return cPosition (center.x() - window + i % side, center.y() - window + i / side); };
	bool found = false;
	while (!open.empty())
	{
		const auto [f, cur] = open.top();
		open.pop();
		const cPosition p = posOf (cur);
		if (f - heur (p) != g[cur]) continue; // устаревшая запись
		if (p == to)
		{
			found = true;
			break;
		}
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0) continue;
				const cPosition n = p + cPosition (dx, dy);
				if (!inWin (n) || !view.isValidPosition (n) || !view.possiblePlace (v, n)) continue;
				const int ng = g[cur] + cPathCalculator::calcNextCost (p, n, &v, &view);
				const size_t ni = li (n);
				if (ng < g[ni])
				{
					g[ni] = ng;
					prev[ni] = cur;
					open.push ({ng + heur (n), static_cast<int> (ni)});
				}
			}
	}
	if (!found) return {};
	std::vector<cPosition> path;
	for (int c = static_cast<int> (li (to)); c >= 0 && c != static_cast<int> (li (from)); c = prev[c])
		path.push_back (posOf (c));
	std::reverse (path.begin(), path.end());
	return path;
}

//------------------------------------------------------------------------------
void cArmyOps::planAdvance (const sContext& ctx, sOperation& op, const std::vector<unsigned int>& busy,
	std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks)
{
	const auto lit = fighterById_.find (op.leader);
	if (lit == fighterById_.end()) return;
	const cVehicle& leader = *lit->second;
	// шаг 13: сплочённость — по первым kCohesionCap машинам: группа в сотни машин (большие карты, подмога
	// сливается в одну операцию) никогда не соберётся на 70% в радиусе — лидер ждал до конца партии
	const int N = std::min (static_cast<int> (op.members.size()), kCohesionCap);
	const int Rcoh = 2 + static_cast<int> (std::ceil (std::sqrt (static_cast<double> (std::max (1, N)))));
	const cPosition L0 = leader.getPosition();
	int near = 0;
	for (unsigned int id : op.members)
		if (const auto it = fighterById_.find (id); it != fighterById_.end() && cheb (it->second->getPosition(), L0) <= Rcoh) ++near;
	const bool cohesive = near * 100 >= kCohesionPct * N;
	const std::string tag = "op " + std::to_string (op.id) + " " + opStateName (op.state);
	cPosition L = L0;
	if (!contains (busy, leader.getId()))
	{
		auto leaderPath = [&] (const cVehicle& v) {
			// путь по клеткам с меньшей угрозой — к кругу у цели
			auto p = ctx.world->safePathNear (v, *ctx.view, op.target, std::max (1, kEngage - 2), eThreatLayer::Reach);
			if (p.empty()) p = enginePath (v, *ctx.view, op.target, true);
			truncateToTurn (v, *ctx.view, p, kPacePct);
			cutAtFireEdge (*ctx.world, v, p);
			avoidReservedEnd (p, reserved);
			return p;
		};
		std::vector<cPosition> path;
		if (cohesive) path = leaderPath (leader);
		// шаг 13: группа сплочена, а лидеру некуда шагнуть (большая группа обступила его со всех сторон —
		// на 250×250 армия в 200 машин стояла так до конца партии) — ведёт ближайшая к цели машина строя, у
		// которой путь есть; она — лидер и дальше
		const cVehicle* vanguard = nullptr;
		if (cohesive && path.empty())
		{
			std::vector<std::tuple<long long, unsigned int, const cVehicle*>> cand;
			for (unsigned int id : op.members)
				if (const auto it = fighterById_.find (id); it != fighterById_.end() && id != leader.getId() && !isSupport (id)
					&& !contains (busy, id) && !tasks.count (id))
					cand.emplace_back (dist2 (it->second->getPosition(), op.target), id, it->second);
			std::sort (cand.begin(), cand.end(), [] (const auto& a, const auto& b) {
				return std::get<0> (a) != std::get<0> (b) ? std::get<0> (a) < std::get<0> (b) : std::get<1> (a) < std::get<1> (b);
			});
			for (size_t k = 0; k < cand.size() && k < 6 && vanguard == nullptr; ++k)
			{
				auto p = leaderPath (*std::get<2> (cand[k]));
				if (p.empty()) continue;
				vanguard = std::get<2> (cand[k]);
				path = std::move (p);
			}
		}
		if (vanguard != nullptr)
		{
			L = path.back();
			reserved.push_back (L);
			tasks[vanguard->getId()] = {std::move (path), tag + ": vanguard (leader boxed in)"};
			op.leader = vanguard->getId();
		}
		else
		{
			if (!path.empty()) L = path.back();
			reserved.push_back (L);
			tasks[leader.getId()] = {std::move (path), tag + (cohesive ? ": leader" : ": leader waits for group")};
		}
	}
	planFormation (ctx, op, L, false, busy, reserved, tasks, tag + ": follow", true, 100);
}

//------------------------------------------------------------------------------
void cArmyOps::planAttack (const sContext& ctx, sOperation& op, const std::vector<unsigned int>& busy,
	std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks)
{
	const auto lit = fighterById_.find (op.leader);
	const cPosition L0 = lit != fighterById_.end() ? lit->second->getPosition() : op.target;
	const int N = std::min (static_cast<int> (op.members.size()), kCohesionCap); // шаг 13: как в походе
	const int Rcoh = 4 + static_cast<int> (std::ceil (std::sqrt (static_cast<double> (std::max (1, N)))));
	int near = 0;
	for (unsigned int id : op.members)
		if (const auto it = fighterById_.find (id); it != fighterById_.end() && cheb (it->second->getPosition(), L0) <= Rcoh) ++near;
	const bool cohesive = near * 100 >= kCohesionPct * N;
	// слаженный вход: сил хватает и группа рядом — все входят в зону огня в один ход
	const bool enter = op.force >= op.need && cohesive;
	std::vector<const sEnemyMemory*> foes; // видимые вооружённые враги у группы
	for (const auto& kv : ctx.world->enemies())
		if (kv.second.visible && kv.second.hitsGround() && dist2 (kv.second.pos, L0) <= static_cast<long long> (kSense) * kSense)
			foes.push_back (&kv.second);
	const std::string tag = "op " + std::to_string (op.id) + " attack";
	std::vector<std::tuple<long long, unsigned int, const cVehicle*>> order;
	for (unsigned int id : op.members)
	{
		if (contains (busy, id) || tasks.count (id)) continue;
		const auto it = fighterById_.find (id);
		if (it == fighterById_.end()) continue;
		order.emplace_back (dist2 (it->second->getPosition(), op.target), id, it->second);
	}
	std::sort (order.begin(), order.end());
	sOperation rearGroup; // поддержка без видимых целей — в хвосте строя у лидера
	rearGroup.target = op.target;
	for (const auto& [d, id, v] : order)
	{
		// видимые цели у группы и у цели операции (шаг 9)
		std::vector<const sEnemyMemory*> targets;
		for (const sEnemyMemory* m : attackableTargets (*ctx.model, *ctx.world, *v))
			if (dist2 (m->pos, L0) <= static_cast<long long> (kSense + kSupportSense) * (kSense + kSupportSense)
				|| dist2 (m->pos, op.target) <= static_cast<long long> (kEngage) * kEngage)
				targets.push_back (m);
		if (isSupport (id))
		{
			// поддержка: вне «удара с подъезда», бить с предельной дальности, выстрел в конце пути
			if (!targets.empty())
				planSupport (ctx, *v, targets, reserved, tasks, tag);
			else
				rearGroup.members.push_back (id);
			continue;
		}
		cPosition dest = op.target;
		long long best = std::numeric_limits<long long>::max();
		for (const sEnemyMemory* m : foes)
			if (dist2 (m->pos, v->getPosition()) < best)
			{
				best = dist2 (m->pos, v->getPosition());
				dest = m->pos;
			}
		auto path = enginePath (*v, *ctx.view, dest, true);
		truncateToTurn (*v, *ctx.view, path, 100);
		if (!enter) cutAtFireEdge (*ctx.world, *v, path);
		avoidReservedEnd (path, reserved);
		reserved.push_back (path.empty() ? v->getPosition() : path.back());
		const char* what = foes.empty() ? ": to target" : (enter ? ": enter" : ": to fire edge");
		// шаг 9: доехал до цели в дальности, выстрел остался — выстрелить в конце пути
		const unsigned int shot = endMoveTarget (*ctx.model, *v, path, targets);
		tasks[id] = {std::move (path), tag + what + (shot != kNoUnit ? " +fire" : ""), shot};
	}
	if (!rearGroup.members.empty())
	{
		cPosition L = L0;
		if (const auto it = tasks.find (op.leader); it != tasks.end() && !it->second.path.empty()) L = it->second.path.back();
		planFormation (ctx, rearGroup, L, false, busy, reserved, tasks, tag + ": support follows", true, 100);
	}
}

//------------------------------------------------------------------------------
bool cArmyOps::planSupport (const sContext& ctx, const cVehicle& v, const std::vector<const sEnemyMemory*>& foes,
	std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks, const std::string& tag)
{
	const int maxDanger = supportMaxDanger (roleOfId (v.getId()));
	sStandoff s = findStandoff (*ctx.model, *ctx.view, *ctx.world, v, foes, v.data.getRange(), true, maxDanger, reserved);
	reserved.push_back (s.path.empty() ? v.getPosition() : s.path.back());
	const bool moving = !s.path.empty();
	tasks[v.getId()] = {std::move (s.path), tag + (moving ? (s.attackId != kNoUnit ? ": standoff +fire" : ": standoff") : ": standoff (hold)"), s.attackId};
	return true;
}

//------------------------------------------------------------------------------
void cArmyOps::planScouts (const sContext& ctx, const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved,
	std::map<unsigned int, sOpTask>& tasks)
{
	const cMapView& view = *ctx.view;
	std::vector<cPosition> taken; // клетки, куда уже пошли другие разведчики
	// шаг 9: разведчики малой фабрики и корректировщики без армии в походе — тоже в разведку
	std::vector<unsigned int> explorers = scouts_;
	for (unsigned int id : spotters_)
		if (!tasks.count (id)) explorers.push_back (id);
	for (unsigned int id : explorers)
	{
		if (contains (busy, id) || tasks.count (id)) continue;
		const cVehicle* vp = nullptr;
		if (const auto it = fighterById_.find (id); it != fighterById_.end()) vp = it->second;
		else if (const auto st = scoutById_.find (id); st != scoutById_.end()) vp = st->second;
		if (vp == nullptr) continue;
		const cVehicle& v = *vp;
		std::vector<cPosition> tried;
		std::vector<cPosition> path;
		for (int attempt = 0; attempt < 3 && path.empty(); ++attempt)
		{
			// ближайшая невиданная (иначе — давно не виденная) достижимая клетка без угрозы
			cPosition best (-1, -1);
			long long bestKey = std::numeric_limits<long long>::max();
			for (int y = 0; y < H_; ++y)
				for (int x = 0; x < W_; ++x)
				{
					const cPosition p (x, y);
					if (landDist (p) < 0) continue;
					const int seen = ctx.world->seenTurn (p);
					if (seen >= ctx.turn - 20) continue;
					if (ctx.world->groundThreat (p) > 0) continue;
					if (containsPos (tried, p)) continue;
					if (ctx.scoutAvoid != nullptr)
					{
						bool avoid = false;
						for (const auto& a : *ctx.scoutAvoid)
							if (cheb (a, p) <= ctx.scoutAvoidR)
							{
								avoid = true;
								break;
							}
						if (avoid) continue;
					}
					bool near = false;
					for (const auto& t : taken)
						if (cheb (t, p) <= 15) near = true;
					if (near) continue;
					// невиданные — раньше давно виденных; из них — дальние от базы (враг высадился
					// далеко), но не в ущерб пути разведчика: ключ = путь до клетки − 1.5 × её путь от базы
					const long long key = (seen < 0 ? 0LL : 1000000000LL) + 2LL * cheb (p, v.getPosition()) - 3LL * landDist (p) / 2;
					if (key < bestKey)
					{
						bestKey = key;
						best = p;
					}
				}
			if (best.x() < 0) break;
			tried.push_back (best);
			path = ctx.world->safePathNear (v, view, best, 1, eThreatLayer::Reach);
			if (ctx.world->pathUnsafe (v, path)) path.clear();
			if (!path.empty()) taken.push_back (best);
		}
		// шаг 9: безопасного пути разведки нет, а сам под ударом врага — сначала уйти из-под него
		// (стоять на месте значило погибнуть: 152 из 256 погибших разведчиков — «explore» среди врагов)
		if (path.empty() && (ctx.world->groundStrike (v.getPosition()) > 0 || ctx.world->groundThreat (v.getPosition()) > 0))
		{
			std::vector<const sEnemyMemory*> foes;
			for (const auto& kv : ctx.world->enemies())
				if (kv.second.visible && kv.second.hitsGround()) foes.push_back (&kv.second);
			sStandoff s = findStandoff (*ctx.model, view, *ctx.world, v, foes, std::max (1, v.data.getScan() - 1), false, 0, reserved);
			const int here = ctx.world->groundFire (v.getPosition()) > 0 ? 2 : (ctx.world->groundStrike (v.getPosition()) > 0 ? 1 : 0);
			if (!s.path.empty() && s.danger <= here)
			{
				reserved.push_back (s.path.back());
				tasks[id] = {std::move (s.path), "scout: evade"};
				continue;
			}
		}
		truncateToTurn (v, view, path, 100);
		cutAtFireEdge (*ctx.world, v, path);
		avoidReservedEnd (path, reserved);
		reserved.push_back (path.empty() ? v.getPosition() : path.back());
		tasks[id] = {std::move (path), "scout: explore"};
	}
}

//------------------------------------------------------------------------------
void cArmyOps::planSpotters (const sContext& ctx, const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved,
	std::map<unsigned int, sOpTask>& tasks)
{
	// первая по id операция в походе или бою; нет — корректировщики уйдут в разведку
	const sOperation* op = nullptr;
	for (const auto& o : ops_)
		if (!o.members.empty() && (o.state == eOpState::Advance || o.state == eOpState::Attack || o.state == eOpState::Search))
		{
			op = &o;
			break;
		}
	if (op == nullptr) return;
	const auto lit = fighterById_.find (op->leader);
	if (lit == fighterById_.end()) return;
	cPosition L = lit->second->getPosition();
	if (const auto it = tasks.find (op->leader); it != tasks.end() && !it->second.path.empty()) L = it->second.path.back();
	// видимые вооружённые враги у армии
	std::vector<const sEnemyMemory*> foes;
	const long long r2 = static_cast<long long> (kSense + kSupportSense) * (kSense + kSupportSense);
	for (const auto& kv : ctx.world->enemies())
		if (kv.second.visible && kv.second.armed() && dist2 (kv.second.pos, L) <= r2) foes.push_back (&kv.second);
	sOperation group;
	group.target = op->target;
	for (unsigned int id : spotters_)
	{
		if (contains (busy, id) || tasks.count (id)) continue;
		const auto st = scoutById_.find (id);
		if (st == scoutById_.end()) continue;
		const cVehicle& v = *st->second;
		if (!foes.empty())
		{
			// держать врага в обзоре (для артиллерии), самому — вне «удара с подъезда»
			sStandoff s = findStandoff (*ctx.model, *ctx.view, *ctx.world, v, foes, std::max (1, v.data.getScan() - 1), false, 0, reserved);
			reserved.push_back (s.path.empty() ? v.getPosition() : s.path.back());
			tasks[id] = {std::move (s.path), "spot: keep in sight"};
		}
		else
			group.members.push_back (id);
	}
	if (!group.members.empty())
		planFormation (ctx, group, L, false, busy, reserved, tasks, "spot: follow army", true, 100);
}

//------------------------------------------------------------------------------
void cArmyOps::planReserve (const sContext& ctx, const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved,
	std::map<unsigned int, sOpTask>& tasks)
{
	// резерв — у края базы (точка сбора формирующейся операции), не внутри: там выезды фабрик
	const sOperation* f = nullptr;
	for (const auto& op : ops_)
		if (op.state == eOpState::Wait || op.state == eOpState::Gather) f = &op;
	const cPosition centre = (f != nullptr && f->rally.x() >= 0 && cheb (f->rally, home_) <= 2 * kReserveRadius) ? f->rally : home_;
	sOperation tmp;
	for (unsigned int id : reserve_)
	{
		if (contains (busy, id) || tasks.count (id)) continue;
		const auto it = fighterById_.find (id);
		if (it == fighterById_.end()) continue;
		if (cheb (it->second->getPosition(), centre) <= kReserveRadius && !avoided (it->second->getPosition()))
		{
			reserved.push_back (it->second->getPosition());
			tasks[id] = {{}, "reserve: hold"};
			continue;
		}
		tmp.members.push_back (id);
	}
	if (!tmp.members.empty()) planFormation (ctx, tmp, centre, false, busy, reserved, tasks, "reserve: to base", true, 100);
}

//------------------------------------------------------------------------------
bool cArmyOps::planDefense (const sContext& ctx, const sOperation& op, const std::vector<unsigned int>& busy,
	std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks)
{
	std::vector<const sEnemyMemory*> foes; // видимые вооружённые враги у точки сбора
	for (const auto& kv : ctx.world->enemies())
		if (kv.second.visible && kv.second.hitsGround() && dist2 (kv.second.pos, op.rally) <= static_cast<long long> (kDefend) * kDefend)
			foes.push_back (&kv.second);
	if (foes.empty()) return false;
	const std::string tag = "op " + std::to_string (op.id) + " " + opStateName (op.state) + ": defend";
	for (unsigned int id : op.members)
	{
		if (contains (busy, id) || tasks.count (id)) continue;
		const auto it = fighterById_.find (id);
		if (it == fighterById_.end()) continue;
		const cVehicle& v = *it->second;
		// шаг 9: цели — видимые враги у точки сбора
		std::vector<const sEnemyMemory*> targets;
		for (const sEnemyMemory* m : attackableTargets (*ctx.model, *ctx.world, v))
			if (dist2 (m->pos, op.rally) <= static_cast<long long> (kDefend + kSupportSense) * (kDefend + kSupportSense))
				targets.push_back (m);
		if (isSupport (id) && !targets.empty())
		{
			planSupport (ctx, v, targets, reserved, tasks, tag);
			continue;
		}
		const sEnemyMemory* n = foes.front();
		for (const sEnemyMemory* m : foes)
			if (dist2 (m->pos, v.getPosition()) < dist2 (n->pos, v.getPosition())) n = m;
		auto path = enginePath (v, *ctx.view, n->pos, true);
		truncateToTurn (v, *ctx.view, path, 100);
		cutAtFireEdge (*ctx.world, v, path);
		avoidReservedEnd (path, reserved);
		reserved.push_back (path.empty() ? v.getPosition() : path.back());
		const unsigned int shot = endMoveTarget (*ctx.model, v, path, targets);
		tasks[id] = {std::move (path), tag + (shot != kNoUnit ? " +fire" : ""), shot};
	}
	return true;
}

//------------------------------------------------------------------------------
void cArmyOps::plan (const sContext& ctx, const std::vector<unsigned int>& busy, std::map<unsigned int, sOpTask>& tasks)
{
	std::vector<cPosition> reserved;
	// сначала операции в походе и бою (по id), затем отход, формирующаяся, резерв, разведка
	for (auto& op : ops_)
	{
		if (op.members.empty()) continue;
		switch (op.state)
		{
			case eOpState::Advance:
			case eOpState::Search:
				planAdvance (ctx, op, busy, reserved, tasks);
				break;
			case eOpState::Attack:
				planAttack (ctx, op, busy, reserved, tasks);
				break;
			default:
				break;
		}
	}
	for (auto& op : ops_)
		if (!op.members.empty() && op.state == eOpState::Retreat)
			planFormation (ctx, op, op.rally, true, busy, reserved, tasks, "op " + std::to_string (op.id) + " retreat", false, 100);
	if (sOperation* f = forming(); f != nullptr && !f->members.empty() && f->rally.x() >= 0)
		if (!planDefense (ctx, *f, busy, reserved, tasks))
			planFormation (ctx, *f, f->rally, true, busy, reserved, tasks,
				"op " + std::to_string (f->id) + " " + opStateName (f->state) + ": rally", true, 100);
	planReserve (ctx, busy, reserved, tasks);
	planSpotters (ctx, busy, reserved, tasks);
	planScouts (ctx, busy, reserved, tasks);
}

} // namespace ai
