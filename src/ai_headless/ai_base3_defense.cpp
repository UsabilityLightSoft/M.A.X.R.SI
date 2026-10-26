/***************************************************************************
 * ai_base3_defense.cpp — замороженная копия ai_defense.cpp (мозг «base3»).
 *
 * ЗАМОРОЖЕН 26.10.2026, НЕ ПРАВИТЬ — чемпион с шага 13, часть 2 (PLAN.md §6, мерило — последний
 * чемпион): итог шага 13, часть 1 (rules/13_1_defense.md: оборона, высадка из файла карты,
 * скорость; full D = 0.565 против base2) на движке с правками 09–12. Копия сделана скриптом
 * tmp/step13/tools/make_base3.py; отличия от ai_defense.cpp — только пространство имён ai::base3,
 * стражи заголовков, include копий и name() = "base3".
 ***************************************************************************/

/***************************************************************************
 * ai_defense.cpp — анализ карты (П3) и оборона базы от налёта (П4–П9, П18, П22),
 * шаг 13 PLAN.md, часть 1. Описание — в заголовке.
 ***************************************************************************/

#include "ai_base3_defense.h"

#include "ai_base3_roles.h"
#include "ai_base3_world_model.h"

#include "game/data/base/base.h"
#include "game/data/map/map.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <tuple>

namespace ai::base3
{

namespace
{

// типы зданий и машин (как в ai_rule_brain.cpp: экономика ищет здания по sID)
const sID kIdRadar (1, 26);
const sID kIdGunAA (1, 15);
const sID kIdBlock (1, 2);
const sID kIdTraining (1, 34);
const sID kIdLandMine (1, 21);
const sID kIdEnergyBig (1, 7);
const sID kIdEnergySmall (1, 8);
const sID kIdMineStation (1, 22);
const sID kIdFactoryBig (1, 11);
const sID kIdInfantry (0, 18);
const sID kIdCommando (0, 13);
const sID kIdDepot (1, 5);

// --- пороги уровней обороны (простой вариант; сколько металла в оборону — потом решает Модель) ---
// добыча металла стартовой шахты — около 12 в ход: уровни — с первой своей шахты и дальше
// (бедная экономика — Donuts — уровни по ходу позже: страховка там дороже)
constexpr int kLevel1Income = 16;
constexpr int kLevel1Turn = 60;
constexpr int kLevel2Income = 20;
constexpr int kLevel2Turn = 100;
constexpr int kLevel3Income = 28;
constexpr int kLevel3Turn = 150;
constexpr int kCommandoAlarmTurns = 40; // тревога «диверсант» держится столько ходов после выключения
constexpr int kGuardsCap = 8;           // постов пехоты — не больше (ур. 3)
constexpr int kGuardsCapAlarm = 6;      // по тревоге до ур. 3
constexpr int kMineFieldCells = 12;     // мин на подходах (всего своих мин на карте)
constexpr int kMineMinFromLanding = 16; // мины — не ближе к высадке (там площадки зданий базы, kBaseRadius 14)
constexpr int kSeaNear = 20;            // до большого моря не дальше — упор «море» возможен
constexpr int kBigSeaCells = 400;       // водоём от стольких клеток — море (флот пройдёт)
constexpr int kStatusEvery = 10;        // снимок обороны в журнал — раз в столько ходов

int cheb (const cPosition& a, const cPosition& b)
{
	return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
}

long long dist2 (const cPosition& a, const cPosition& b)
{
	return (a - b).l2NormSquared();
}

std::vector<cPosition> footprint (const cPosition& q, bool big)
{
	if (!big) return {q};
	return {q, q + cPosition (1, 0), q + cPosition (0, 1), q + cPosition (1, 1)};
}

// кольцо клеток вокруг здания (с углами) — откуда коммандос выключает, куда выезжают машины
std::vector<cPosition> ring (const cPosition& q, bool big)
{
	const int s = big ? 2 : 1;
	std::vector<cPosition> r;
	for (int dy = -1; dy <= s; ++dy)
		for (int dx = -1; dx <= s; ++dx)
			if (dx < 0 || dy < 0 || dx >= s || dy >= s) r.push_back (q + cPosition (dx, dy));
	return r;
}

bool inFootprint (const cPosition& p, const cPosition& q, bool big)
{
	const int s = big ? 2 : 1;
	return p.x() >= q.x() && p.y() >= q.y() && p.x() < q.x() + s && p.y() < q.y() + s;
}

// на клетке здание, которое не пускает машину (не соединитель, не дорога, не мина, не обломки)
bool solidBuilding (const cMap& map, const cPosition& p)
{
	for (const cBuilding* b : map.getField (p).getBuildings())
	{
		if (b->isRubble()) continue;
		const auto sp = b->getStaticUnitData().surfacePosition;
		if (sp != eSurfacePosition::Above && sp != eSurfacePosition::Base && sp != eSurfacePosition::AboveBase) return true;
	}
	return false;
}

// клетка проходима для наземной машины (суша или берег, без «твёрдого» здания)
bool walkable (const cMap& map, const cPosition& p)
{
	if (!map.isValidPosition (p) || map.isBlocked (p) || map.isWater (p)) return false;
	return !solidBuilding (map, p);
}

int explored (const cMap& map, const cPlayer& player, const cPosition& p, eResourceType type)
{
	if (!map.isValidPosition (p) || !player.hasResourceExplored (p)) return 0;
	const auto& r = map.getResource (p);
	return r.typ == type ? r.value : 0;
}

bool hasOwnLandMine (const cMap& map, const cPlayer& player, const cPosition& p)
{
	for (const cBuilding* b : map.getField (p).getBuildings())
		if (b->getOwner() == &player && b->getStaticUnitData().ID == kIdLandMine) return true;
	return false;
}

// 8 направлений кольца ПВО: сначала 4 стороны, потом диагонали — так, что первые 4 и первые 6 разнесены
const cPosition kRingDirs[8] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};

} // namespace

//------------------------------------------------------------------------------
nlohmann::json sMapPicture::toJson() const
{
	return {{"emphasis", emphasis}, {"sure", sure}, {"why", why}, {"waterPct", waterPct}, {"landRegionCells", landRegionCells},
		{"landTotal", landTotal}, {"landSharePct", landSharePct}, {"bigRegions", bigRegions}, {"islands", islands},
		{"seaDistance", seaDistance}, {"seaCells", seaCells}, {"enemyLand", enemyLand}};
}

//------------------------------------------------------------------------------
void cBaseDefense::updateMap (const cModel& model, const cPlayer& player, const cWorldModel& world)
{
	const cMap& map = *model.getMap();
	// статичное — раз за партию: доля воды, водоёмы
	if (!waterDone_)
	{
		waterDone_ = true;
		const size_t N = static_cast<size_t> (W_) * H_;
		long long water = 0;
		seaLabel_.assign (N, -1);
		seaSizes_.clear();
		std::vector<int> queue;
		for (int y = 0; y < H_; ++y)
			for (int x = 0; x < W_; ++x)
			{
				const cPosition p (x, y);
				if (map.isWater (p) || map.isCoast (p)) ++water;
			}
		waterPct_ = N > 0 ? static_cast<int> (water * 100 / static_cast<long long> (N)) : 0;
		for (size_t s = 0; s < N; ++s)
		{
			const cPosition sp (static_cast<int> (s % W_), static_cast<int> (s / W_));
			if (seaLabel_[s] >= 0 || !map.isWater (sp)) continue;
			const int id = static_cast<int> (seaSizes_.size());
			seaLabel_[s] = id;
			queue.assign (1, static_cast<int> (s));
			for (size_t qi = 0; qi < queue.size(); ++qi)
			{
				const int cx = queue[qi] % W_, cy = queue[qi] / W_;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = cx + dx, ny = cy + dy;
						if (nx < 0 || ny < 0 || nx >= W_ || ny >= H_) continue;
						const size_t o = static_cast<size_t> (ny) * W_ + nx;
						if (seaLabel_[o] >= 0 || !map.isWater (cPosition (nx, ny))) continue;
						seaLabel_[o] = id;
						queue.push_back (static_cast<int> (o));
					}
			}
			seaSizes_.push_back (static_cast<int> (queue.size()));
		}
		bigSea_ = -1;
		for (int i = 0; i < static_cast<int> (seaSizes_.size()); ++i)
			if (bigSea_ < 0 || seaSizes_[i] > seaSizes_[bigSea_]) bigSea_ = i;
	}
	sMapPicture m;
	m.waterPct = waterPct_;
	m.seaCells = bigSea_ >= 0 ? seaSizes_[bigSea_] : 0;
	// область суши танка у базы (высадка — на суше; иначе — ближайшая клетка суши)
	const cPosition home = player.getLandingPos();
	for (int r = 0; r <= 4 && m.landRegion < 0; ++r)
		for (int dy = -r; dy <= r && m.landRegion < 0; ++dy)
			for (int dx = -r; dx <= r && m.landRegion < 0; ++dx)
				if (std::max (std::abs (dx), std::abs (dy)) == r) m.landRegion = world.tankRegionOf (home + cPosition (dx, dy));
	m.landRegionCells = world.tankRegionSize (m.landRegion);
	for (int i = 0;; ++i)
	{
		const int s = world.tankRegionSize (i); // 0 — областей больше нет
		if (s <= 0) break;
		if (s >= 20) m.landTotal += s;
		if (s >= 300) ++m.bigRegions;
		else if (s >= 20) ++m.islands;
	}
	m.landSharePct = m.landTotal > 0 ? static_cast<int> (static_cast<long long> (m.landRegionCells) * 100 / m.landTotal) : 100;
	// до большого моря от базы (Чебышёв, до 40 клеток)
	if (bigSea_ >= 0 && m.seaCells >= kBigSeaCells)
		for (int r = 0; r <= 40 && m.seaDistance < 0; ++r)
			for (int dy = -r; dy <= r && m.seaDistance < 0; ++dy)
				for (int dx = -r; dx <= r && m.seaDistance < 0; ++dx)
				{
					if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
					const cPosition p = home + cPosition (dx, dy);
					if (p.x() < 0 || p.y() < 0 || p.x() >= W_ || p.y() >= H_) continue;
					if (seaLabel_[static_cast<size_t> (p.y()) * W_ + p.x()] == bigSea_) m.seaDistance = r;
				}
	// где враг: здания врага из памяти (честно — только виденные)
	int ours = 0, elsewhere = 0;
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& e = kv.second;
		if (!e.building || e.owner < 0) continue;
		const int r = world.tankRegionOf (e.pos);
		if (r >= 0 && r == m.landRegion) ++ours;
		else ++elsewhere;
	}
	m.enemyLand = ours > 0 ? 1 : (elsewhere > 0 ? 0 : -1);
	const bool seaNear = m.seaDistance >= 0 && m.seaDistance <= kSeaNear;
	if (m.enemyLand == 1)
	{
		m.emphasis = "land";
		m.sure = true;
		m.why = "enemy buildings on our land";
	}
	else if (m.enemyLand == 0)
	{
		m.emphasis = seaNear ? "sea" : "air";
		m.sure = true;
		m.why = seaNear ? "enemy only beyond water, the sea is near the base" : "enemy only beyond water, no sea near the base";
	}
	else if (m.landSharePct >= 60)
	{
		m.emphasis = "land";
		m.why = "enemy not seen; our land region is most of the land";
	}
	else
	{
		m.emphasis = seaNear ? "sea" : "air";
		m.why = std::string ("enemy not seen; our land region is ") + std::to_string (m.landSharePct) + "% of the land"
			+ (seaNear ? ", the sea is near the base" : ", no sea near the base");
	}
	map_ = m;
	if (lastEmphasis_ != map_.emphasis || lastSure_ != map_.sure)
	{
		nlohmann::json e = map_.toJson();
		e["event"] = "map";
		e["level"] = "strategy";
		e["from"] = lastEmphasis_.empty() ? std::string ("none") : lastEmphasis_;
		events_.push_back (std::move (e));
		lastEmphasis_ = map_.emphasis;
		lastSure_ = map_.sure;
	}
}

//------------------------------------------------------------------------------
void cBaseDefense::update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
	const cComposition& comp, const cSubBase* main, int income, int turn)
{
	turn_ = turn;
	const cMap& map = *model.getMap();
	W_ = map.getSize().x();
	H_ = map.getSize().y();
	const cPosition home = player.getLandingPos();

	// центр и радиус базы: здания главной подбазы у высадки (шахты бывают в 18 клетках — не тянут центр)
	{
		long long sx = 0, sy = 0, n = 0;
		std::vector<std::pair<cPosition, bool>> core;
		for (const auto& b : player.getBuildings())
		{
			if (main != nullptr && b->subBase != main) continue;
			if (b->getStaticUnitData().surfacePosition != eSurfacePosition::Ground) continue;
			if (cheb (b->getPosition(), home) > 14) continue;
			core.emplace_back (b->getPosition(), b->getIsBig());
			sx += 2 * b->getPosition().x() + (b->getIsBig() ? 1 : 0);
			sy += 2 * b->getPosition().y() + (b->getIsBig() ? 1 : 0);
			n += 2;
		}
		center_ = n > 0 ? cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n)) : home;
		radius_ = 3;
		for (const auto& [q, big] : core)
			for (const auto& c : footprint (q, big))
				radius_ = std::max (radius_, cheb (c, center_) + 1);
		radius_ = std::min (radius_, 14);
	}

	// выезды фабрик и хранилищ, свои радары и ПВО-пушки, выключенные свои юниты
	exits_.clear();
	radars_.clear();
	aaGuns_.clear();
	counts_ = {};
	bool newDisable = false;
	std::vector<unsigned int> disabledNow;
	// выключенные с прошлого хода (сигнал модели): выключение на 1 ход к нашему решению уже снято
	for (unsigned int id : reportedDisabled_)
	{
		const cUnit* u = model.getUnitFromID (id);
		if (u == nullptr || u->getOwner() != &player) continue;
		newDisable = true;
		const auto& sd = u->getStaticUnitData();
		if (u->isABuilding() && sd.canBuild.empty() && sd.storageUnitsMax == 0
			&& std::find (alarmSites_.begin(), alarmSites_.end(), u->getPosition()) == alarmSites_.end())
			alarmSites_.push_back (u->getPosition());
		recentDisables_.emplace_back (u->getPosition(), turn);
		events_.push_back ({{"event", "defense"}, {"level", "operational"}, {"what", "disabled"}, {"unitId", id},
			{"type", sd.getDefaultName()}, {"pos", {{"x", u->getPosition().x()}, {"y", u->getPosition().y()}}}});
	}
	reportedDisabled_.clear();
	// поиск у выключенного здания — 4 хода после выключения
	recentDisables_.erase (std::remove_if (recentDisables_.begin(), recentDisables_.end(), [turn] (const auto& r) { return turn - r.second > 4; }),
		recentDisables_.end());
	for (const auto& b : player.getBuildings())
	{
		const auto& sd = b->getStaticUnitData();
		if (!sd.canBuild.empty() || sd.storageUnitsMax > 0)
			for (const auto& c : ring (b->getPosition(), b->getIsBig())) exits_.push_back (c);
		if (sd.ID == kIdRadar)
		{
			radars_.push_back (b->getPosition());
			if (!b->isDisabled()) ++counts_.radarsWorking;
		}
		if (sd.ID == kIdGunAA) aaGuns_.push_back (b->getPosition());
		if (sd.ID == kIdBlock) ++counts_.walls;
		if (b->isDisabled())
		{
			disabledNow.push_back (b->getId());
			++counts_.disabled;
			if (std::find (disabledSeen_.begin(), disabledSeen_.end(), b->getId()) == disabledSeen_.end())
			{
				newDisable = true;
				// стены — вокруг выключенного здания, если ему не нужен выезд
				if (sd.canBuild.empty() && sd.storageUnitsMax == 0
					&& std::find (alarmSites_.begin(), alarmSites_.end(), b->getPosition()) == alarmSites_.end())
					alarmSites_.push_back (b->getPosition());
				events_.push_back ({{"event", "defense"}, {"level", "operational"}, {"what", "disabled"}, {"unitId", b->getId()},
					{"type", sd.getDefaultName()}, {"pos", {{"x", b->getPosition().x()}, {"y", b->getPosition().y()}}}});
			}
		}
	}
	for (const auto& v : player.getVehicles())
		if (v->isDisabled())
		{
			disabledNow.push_back (v->getId());
			++counts_.disabled;
			if (std::find (disabledSeen_.begin(), disabledSeen_.end(), v->getId()) == disabledSeen_.end()) newDisable = true;
		}
	disabledSeen_ = disabledNow; // очнулся — снова выключат — снова тревога
	// здания тревоги, которых больше нет, — забыть
	alarmSites_.erase (std::remove_if (alarmSites_.begin(), alarmSites_.end(), [&] (const cPosition& p) {
		for (const auto& b : player.getBuildings())
			if (b->getPosition() == p) return false;
		return true;
	}), alarmSites_.end());
	// коммандос врага на виду (или в памяти) — тоже тревога
	bool commandoSeen = false;
	for (const auto& kv : world.enemies())
		if (!kv.second.building && sID (kv.second.typeFirst, kv.second.typeSecond) == kIdCommando && world.age (kv.second) <= 1)
			commandoSeen = true;
	if (newDisable || commandoSeen)
	{
		if (commandoAlarmUntil_ <= turn)
			events_.push_back ({{"event", "defense"}, {"level", "operational"}, {"what", "commando alarm"},
				{"why", newDisable ? "own unit disabled" : "enemy commando seen"}});
		commandoAlarmUntil_ = turn + kCommandoAlarmTurns;
	}
	airAlarm_ = comp.enemy().air > 0 || comp.enemy().airFactories > 0;

	// уровень обороны
	int factories = 0;
	bool training = false;
	for (const auto& b : player.getBuildings())
	{
		if (b->getStaticUnitData().ID == kIdFactoryBig) ++factories;
		if (b->getStaticUnitData().ID == kIdTraining) training = true;
	}
	level_ = 0;
	if (factories >= 1 && (income >= kLevel1Income || turn >= kLevel1Turn))
	{
		level_ = 1;
		if (income >= kLevel2Income || turn >= kLevel2Turn) level_ = 2;
		if (income >= kLevel3Income || turn >= kLevel3Turn) level_ = 3;
	}
	// видна авиация врага — радары и кольцо ПВО сразу, как есть фабрика
	if (airAlarm_ && factories >= 1) level_ = std::max (level_, 1);
	radarsWanted_ = level_ >= 3 ? 3 : level_;
	aaWanted_ = level_ >= 3 ? 6 : (level_ >= 2 ? 4 : 0);
	if (airAlarm_ && level_ >= 1)
	{
		radarsWanted_ = std::max (radarsWanted_, 2);
		aaWanted_ = std::max (aaWanted_, level_ >= 3 ? 8 : 6);
	}
	// стены: ур. 2 — электростанции; ур. 3 — и радары, и две главные шахты; тревога — выключенные здания
	wallTargets_.clear();
	if (commandoAlarm())
		for (const auto& p : alarmSites_)
			for (const auto& b : player.getBuildings())
				if (b->getPosition() == p) wallTargets_.emplace_back (p, b->getIsBig());
	if (level_ >= 2)
		for (const auto& b : player.getBuildings())
		{
			const sID id = b->getStaticUnitData().ID;
			if (id == kIdEnergyBig || id == kIdEnergySmall) wallTargets_.emplace_back (b->getPosition(), b->getIsBig());
		}
	if (level_ >= 3)
	{
		for (const auto& b : player.getBuildings())
			if (b->getStaticUnitData().ID == kIdRadar) wallTargets_.emplace_back (b->getPosition(), false);
		std::vector<std::tuple<int, unsigned int, cPosition>> mines; // −добыча, id, клетка
		for (const auto& b : player.getBuildings())
			if (b->getStaticUnitData().ID == kIdMineStation) mines.emplace_back (-b->getMaxProd().total(), b->getId(), b->getPosition());
		std::sort (mines.begin(), mines.end(), [] (const auto& a, const auto& b) {
			return std::get<0> (a) != std::get<0> (b) ? std::get<0> (a) < std::get<0> (b) : std::get<1> (a) < std::get<1> (b);
		});
		for (size_t i = 0; i < mines.size() && i < 2; ++i) wallTargets_.emplace_back (std::get<2> (mines[i]), true);
	}
	// без повторов (тревога и уровень могут назвать одно здание)
	{
		std::vector<std::pair<cPosition, bool>> uniq;
		for (const auto& t : wallTargets_)
			if (std::find (uniq.begin(), uniq.end(), t) == uniq.end()) uniq.push_back (t);
		wallTargets_.swap (uniq);
	}
	// учебный центр (пехота): ур. 3 или тревога «диверсант» (с фабрикой); депо и мобильный радар — ур. 3
	wantTrainingHall_ = factories >= 1 && (level_ >= 3 || commandoAlarm());
	wantDepot_ = level_ >= 3;
	bool haveDepot = false;
	for (const auto& b : player.getBuildings())
		if (b->getStaticUnitData().ID == kIdDepot) haveDepot = true;
	scannersWanted_ = level_ >= 3 && haveDepot ? 1 : 0; // мобильный радар — когда есть где прятать
	minelayersWanted_ = level_ >= 3 && income >= kLevel3Income ? 1 : 0;
	surveyorsWanted_ = level_ >= 3 && income >= kLevel3Income ? 1 : 0;

	updateMap (model, player, world);
	updatePosts (model, player, roles);
	updateRing (model, player, main);
	guardsWanted_ = training || wantTrainingHall_ ? static_cast<int> (posts_.size()) : 0;
	updateMineField (model, player, world);

	// счётчики и «отставание» стройки обороны — второй пионер, когда дел много
	counts_.radars = static_cast<int> (radars_.size());
	counts_.radarsWanted = radarsWanted_;
	counts_.aa = static_cast<int> (aaGuns_.size());
	counts_.aaWanted = aaWanted_;
	for (const auto& [q, big] : wallTargets_)
		for (const auto& c : ring (q, big))
			if (map.isValidPosition (c) && !map.isWater (c) && !map.isCoast (c) && !solidBuilding (map, c) && !exitCell (c)) ++counts_.wallsOpen;
	int guards = 0;
	for (const auto& v : player.getVehicles())
		if (roles.roleOf (*v) == eRole::Guard) ++guards;
	counts_.guards = guards;
	counts_.posts = static_cast<int> (posts_.size());
	const int backlog = std::max (0, radarsWanted_ - counts_.radars) + std::max (0, aaWanted_ - counts_.aa) + (counts_.wallsOpen + 3) / 4;
	pioneersWanted_ = 1 + ((level_ >= 2 && backlog >= 3) || commandoAlarm() ? 1 : 0);

	if (level_ != lastLevel_)
	{
		events_.push_back ({{"event", "defense"}, {"level", "strategy"}, {"what", "level"}, {"from", lastLevel_}, {"to", level_},
			{"income", income}, {"why", level_ == 0 ? "no factory or income" : (level_ == 1 ? (income >= kLevel1Income ? "income" : (turn >= kLevel1Turn ? "turn" : "enemy air"))
				: (level_ == 2 ? (income >= kLevel2Income ? "income" : "turn") : (income >= kLevel3Income ? "income" : "turn")))}});
		lastLevel_ = level_;
	}
	if (turn % kStatusEvery == 0)
	{
		nlohmann::json e = toJson();
		e["event"] = "defense";
		e["level"] = "operational";
		e["what"] = "status";
		events_.push_back (std::move (e));
	}
}

//------------------------------------------------------------------------------
void cBaseDefense::updateRing (const cModel& model, const cPlayer& player, const cSubBase* main)
{
	linkCell_ = cPosition (-1, -1);
	linkCut_ = cPosition (-1, -1);
	counts_.cutPoints = 0;
	if (level_ < 2 || main == nullptr) return;
	const cMap& map = *model.getMap();
	const size_t N = static_cast<size_t> (W_) * H_;
	const sID kIdConnector (1, 4);
	// клетки подбазы: 1 — здание, 2 — соединитель (кандидат в точку отказа)
	std::vector<char> cell (N, 0);
	std::vector<int> linkCells;
	int core = -1;
	long long coreKey = std::numeric_limits<long long>::max();
	for (const cBuilding* b : main->getBuildings())
	{
		const bool conn = b->getStaticUnitData().ID == kIdConnector;
		for (const auto& c : footprint (b->getPosition(), b->getIsBig()))
		{
			if (!map.isValidPosition (c)) continue;
			const int o = c.y() * W_ + c.x();
			if (conn)
			{
				if (cell[o] == 0) cell[o] = 2;
				linkCells.push_back (o);
				continue;
			}
			cell[o] = 1;
			// ядро — клетка фабрики у центра базы (иначе — любого здания у центра)
			const long long key = (b->getStaticUnitData().canBuild.empty() ? 1000000000LL : 0LL) + dist2 (c, center_) * 100000 + o;
			if (key < coreKey)
			{
				coreKey = key;
				core = o;
			}
		}
	}
	if (core < 0 || linkCells.empty()) return;
	std::sort (linkCells.begin(), linkCells.end());
	linkCells.erase (std::unique (linkCells.begin(), linkCells.end()), linkCells.end());
	int buildingsTotal = 0;
	for (size_t o = 0; o < N; ++o)
		if (cell[o] == 1) ++buildingsTotal;
	const int dirs[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
	std::vector<int> mark (N, 0);
	int stamp = 0;
	int bestCut = -1;
	std::vector<int> bestReached;
	for (int cut : linkCells)
	{
		if (cell[cut] != 2) continue;
		++stamp;
		std::vector<int> q {core};
		mark[core] = stamp;
		int reached = 1;
		for (size_t i = 0; i < q.size(); ++i)
			for (const auto& d : dirs)
			{
				const int x = q[i] % W_ + d[0], y = q[i] / W_ + d[1];
				if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
				const int o = y * W_ + x;
				if (o == cut || cell[o] == 0 || mark[o] == stamp) continue;
				mark[o] = stamp;
				if (cell[o] == 1) ++reached;
				q.push_back (o);
			}
		if (reached >= buildingsTotal) continue;
		++counts_.cutPoints;
		if (bestCut < 0)
		{
			bestCut = cut;
			bestReached = q;
		}
	}
	if (bestCut < 0) return;
	linkCut_ = cPosition (bestCut % W_, bestCut / W_);
	// стороны: M (1) — достижимые от ядра без точки отказа, S (2) — отрезанные клетки подбазы
	std::vector<char> side (N, 0);
	for (int o : bestReached) side[o] = 1;
	for (size_t o = 0; o < N; ++o)
		if (cell[o] != 0 && side[o] == 0 && static_cast<int> (o) != bestCut) side[o] = 2;
	const auto& connData = model.getUnitsData()->getStaticUnitData (kIdConnector);
	auto freeCell = [&] (int o) {
		const cPosition p (o % W_, o / W_);
		if (cell[o] != 0 || map.isBlocked (p) || !map.getField (p).getBuildings().empty()) return false;
		if (exitCell (p)) return false;
		if (explored (map, player, p, eResourceType::Metal) >= 2 || explored (map, player, p, eResourceType::Oil) >= 3) return false;
		return map.possiblePlaceBuilding (connData, p, &player, map.getField (p).getVehicle());
	};
	auto touches = [&] (int o, char s) {
		for (const auto& d : dirs)
		{
			const int x = o % W_ + d[0], y = o / W_ + d[1];
			if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
			if (side[y * W_ + x] == s) return true;
		}
		return false;
	};
	// обход: от свободных клеток у S до свободной клетки у M (по сторонам, не длиннее 12)
	std::vector<int> dist (N, -1), prev (N, -1), q;
	for (size_t o = 0; o < N; ++o)
		if (side[o] == 0 && touches (static_cast<int> (o), 2) && freeCell (static_cast<int> (o)))
		{
			dist[o] = 0;
			q.push_back (static_cast<int> (o));
		}
	int goal = -1;
	for (size_t i = 0; i < q.size(); ++i)
	{
		const int o = q[i];
		if (touches (o, 1))
		{
			goal = o;
			break;
		}
		if (dist[o] >= 12) continue;
		for (const auto& d : dirs)
		{
			const int x = o % W_ + d[0], y = o / W_ + d[1];
			if (x < 0 || y < 0 || x >= W_ || y >= H_) continue;
			const int n = y * W_ + x;
			if (dist[n] >= 0 || side[n] != 0 || !freeCell (n)) continue;
			dist[n] = dist[o] + 1;
			prev[n] = o;
			q.push_back (n);
		}
	}
	if (goal < 0) return;
	// строить с отрезанной стороны: первая клетка цепочки обхода
	int first = goal;
	while (prev[first] >= 0) first = prev[first];
	linkCell_ = cPosition (first % W_, first / W_);
}

//------------------------------------------------------------------------------
bool cBaseDefense::exitCell (const cPosition& p) const
{
	return std::find (exits_.begin(), exits_.end(), p) != exits_.end();
}

//------------------------------------------------------------------------------
bool cBaseDefense::spotOk (const cModel& model, const cPlayer& player, const cWorldModel& world, const sID& type, const cPosition& c,
	const std::vector<std::pair<cPosition, bool>>& reserved, const std::vector<cPosition>& extraBlocked) const
{
	const cMap& map = *model.getMap();
	if (!map.isValidPosition (c) || map.isWater (c) || map.isCoast (c) || map.isBlocked (c)) return false;
	const auto& data = model.getUnitsData()->getStaticUnitData (type);
	if (!map.possiblePlaceBuilding (data, c, &player, map.getField (c).getVehicle())) return false;
	if (const cVehicle* v = map.getField (c).getVehicle(); v != nullptr && (v->getOwner() != &player || v->isUnitBuildingABuilding())) return false;
	for (const auto& [q, big] : reserved)
		if (inFootprint (c, q, big)) return false;
	if (std::find (extraBlocked.begin(), extraBlocked.end(), c) != extraBlocked.end()) return false;
	if (exitCell (c)) return false;
	// ресурсы — под шахты (как findBaseSpot правил)
	if (explored (map, player, c, eResourceType::Metal) >= 2 || explored (map, player, c, eResourceType::Oil) >= 3
		|| explored (map, player, c, eResourceType::Gold) >= 2) return false;
	if (world.groundThreat (c) > 0) return false;
	// проход: соседние проходимые клетки должны остаться связаны в окне 5×5 без этой клетки
	std::vector<cPosition> nbrs;
	auto open = [&] (const cPosition& p) {
		if (p == c || !walkable (map, p)) return false;
		if (std::find (extraBlocked.begin(), extraBlocked.end(), p) != extraBlocked.end()) return false;
		for (const auto& [q, big] : reserved)
			if (inFootprint (p, q, big)) return false;
		return true;
	};
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			if ((dx != 0 || dy != 0) && open (c + cPosition (dx, dy))) nbrs.push_back (c + cPosition (dx, dy));
	if (nbrs.size() >= 2)
	{
		std::vector<cPosition> seen {nbrs.front()};
		for (size_t i = 0; i < seen.size(); ++i)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const cPosition n = seen[i] + cPosition (dx, dy);
					if (cheb (n, c) > 2 || !open (n) || std::find (seen.begin(), seen.end(), n) != seen.end()) continue;
					seen.push_back (n);
				}
		for (const auto& n : nbrs)
			if (std::find (seen.begin(), seen.end(), n) == seen.end()) return false;
	}
	return true;
}

//------------------------------------------------------------------------------
cBaseDefense::sBuild cBaseDefense::nextSmall (const cModel& model, const cPlayer& player, const cWorldModel& world,
	const std::vector<std::pair<sID, cPosition>>& planned, const std::vector<std::pair<cPosition, bool>>& reserved) const
{
	sBuild none;
	if (level_ <= 0 && !commandoAlarm()) return none;
	const cMap& map = *model.getMap();
	std::vector<cPosition> radars = radars_, guns = aaGuns_, walls;
	for (const auto& [id, p] : planned)
	{
		if (id == kIdRadar) radars.push_back (p);
		else if (id == kIdGunAA) guns.push_back (p);
		else if (id == kIdBlock) walls.push_back (p);
	}
	auto offs = [this] (const cPosition& p) { return static_cast<long long> (p.y()) * W_ + p.x(); };

	// радар: первый — у центра базы, следующие — дальше всех от прочих (не ближе 6 клеток), внутри базы
	auto radarSpot = [&] () -> cPosition {
		cPosition best (-1, -1);
		long long bestKey = std::numeric_limits<long long>::max();
		const int R = std::max (3, radius_);
		for (int dy = -R; dy <= R; ++dy)
			for (int dx = -R; dx <= R; ++dx)
			{
				const cPosition c = center_ + cPosition (dx, dy);
				if (!spotOk (model, player, world, kIdRadar, c, reserved, walls)) continue;
				long long key = dist2 (c, center_) * 100000 + offs (c);
				if (!radars.empty())
				{
					long long near = std::numeric_limits<long long>::max();
					for (const auto& r : radars) near = std::min (near, dist2 (c, r));
					if (near < 36) continue;
					key = -near * 100000000LL + dist2 (c, center_) * 100000 + offs (c);
				}
				if (key < bestKey)
				{
					bestKey = key;
					best = c;
				}
			}
		return best;
	};
	// ПВО-пушка: точки кольца по краю базы (8 направлений по порядку), занятая точка — пушка в 3 клетках
	auto aaSpot = [&] (int want) -> cPosition {
		const int rr = std::clamp (radius_, 3, 7);
		for (int k = 0; k < want && k < 8; ++k)
		{
			const cPosition d = kRingDirs[k];
			const cPosition t = (d.x() != 0 && d.y() != 0) ? center_ + cPosition (d.x() * rr * 7 / 10, d.y() * rr * 7 / 10) : center_ + cPosition (d.x() * rr, d.y() * rr);
			bool covered = false;
			for (const auto& g : guns)
				if (cheb (g, t) <= 3) covered = true;
			if (covered) continue;
			cPosition best (-1, -1);
			long long bestKey = std::numeric_limits<long long>::max();
			for (int dy = -3; dy <= 3; ++dy)
				for (int dx = -3; dx <= 3; ++dx)
				{
					const cPosition c = t + cPosition (dx, dy);
					if (!spotOk (model, player, world, kIdGunAA, c, reserved, walls)) continue;
					const long long key = dist2 (c, t) * 100000 + offs (c);
					if (key < bestKey)
					{
						bestKey = key;
						best = c;
					}
				}
			if (best.x() >= 0) return best;
		}
		return cPosition (-1, -1);
	};
	// стена: первая открытая клетка кольца здания из списка (по порядку списка и кольца)
	auto wallSpot = [&] (size_t from, size_t to, std::string& what) -> cPosition {
		for (size_t i = from; i < to && i < wallTargets_.size(); ++i)
		{
			const auto& [q, big] = wallTargets_[i];
			for (const auto& c : ring (q, big))
			{
				if (!map.isValidPosition (c) || solidBuilding (map, c)) continue;
				if (std::find (walls.begin(), walls.end(), c) != walls.end()) continue;
				if (!spotOk (model, player, world, kIdBlock, c, reserved, walls)) continue;
				for (const cBuilding* b : map.getField (q).getBuildings())
					if (b->getOwner() == &player && b->getStaticUnitData().surfacePosition == eSurfacePosition::Ground)
						what = b->getStaticUnitData().getDefaultName();
				return c;
			}
		}
		return cPosition (-1, -1);
	};
	const size_t alarmWalls = commandoAlarm() ? std::min (alarmSites_.size(), wallTargets_.size()) : 0;
	std::string what;
	// 1) тревога «диверсант»: стены вокруг выключенного здания
	if (alarmWalls > 0)
		if (const cPosition c = wallSpot (0, alarmWalls, what); c.x() >= 0) return {kIdBlock, c, "wall: commando alarm at " + what};
	if (level_ <= 0) return none;
	// 2) первый радар
	if (radars.empty() && radarsWanted_ >= 1)
		if (const cPosition c = radarSpot(); c.x() >= 0) return {kIdRadar, c, "radar 1 of " + std::to_string (radarsWanted_)};
	// 3) авиация врага видна — кольцо ПВО раньше прочего
	if (airAlarm_ && static_cast<int> (guns.size()) < aaWanted_)
		if (const cPosition c = aaSpot (aaWanted_); c.x() >= 0) return {kIdGunAA, c, "aa ring (enemy air seen)"};
	// 4) остальные радары
	if (static_cast<int> (radars.size()) < radarsWanted_)
		if (const cPosition c = radarSpot(); c.x() >= 0)
			return {kIdRadar, c, "radar " + std::to_string (radars.size() + 1) + " of " + std::to_string (radarsWanted_)};
	// 5) стены вокруг электростанций (и прочих целей уровня) — по порядку списка
	// 6) кольцо ПВО заранее
	if (static_cast<int> (guns.size()) < aaWanted_)
		if (const cPosition c = aaSpot (aaWanted_); c.x() >= 0)
			return {kIdGunAA, c, "aa ring " + std::to_string (guns.size() + 1) + " of " + std::to_string (aaWanted_)};
	// П9: соединители кольцом — обход точки отказа подбазы; на стороне врага — стеной (вдвое крепче, но не
	// пропускает машины: только если проход остаётся), дальше — соединителем. Со стенами у зданий — через ход
	// (иначе обход не дождётся очереди: стен у ключевых зданий — десятки)
	auto ringLink = [&] () -> sBuild {
		if (linkCell_.x() < 0 || std::find (walls.begin(), walls.end(), linkCell_) != walls.end()) return none;
		for (const auto& [q, big] : reserved)
			if (inFootprint (linkCell_, q, big)) return none;
		for (const auto& [id, p] : planned)
			if (p == linkCell_) return none;
		const cPosition rel = linkCell_ - center_;
		const bool front = static_cast<long long> (rel.x()) * frontDir_.x() + static_cast<long long> (rel.y()) * frontDir_.y() > 0;
		const std::string cut = std::to_string (linkCut_.x()) + "," + std::to_string (linkCut_.y());
		if (front && spotOk (model, player, world, kIdBlock, linkCell_, reserved, walls))
			return {kIdBlock, linkCell_, "ring link (wall, front side) around cut point " + cut};
		if (world.groundThreat (linkCell_) == 0) return {sID (1, 4), linkCell_, "ring link (connector) around cut point " + cut};
		return none;
	};
	if (turn_ % 2 == 0)
		if (const sBuild r = ringLink(); r.valid()) return r;
	if (const cPosition c = wallSpot (alarmWalls, wallTargets_.size(), what); c.x() >= 0) return {kIdBlock, c, "wall around " + what};
	if (const sBuild r = ringLink(); r.valid()) return r;
	return none;
}

//------------------------------------------------------------------------------
void cBaseDefense::updatePosts (const cModel& model, const cPlayer& player, const cRoleBook& roles)
{
	const cMap& map = *model.getMap();
	posts_.clear();
	const int cap = level_ >= 3 ? kGuardsCap : (commandoAlarm() ? kGuardsCapAlarm : 0);
	if (cap <= 0)
	{
		postOf_.clear();
		return;
	}
	// обзор пехоты (текущая версия) — радиус охраны
	int scan = 4;
	if (const auto* d = player.getLastUnitData (kIdInfantry)) scan = std::max (1, d->getScan());
	const long long scan2 = static_cast<long long> (scan) * scan;
	// ключевые здания: электростанции, шахты, фабрики, радары, ПВО-пушки, исследования, учебный центр, депо
	struct sKey
	{
		cPosition q;
		bool big;
		bool alarm;
	};
	std::vector<sKey> keys;
	for (const auto& b : player.getBuildings())
	{
		const auto& sd = b->getStaticUnitData();
		if (sd.surfacePosition != eSurfacePosition::Ground || sd.ID == kIdBlock) continue;
		const bool key = sd.produceEnergy > 0 || sd.ID == kIdEnergyBig || sd.ID == kIdEnergySmall || sd.buildingData.canMineMaxRes > 0
			|| !sd.canBuild.empty() || sd.ID == kIdRadar || sd.ID == kIdGunAA || sd.buildingData.canResearch || sd.storageUnitsMax > 0;
		if (!key) continue;
		bool alarm = b->isDisabled();
		for (const auto& r : recentDisables_)
			if (r.first == b->getPosition()) alarm = true;
		keys.push_back ({b->getPosition(), b->getIsBig(), alarm});
	}
	// здание прикрыто постом g: все клетки его кольца — в обзоре
	auto covers = [&] (const cPosition& g, const sKey& k) {
		for (const auto& c : ring (k.q, k.big))
			if (dist2 (g, c) > scan2) return false;
		return true;
	};
	auto postFree = [&] (const cPosition& c) {
		if (!map.isValidPosition (c) || map.isWater (c) || map.isBlocked (c) || solidBuilding (map, c) || exitCell (c)) return false;
		return true;
	};
	std::vector<char> covered (keys.size(), 0);
	// жадно: пост, прикрывающий больше всего неприкрытых зданий (тревожные — вдвое ценнее)
	while (static_cast<int> (posts_.size()) < cap)
	{
		cPosition best (-1, -1);
		long long bestKey = std::numeric_limits<long long>::min();
		std::vector<cPosition> cands;
		for (size_t i = 0; i < keys.size(); ++i)
		{
			if (covered[i]) continue;
			const int s = keys[i].big ? 2 : 1;
			for (int dy = -2; dy < s + 2; ++dy)
				for (int dx = -2; dx < s + 2; ++dx)
				{
					const cPosition c = keys[i].q + cPosition (dx, dy);
					if (inFootprint (c, keys[i].q, keys[i].big) || !postFree (c)) continue;
					if (std::find (cands.begin(), cands.end(), c) == cands.end()) cands.push_back (c);
				}
		}
		for (const auto& c : cands)
		{
			bool used = false;
			for (const auto& p : posts_)
				if (cheb (p.post, c) <= 1) used = true;
			if (used) continue;
			long long gain = 0;
			for (size_t i = 0; i < keys.size(); ++i)
				if (!covered[i] && covers (c, keys[i])) gain += keys[i].alarm ? 2 : 1;
			if (gain <= 0) continue;
			const long long key = gain * 1000000000LL - dist2 (c, center_) * 100000 - (static_cast<long long> (c.y()) * W_ + c.x());
			if (key > bestKey)
			{
				bestKey = key;
				best = c;
			}
		}
		if (best.x() < 0) break;
		sPost p;
		p.post = best;
		for (size_t i = 0; i < keys.size(); ++i)
			if (!covered[i] && covers (best, keys[i]))
			{
				covered[i] = 1;
				if (keys[i].alarm) p.search = true;
			}
		// клетка проверки: соседняя свободная (по порядку направлений)
		for (const auto& d : kRingDirs)
		{
			const cPosition c = best + d;
			if (postFree (c))
			{
				p.patrol = c;
				break;
			}
		}
		posts_.push_back (p);
	}
	// раздача постов: прежний пост — если он ещё в списке; остальным — ближайший свободный (по id)
	std::vector<std::pair<unsigned int, const cVehicle*>> guards;
	for (const auto& v : player.getVehicles())
		if (roles.roleOf (*v) == eRole::Guard && !v->isUnitLoaded()) guards.emplace_back (v->getId(), v.get());
	std::map<unsigned int, int> next;
	std::vector<char> taken (posts_.size(), 0);
	std::map<unsigned int, cPosition> prevCell;
	for (const auto& [id, idx] : postOf_)
		if (idx >= 0 && idx < static_cast<int> (prevPostCells_.size())) prevCell[id] = prevPostCells_[idx];
	for (const auto& [id, v] : guards)
	{
		const auto it = prevCell.find (id);
		if (it == prevCell.end()) continue;
		for (size_t i = 0; i < posts_.size(); ++i)
			if (!taken[i] && posts_[i].post == it->second)
			{
				taken[i] = 1;
				next[id] = static_cast<int> (i);
				break;
			}
	}
	for (const auto& [id, v] : guards)
	{
		if (next.count (id)) continue;
		int best = -1;
		long long bestD = std::numeric_limits<long long>::max();
		for (size_t i = 0; i < posts_.size(); ++i)
		{
			if (taken[i]) continue;
			const long long d = dist2 (v->getPosition(), posts_[i].post);
			if (d < bestD)
			{
				bestD = d;
				best = static_cast<int> (i);
			}
		}
		if (best < 0) break;
		taken[best] = 1;
		next[id] = best;
	}
	postOf_.swap (next);
	prevPostCells_.clear();
	for (const auto& p : posts_) prevPostCells_.push_back (p.post);
}

//------------------------------------------------------------------------------
cBaseDefense::sPost cBaseDefense::postOf (unsigned int id) const
{
	const auto it = postOf_.find (id);
	if (it == postOf_.end() || it->second < 0 || it->second >= static_cast<int> (posts_.size())) return {};
	return posts_[it->second];
}

//------------------------------------------------------------------------------
bool cBaseDefense::patrolTurn (unsigned int id) const
{
	const sPost p = postOf (id);
	if (p.search) return true;
	return (static_cast<unsigned int> (turn_) + id) % 4 == 0;
}

//------------------------------------------------------------------------------
void cBaseDefense::updateMineField (const cModel& model, const cPlayer& player, const cWorldModel& world)
{
	mineField_.clear();
	const cMap& map = *model.getMap();
	// откуда ждать врага: здания врага на нашей суше (память), иначе — середина карты
	cPosition target (W_ / 2, H_ / 2);
	{
		long long sx = 0, sy = 0, n = 0;
		for (const auto& kv : world.enemies())
			if (kv.second.building && kv.second.owner >= 0 && world.tankRegionOf (kv.second.pos) == map_.landRegion)
			{
				sx += kv.second.pos.x();
				sy += kv.second.pos.y();
				++n;
			}
		if (n > 0) target = cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n));
	}
	frontDir_ = target - center_; // П9: стены обхода — на стороне врага
	if (minelayersWanted_ <= 0) return;
	const cPosition d = target - center_;
	const long long dd = static_cast<long long> (d.x()) * d.x() + static_cast<long long> (d.y()) * d.y();
	if (dd == 0) return;
	// всего своих мин на карте — не больше поля (база растёт, кольцо сдвигается — мины не множить)
	int mines = 0;
	for (const auto& b : player.getBuildings())
		if (b->getStaticUnitData().ID == kIdLandMine) ++mines;
	if (mines >= kMineFieldCells) return;
	std::vector<std::tuple<int, long long, long long, cPosition>> cands; // кольцо, |поперёк|, смещение
	// за площадками зданий базы (правила ставят их не дальше 14 клеток от высадки): мина не даёт строить
	const cPosition home = player.getLandingPos();
	const int r0 = std::max (radius_ + 3, kMineMinFromLanding - cheb (home, center_)), r1 = r0 + 2;
	for (int dy = -r1; dy <= r1; ++dy)
		for (int dx = -r1; dx <= r1; ++dx)
		{
			const int r = std::max (std::abs (dx), std::abs (dy));
			if (r < r0) continue;
			const cPosition c = center_ + cPosition (dx, dy);
			if (!map.isValidPosition (c) || map.isWater (c) || map.isCoast (c) || map.isBlocked (c)) continue;
			if (cheb (c, home) < kMineMinFromLanding) continue;
			if (world.tankRegionOf (c) != map_.landRegion) continue;
			const long long dot = static_cast<long long> (dx) * d.x() + static_cast<long long> (dy) * d.y();
			const long long len2 = static_cast<long long> (dx) * dx + static_cast<long long> (dy) * dy;
			// конус ±45° к врагу: dot > 0 и dot² ≥ ½·|c|²·|d|²
			if (dot <= 0 || 2 * dot * dot < len2 * dd) continue;
			if (!map.getField (c).getBuildings().empty() && !hasOwnLandMine (map, player, c)) continue;
			if (exitCell (c)) continue;
			if (explored (map, player, c, eResourceType::Metal) >= 2 || explored (map, player, c, eResourceType::Oil) >= 3) continue;
			const long long cross = std::abs (static_cast<long long> (dx) * d.y() - static_cast<long long> (dy) * d.x());
			cands.emplace_back (r, cross, static_cast<long long> (c.y()) * W_ + c.x(), c);
		}
	std::sort (cands.begin(), cands.end(), [] (const auto& a, const auto& b) {
		if (std::get<1> (a) != std::get<1> (b)) return std::get<1> (a) < std::get<1> (b);
		if (std::get<0> (a) != std::get<0> (b)) return std::get<0> (a) < std::get<0> (b);
		return std::get<2> (a) < std::get<2> (b);
	});
	int laid = mines;
	for (const auto& t : cands)
	{
		if (laid + static_cast<int> (mineField_.size()) >= kMineFieldCells) break;
		const cPosition c = std::get<3> (t);
		if (!hasOwnLandMine (map, player, c)) mineField_.push_back (c);
	}
}

//------------------------------------------------------------------------------
std::pair<cPosition, bool> cBaseDefense::alarmSite (const cModel& model, const cPlayer& player) const
{
	(void) model;
	for (auto it = recentDisables_.rbegin(); it != recentDisables_.rend(); ++it)
		for (const auto& b : player.getBuildings())
			if (b->getPosition() == it->first) return {it->first, b->getIsBig()};
	return {cPosition (-1, -1), false};
}

//------------------------------------------------------------------------------
bool cBaseDefense::airRaidNear (const cWorldModel& world, const cPosition& p, int r) const
{
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.plane || !m.armed() || (m.canAttack & (eTerrainFlag::Ground | eTerrainFlag::Sea)) == 0) continue;
		if (world.age (m) > 2) continue;
		if (dist2 (m.pos, p) <= static_cast<long long> (r) * r) return true;
	}
	return false;
}

//------------------------------------------------------------------------------
std::vector<nlohmann::json> cBaseDefense::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

//------------------------------------------------------------------------------
nlohmann::json cBaseDefense::toJson() const
{
	nlohmann::json posts = nlohmann::json::array();
	for (const auto& p : posts_)
		posts.push_back ({p.post.x(), p.post.y(), p.search ? 1 : 0});
	return {{"defenseLevel", level_}, {"airAlarm", airAlarm_}, {"commandoAlarm", commandoAlarm()},
		{"center", {{"x", center_.x()}, {"y", center_.y()}}}, {"radius", radius_},
		{"radars", counts_.radars}, {"radarsWanted", radarsWanted_}, {"aa", counts_.aa}, {"aaWanted", aaWanted_},
		{"walls", counts_.walls}, {"wallsOpen", counts_.wallsOpen}, {"wallTargets", wallTargets_.size()},
		{"guards", counts_.guards}, {"posts", posts}, {"trainingHall", wantTrainingHall_}, {"mineField", mineField_.size()},
		{"cutPoints", counts_.cutPoints}, {"ringLink", {linkCell_.x(), linkCell_.y()}},
		{"disabled", counts_.disabled}, {"map", map_.toJson()}};
}

} // namespace ai::base3
