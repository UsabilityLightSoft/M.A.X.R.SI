/***************************************************************************
 * ai_navy.cpp — флот ИИИ (П11–П14, шаг 13 PLAN.md, часть 2). Описание — в заголовке.
 ***************************************************************************/

#include "ai_navy.h"

#include "ai_army_ops.h"
#include "ai_defense.h"
#include "ai_roles.h"
#include "ai_world_model.h"

#include "game/data/base/base.h"
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
#include <cstdlib>
#include <tuple>

namespace ai
{

namespace
{

const sID kIdShipyard (1, 12); // верфь: 2×2 на воде, строит корабли
const sID kIdTank (0, 32);     // эталон наземного бойца — куда встанет груз

constexpr int kSeaNear = 20;        // до большого моря от базы не дальше — флот возможен (как П3)
constexpr int kDockHpPct = 60;      // корабль с hp ниже — в док
constexpr int kRetreatHpPct = 45;   // без дока — к своей верфи
constexpr int kCargoMax = 6;        // мест в морском транспорте
constexpr int kBoardTurns = 8;      // ждать груз у причала — не дольше (если в транспорте уже 3)
constexpr int kHomeKeep = 2;        // свободных бойцов дома — не в десант
constexpr int kEscortValue = 60;    // груз с транспортом дороже (металл) — без охраны не плыть (П14)
constexpr int kPickupRadius = 24;   // причал — не дальше от центра базы (Чебышёв)
constexpr int kCargoReach = 25;     // бойцы в десант — не дальше от причала
constexpr int kBeachRadius = 30;    // берег высадки — не дальше от цели
constexpr int kBeachRing = 10;      // у известных зданий врага — высаживаться в стольких клетках (не в их обороне)
constexpr int kStatusEvery = 10;
constexpr int kEnemySubFresh = 20;  // подлодка врага в памяти не старше — угроза
constexpr int kGunboatReach = 7;    // дальность канонерки — «цель у нашей воды»
constexpr int kSpotRange = 5;       // корректировщик (подлодка, обзор 7) — не дальше от цели обстрела
constexpr int kCorvetteGuardR = 4;  // корвет охраны — не дальше от охраняемого (Чебышёв)
constexpr int kYardIncome = 30;     // упор «море»: верфь — с добычи металла в ход (экономика сначала)
constexpr int kYardTurn = 220;      // или с этого хода (бедная сторона — экономика важнее)
constexpr int kReconWaves = 1;      // разведка высадкой (один разведчик) — один раз за партию
constexpr int kTankWaves = 2;       // волн десанта бойцами — не больше за партию
constexpr int kSoftRadius = 10;     // «мягкая» цель высаженных: вооружённых врагов не ближе (клеток)

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

// путь до хода: столько клеток, сколько машина пройдёт за ход (как в правилах)
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

bool isLand (const cMap& map, const cPosition& p)
{
	return map.isValidPosition (p) && !map.isWater (p) && !map.isCoast (p) && !map.isBlocked (p);
}

int hpPct (const cVehicle& v)
{
	const int m = v.data.getHitpointsMax();
	return m > 0 ? v.data.getHitpoints() * 100 / m : 100;
}

// кольцо клеток вокруг здания (с углами)
std::vector<cPosition> ringOf (const cPosition& q, bool big)
{
	const int s = big ? 2 : 1;
	std::vector<cPosition> r;
	for (int dy = -1; dy <= s; ++dy)
		for (int dx = -1; dx <= s; ++dx)
			if (dx < 0 || dy < 0 || dx >= s || dy >= s) r.push_back (q + cPosition (dx, dy));
	return r;
}

// расстояние² от клетки до ближайшей клетки юнита из памяти (здание 2×2 — до ближайшей из четырёх)
long long distToMem (const cPosition& c, const sEnemyMemory& m)
{
	long long best = dist2 (c, m.pos);
	if (m.big)
		for (const cPosition d : {cPosition (1, 0), cPosition (0, 1), cPosition (1, 1)})
			best = std::min (best, dist2 (c, m.pos + d));
	return best;
}

std::shared_ptr<const cPlayer> sharedPlayer (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id) return p;
	return nullptr;
}

const char* landName (int s)
{
	static constexpr const char* names[5] = {"idle", "board", "sail", "unload", "return"};
	return s >= 0 && s < 5 ? names[s] : "?";
}

} // namespace

//------------------------------------------------------------------------------
const char* cNavy::modeName() const
{
	switch (mode_)
	{
		case eMode::Coast: return "coast";
		case eMode::Sea: return "sea";
		default: return "none";
	}
}

const char* cNavy::landingStateName() const
{
	return landName (static_cast<int> (landing_.state));
}

//------------------------------------------------------------------------------
void cNavy::event (const char* what, nlohmann::json extra)
{
	nlohmann::json e = {{"event", "navy"}, {"level", "operational"}, {"what", what}, {"turn", turn_}};
	if (extra.is_object())
		for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
	events_.push_back (std::move (e));
}

void cNavy::setLanding (eLand s, const char* why, nlohmann::json extra)
{
	nlohmann::json e = {{"from", landName (static_cast<int> (landing_.state))}, {"to", landName (static_cast<int> (s))}, {"why", why},
		{"transport", landing_.transport}, {"cargo", landing_.cargo.size()}};
	if (extra.is_object())
		for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
	landing_.state = s;
	landing_.since = turn_;
	event ("landing", std::move (e));
}

std::vector<nlohmann::json> cNavy::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

//------------------------------------------------------------------------------
void cNavy::noteShipyardSite (const cPosition& p)
{
	if (guardPoint_.x() >= 0 || p.x() < 0) return;
	guardPoint_ = p;
	guardsWanted_ = 2;
}

void cNavy::buildThreats (const cModel& model, const cWorldModel& world)
{
	const cMap& map = *model.getMap();
	const auto& ud = *model.getUnitsData();
	if (landDist_.size() != static_cast<size_t> (W_) * H_)
	{
		// расстояние до суши (Чебышёв) — местность статична
		landDist_.assign (static_cast<size_t> (W_) * H_, 16);
		std::vector<int> q;
		for (int y = 0; y < H_; ++y)
			for (int x = 0; x < W_; ++x)
				if (isLand (map, cPosition (x, y)))
				{
					landDist_[static_cast<size_t> (y) * W_ + x] = 0;
					q.push_back (y * W_ + x);
				}
		for (size_t i = 0; i < q.size(); ++i)
		{
			const int cx = q[i] % W_, cy = q[i] / W_;
			const int d = landDist_[q[i]];
			if (d >= 15) continue;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int nx = cx + dx, ny = cy + dy;
					if (nx < 0 || ny < 0 || nx >= W_ || ny >= H_) continue;
					const size_t o = static_cast<size_t> (ny) * W_ + nx;
					if (landDist_[o] <= d + 1) continue;
					landDist_[o] = d + 1;
					q.push_back (static_cast<int> (o));
				}
		}
	}
	threats_.clear();
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.hitsGround()) continue;
		if (!m.building && world.age (m) > 2) continue; // машина — видна сейчас или прошлым ходом (как зона огня правил)
		const sID id (m.typeFirst, m.typeSecond);
		sThreat t;
		t.pos = m.pos;
		t.big = m.big;
		t.landBound = !m.building && !m.ship && !m.plane && ud.isValidId (id) && ud.getStaticUnitData (id).factorSea <= 0;
		t.fire = m.range;
		t.reach = m.range + (m.building ? 0 : m.moveFire);
		threats_.push_back (t);
	}
}

int cNavy::seaDanger (const cPosition& c) const
{
	const int toLand = (c.x() >= 0 && c.y() >= 0 && c.x() < W_ && c.y() < H_ && !landDist_.empty()) ? landDist_[static_cast<size_t> (c.y()) * W_ + c.x()] : 0;
	int danger = 0;
	for (const sThreat& t : threats_)
	{
		if (t.landBound && toLand > t.fire) continue; // с суши не достать
		long long d2 = dist2 (c, t.pos);
		if (t.big)
			for (const cPosition d : {cPosition (1, 0), cPosition (0, 1), cPosition (1, 1)})
				d2 = std::min (d2, dist2 (c, t.pos + d));
		if (d2 <= static_cast<long long> (t.fire) * t.fire) return 2;
		if (d2 <= static_cast<long long> (t.reach) * t.reach) danger = 1;
	}
	return danger;
}

bool cNavy::reservedCell (const cPosition& p) const
{
	return std::find (reserved_.begin(), reserved_.end(), p) != reserved_.end();
}

//------------------------------------------------------------------------------
void cNavy::update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, bool rich, int income, int turn)
{
	turn_ = turn;
	playerId_ = player.getId();
	const cMap& map = *model.getMap();
	W_ = map.getSize().x();
	H_ = map.getSize().y();
	const auto& mp = defense.map();
	homeRegion_ = mp.landRegion;
	homeSea_ = defense.bigSea();
	baseCenter_ = defense.baseCenter();
	const auto& ud = *model.getUnitsData();

	// свои верфи и доки
	shipyards_.clear();
	docks_.clear();
	yardPos_ = cPosition (-1, -1);
	for (const auto& b : player.getBuildings())
	{
		const auto& sd = b->getStaticUnitData();
		if (sd.canBuild == "Ship")
		{
			shipyards_.push_back (b->getId());
			if (yardPos_.x() < 0) yardPos_ = b->getPosition();
		}
		else if (sd.storageUnitsMax > 0 && std::find (sd.storeUnitsTypes.begin(), sd.storeUnitsTypes.end(), "Ship") != sd.storeUnitsTypes.end())
			docks_.push_back (b->getId());
	}
	// корабли по ролям
	const int landed = counts_.landed, waves = counts_.waves, cargo = counts_.cargo, recon = counts_.reconWaves, tank = counts_.tankWaves;
	counts_ = {};
	counts_.reconWaves = recon;
	counts_.tankWaves = tank;
	counts_.landed = landed;
	counts_.waves = waves;
	counts_.cargo = cargo;
	for (const auto& v : player.getVehicles())
	{
		const eRole r = roles.roleOf (*v);
		if (!isNavalRole (r)) continue;
		switch (r)
		{
			case eRole::Corvette: ++counts_.corvettes; break;
			case eRole::NavalGun: ++counts_.guns; break;
			case eRole::Submarine: ++counts_.subs; break;
			case eRole::SeaTransport: ++counts_.transports; break;
			default: ++counts_.other; break;
		}
		counts_.fleetMetal += v->data.getBuildCost();
	}
	counts_.shipyards = static_cast<int> (shipyards_.size());
	counts_.docks = static_cast<int> (docks_.size());

	// угроза с моря и цели у нашей воды (память — честно, только виденное)
	enemySubs_ = 0;
	enemyShipMetal_ = defense.enemyShipMetal();
	coastTargets_ = 0;
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (m.owner < 0) continue;
		const sID id (m.typeFirst, m.typeSecond);
		if (!ud.isValidId (id)) continue;
		const auto& sd = ud.getStaticUnitData (id);
		if (!m.building)
		{
			if (m.ship && (sd.isStealthOn & eTerrainFlag::Sea) != 0 && world.age (m) <= kEnemySubFresh) ++enemySubs_;
			continue;
		}
		if (homeSea_ < 0 || ud.getDynamicUnitData (id).getBuildCost() <= 2) continue;
		bool reach = false;
		for (int dy = -kGunboatReach; dy <= kGunboatReach + (m.big ? 1 : 0) && !reach; ++dy)
			for (int dx = -kGunboatReach; dx <= kGunboatReach + (m.big ? 1 : 0) && !reach; ++dx)
				if (defense.seaLabelOf (m.pos + cPosition (dx, dy)) == homeSea_) reach = true;
		if (reach) ++coastTargets_;
	}

	// где искать врага (разведка подлодок): середина виденных зданий врага; не видели — точка напротив нашей базы
	// (карты 1 на 1 симметричны — догадка, пишется в журнал при смене)
	{
		long long sx = 0, sy = 0, n = 0;
		for (const auto& kv : world.enemies())
			if (kv.second.building && kv.second.owner >= 0)
			{
				sx += kv.second.pos.x();
				sy += kv.second.pos.y();
				++n;
			}
		const bool known = n > 0;
		enemyFocus_ = known ? cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n)) : cPosition (W_ - 1 - baseCenter_.x(), H_ - 1 - baseCenter_.y());
		if (known != enemyFocusKnown_ && mode_ != eMode::None)
			event ("enemy focus", {{"pos", {enemyFocus_.x(), enemyFocus_.y()}}, {"known", known},
				{"why", known ? "centre of the seen enemy buildings" : "enemy not seen: the point opposite our base (1v1 maps are symmetric)"}});
		enemyFocusKnown_ = known;
	}
	// программа флота (простой вариант; сколько металла во флот — потом решает Модель)
	const bool seaNear = homeSea_ >= 0 && mp.seaDistance >= 0 && mp.seaDistance <= kSeaNear;
	const std::string emph = mp.emphasis;
	eMode m = eMode::None;
	std::string why;
	if (!seaNear)
		why = "no big sea within 20 cells of the base";
	else if (emph == "sea")
	{
		m = eMode::Sea;
		why = mp.sure ? "enemy only beyond water: fleet and landing" : "guess (enemy not seen): our land is small, the sea is near";
	}
	else if (enemyShipMetal_ > 0 || defense.enemyShipyardSeen())
	{
		m = eMode::Coast;
		why = "enemy warships or shipyard seen";
	}
	else if (!shipyards_.empty())
	{
		m = eMode::Coast;
		why = "own shipyard: keep a coast guard";
	}
	// верфь без цели (без кораблей врага и зданий у нашей воды) — металл впустую: квик-турнир против base3 на Delta
	// и Donuts — верфь «из лишнего металла» строилась по одному ходу «склад ≥ 80%»
	else if (coastTargets_ > 0 && defense.level() >= 2 && (rich || (mode_ == eMode::Coast && turn_ - coastSince_ < 20)))
	{
		if (mode_ != eMode::Coast) coastSince_ = turn_;
		m = eMode::Coast;
		why = "spare metal, enemy buildings within gunboat range of our sea";
	}
	else
		why = "land emphasis, no naval threat, no spare metal";
	// опыт «вариант против варианта» (шаг 13, ч. 2): MAXR_NAVY=0 — флот выключен (программа «none»), остальное — как есть
	if (const char* off = std::getenv ("MAXR_NAVY"); off != nullptr && off[0] == '0')
	{
		m = eMode::None;
		why = "MAXR_NAVY=0";
	}
	if (m != mode_)
	{
		event ("program", {{"from", modeName()}, {"to", m == eMode::Sea ? "sea" : (m == eMode::Coast ? "coast" : "none")}, {"why", why},
			{"emphasis", emph}, {"sure", mp.sure}, {"seaDistance", mp.seaDistance}, {"coastTargets", coastTargets_},
			{"enemyShipMetal", enemyShipMetal_}, {"enemyShipyard", defense.enemyShipyardSeen()}, {"enemySubs", enemySubs_}});
		mode_ = m;
	}
	modeWhy_ = why;
	// верфь — когда экономика поднялась: враг за водой нас тоже не достанет, спешить некуда, а металл ранних
	// ходов — шахтам (по доходу — от 20, иначе с хода 100; «берег» — уже из лишнего металла)
	wantShipyard_ = mode_ != eMode::None && shipyards_.empty()
		&& (mode_ == eMode::Coast || income >= kYardIncome || turn_ >= kYardTurn || enemyShipMetal_ > 0);
	const int armed = counts_.corvettes + counts_.guns + counts_.subs;
	bool damaged = false;
	for (const auto& v : player.getVehicles())
		if (isNavalRole (roles.roleOf (*v)) && v->data.getHitpoints() * 100 < v->data.getHitpointsMax() * kDockHpPct) damaged = true;
	wantDock_ = !shipyards_.empty() && docks_.empty() && armed + counts_.transports >= 4 && damaged;

	// П13: охрана стройки верфи — конструктор строит верфь (или оперативка её запланирует — noteShipyardSite)
	guardPoint_ = cPosition (-1, -1);
	guardsWanted_ = 0;
	for (const auto& v : player.getVehicles())
		if (v->isUnitBuildingABuilding() && v->getBuildingType() == kIdShipyard)
		{
			guardPoint_ = v->getPosition();
			guardsWanted_ = 2;
			break;
		}
	if (turn_ % kStatusEvery == 0 && (mode_ != eMode::None || counts_.fleetMetal > 0)) event ("status", toJson());
}

//------------------------------------------------------------------------------
sID cNavy::nextShip (const cModel& model, const cPlayer& player, const cRoleBook& book, cComposition& comp, const std::string& buildAs)
{
	if (mode_ == eMode::None) return sID();
	const auto& ud = *model.getUnitsData();
	auto typesOf = [&] (eRole r) {
		std::vector<sID> t;
		for (const auto& sd : ud.getStaticUnitsData())
			if (sd.ID.isAVehicle() && !sd.isAlien && sd.buildAs == buildAs && book.roleOfType (sd.ID) == r) t.push_back (sd.ID);
		return t;
	};
	struct sStep
	{
		eRole role;
		int want;
		const char* why;
	};
	std::vector<sStep> steps;
	// ответ на угрозу с моря — раньше программы
	if (enemySubs_ > 0) steps.push_back ({eRole::Corvette, std::min (4, 1 + enemySubs_), "enemy subs seen: only corvettes see them"});
	if (enemyShipMetal_ > 0) steps.push_back ({eRole::NavalGun, std::min (6, 2 + enemyShipMetal_ / 36), "enemy warships seen"});
	if (mode_ == eMode::Sea)
	{
		// сначала — охрана верфи и разведка (найти врага), потом охрана транспорта и транспорт; канонерок больше —
		// только когда есть что обстреливать (здания врага у нашей воды): флот без целей — металл впустую
		steps.push_back ({eRole::Corvette, 1, "guard the shipyard: only corvettes see subs"});
		steps.push_back ({eRole::Submarine, 1, "hidden scout: find the enemy, spot for gunboats"});
		steps.push_back ({eRole::NavalGun, 2, "escort the transport, bombard the shore"});
		steps.push_back ({eRole::SeaTransport, 1, "landing on the enemy land (6 vehicles)"});
		if (coastTargets_ > 0)
		{
			steps.push_back ({eRole::Corvette, 2, "escort the transport against subs"});
			steps.push_back ({eRole::NavalGun, 4, "enemy buildings within gunboat range of our sea: bombard"});
			steps.push_back ({eRole::Submarine, 2, "spot for gunboats, hunt enemy ships"});
			steps.push_back ({eRole::NavalGun, 6, "enemy buildings within gunboat range of our sea: bombard"});
		}
	}
	else
	{
		steps.push_back ({eRole::Corvette, 1, "coast guard: only corvettes see subs"});
		if (coastTargets_ > 0)
		{
			steps.push_back ({eRole::Submarine, 1, "spotter for gunboats (a gunboat sees 4, hits 7)"});
			steps.push_back ({eRole::NavalGun, 2, "enemy buildings within gunboat range of our sea"});
		}
	}
	nlohmann::json options = nlohmann::json::array();
	for (const auto& s : steps) options.push_back ({roleName (s.role), comp.count (s.role), s.want});
	for (const auto& s : steps)
	{
		if (comp.count (s.role) >= s.want) continue;
		auto types = typesOf (s.role);
		if (types.empty()) continue;
		sID pick = types.front();
		std::string why = s.why;
		if (s.role == eRole::NavalGun && types.size() >= 2)
		{
			// броня и два выстрела (канонерка) — основа; дальнобойный (ракетный корабль) — каждый третий: бьёт
			// турели (6) и артиллерию (8) издали, но не ракетные пушки (11)
			auto rangeOf = [&] (const sID& id) {
				const auto* d = player.getLastUnitData (id);
				return d ? d->getRange() : 0;
			};
			std::stable_sort (types.begin(), types.end(), [&] (const sID& a, const sID& b) { return rangeOf (a) < rangeOf (b); });
			const sID shortT = types.front(), longT = types.back();
			const int all = comp.count (eRole::NavalGun);
			if (comp.countType (longT) < (all + 1) / 3)
			{
				pick = longT;
				why += "; every third — long range";
			}
			else
				pick = shortT;
		}
		comp.noteQueued (book, pick);
		event ("build ship", {{"type", ud.getStaticUnitData (pick).getDefaultName()}, {"role", roleName (s.role)}, {"why", why},
			{"mode", modeName()}, {"options", options}}); // [роль, есть (с очередью), нужно]
		return pick;
	}
	return sID();
}

//------------------------------------------------------------------------------
bool cNavy::pushPath (const cVehicle& v, const cMapView& view, std::vector<cPosition> path, const char* reason, std::vector<cOrder>& out,
	unsigned int target)
{
	cutToTurn (v, view, path);
	while (!path.empty() && reservedCell (path.back())) path.pop_back();
	if (path.empty()) return false;
	cOrder o;
	o.type = eOrderType::Move;
	o.playerId = playerId_;
	o.unitId = v.getId();
	o.path = std::move (path);
	o.position = o.path.back();
	o.unitId2 = target;
	o.level = "micro";
	o.reason = std::string ("navy: ") + reason + (target != kNoUnit ? " +fire" : "");
	reserved_.push_back (o.position);
	out.push_back (std::move (o));
	return true;
}

bool cNavy::moveTo (const cVehicle& v, const cMapView& view, const cWorldModel& world, const cPosition& dest, eThreatLayer layer,
	const char* reason, std::vector<cOrder>& out)
{
	if (dest.x() < 0 || dest == v.getPosition()) return false;
	auto path = world.safePath (v, view, dest, layer);
	if (path.empty()) return false;
	return pushPath (v, view, std::move (path), reason, out);
}

cPosition cNavy::freeNear (const cMapView& view, const cWorldModel& world, const cVehicle& v, const cPosition& anchor, int rmin, int rmax,
	int maxDanger) const
{
	const int vr = world.regionOf (v, v.getPosition());
	cPosition best (-1, -1);
	long long bestKey = std::numeric_limits<long long>::max();
	for (int r = rmin; r <= rmax; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
				const cPosition c = anchor + cPosition (dx, dy);
				if (!view.isValidPosition (c)) continue;
				if (c != v.getPosition() && !view.possiblePlace (v, c)) continue;
				if (reservedCell (c)) continue;
				if (vr >= 0 && world.regionOf (v, c) != vr) continue;
				const bool ship = v.getStaticUnitData().factorGround <= 0;
				const int danger = ship ? seaDanger (c) : (world.groundFire (c) > 0 ? 2 : (world.groundStrike (c) > 0 ? 1 : 0));
				if (danger > maxDanger) continue;
				const long long key = dist2 (c, v.getPosition()) * 1000 + static_cast<long long> (c.y()) * W_ + c.x();
				if (key < bestKey)
				{
					bestKey = key;
					best = c;
				}
			}
	return best;
}

//------------------------------------------------------------------------------
cPosition cNavy::choosePickup (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
	const cBaseDefense& defense, const cVehicle& t, const cPosition& near) const
{
	const cMap& map = *model.getMap();
	const auto& tankData = model.getUnitsData()->getStaticUnitData (kIdTank);
	const int tr = world.regionOf (t, t.getPosition());
	cPosition best (-1, -1);
	long long bestKey = std::numeric_limits<long long>::max();
	for (int dy = -kPickupRadius; dy <= kPickupRadius; ++dy)
		for (int dx = -kPickupRadius; dx <= kPickupRadius; ++dx)
		{
			const cPosition p = near + cPosition (dx, dy);
			if (!map.isValidPosition (p) || (!map.isWater (p) && !map.isCoast (p))) continue;
			if (p != t.getPosition() && !view.possiblePlace (t, p)) continue;
			if (world.regionOf (t, p) != tr || defense.exitCell (p)) continue;
			// не у зданий базы: груз ждёт транспорт у причала — у фабрик он заставил бы выезды (Gen_Archipelago 43)
			bool nearBase = false;
			for (int ey = -3; ey <= 3 && !nearBase; ++ey)
				for (int ex = -3; ex <= 3 && !nearBase; ++ex)
				{
					const cPosition n = p + cPosition (ex, ey);
					if (!map.isValidPosition (n)) continue;
					for (const cBuilding* b : map.getField (n).getBuildings())
						if (b->getOwner() == &player && b->getStaticUnitData().surfacePosition == eSurfacePosition::Ground) nearBase = true;
				}
			if (nearBase) continue;
			int landN = 0;
			for (int ey = -1; ey <= 1; ++ey)
				for (int ex = -1; ex <= 1; ++ex)
				{
					const cPosition n = p + cPosition (ex, ey);
					if ((ex == 0 && ey == 0) || !isLand (map, n) || world.tankRegionOf (n) != homeRegion_) continue;
					if (defense.exitCell (n)) continue;
					if (map.possiblePlaceVehicle (tankData, n, &player) || map.getField (n).getVehicle() != nullptr) ++landN;
				}
			if (landN < 2) continue;
			const long long key = dist2 (p, near) * 100 - landN;
			if (key < bestKey)
			{
				bestKey = key;
				best = p;
			}
		}
	return best;
}

//------------------------------------------------------------------------------
bool cNavy::chooseBeach (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& t, bool recon)
{
	const cMap& map = *model.getMap();
	const auto& tankData = model.getUnitsData()->getStaticUnitData (kIdTank);
	// область цели: где больше зданий врага (память) не на нашей суше; иначе — самая большая чужая (от 150 клеток)
	std::map<int, std::tuple<int, long long, long long>> byRegion; // область → (зданий, Σx, Σy)
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.building || m.owner < 0) continue;
		int r = world.tankRegionOf (m.pos);
		if (r < 0 && m.big)
			for (const cPosition d : {cPosition (1, 0), cPosition (0, 1), cPosition (1, 1)})
				if (r < 0) r = world.tankRegionOf (m.pos + d);
		if (r < 0 || r == homeRegion_) continue;
		auto& e = byRegion[r];
		++std::get<0> (e);
		std::get<1> (e) += m.pos.x();
		std::get<2> (e) += m.pos.y();
	}
	int region = -1;
	int bestN = 0;
	bool known = false;
	cPosition target (-1, -1);
	for (const auto& [r, e] : byRegion)
		if (std::get<0> (e) > bestN)
		{
			bestN = std::get<0> (e);
			region = r;
			target = cPosition (static_cast<int> (std::get<1> (e) / bestN), static_cast<int> (std::get<2> (e) / bestN));
			known = true;
		}
	// врага ещё не видели: бойцов вслепую не высаживать (шаг 13, ч. 2: десант на «ближайший берег» Gen_Continents
	// растерялся по одному) — только разведку: суша (от 150 клеток, не наша) у фокуса врага — точки напротив нас
	if (region < 0)
	{
		if (!recon) return false;
		long long bd = std::numeric_limits<long long>::max();
		for (int y = 0; y < H_; ++y)
			for (int x = 0; x < W_; ++x)
			{
				const cPosition p (x, y);
				const int r = world.tankRegionOf (p);
				if (r < 0 || r == homeRegion_ || world.tankRegionSize (r) < 150) continue;
				const long long d = dist2 (p, enemyFocus_);
				if (d < bd)
				{
					bd = d;
					target = p;
					region = r;
				}
			}
		if (region < 0) return false;
	}
	if (target.x() < 0) return false;
	const int tr = world.regionOf (t, t.getPosition());
	// сила груза и вооружённые машины врага на суше (память не старше 8 ходов): у берега враг сильнее груза — опасно
	long long ourDmg = 0, ourHp = 0;
	int enemyArmor = 0;
	std::vector<const sEnemyMemory*> foes;
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (m.building || m.plane || m.ship || !m.hitsGround() || world.age (m) > 20) continue; // армия за туманом не исчезает
		foes.push_back (&m);
		enemyArmor = std::max (enemyArmor, m.armor);
	}
	int ourArmor = 0;
	for (const cVehicle* s : t.storedUnits)
	{
		ourDmg += static_cast<long long> (std::max (1, s->data.getDamage() - enemyArmor)) * s->data.getShotsMax();
		ourHp += s->data.getHitpoints();
		ourArmor = std::max (ourArmor, s->data.getArmor());
	}
	auto contested = [&] (const cPosition& p) {
		long long dmg = 0, hp = 0;
		for (const sEnemyMemory* m : foes)
			if (dist2 (m->pos, p) <= 12LL * 12)
			{
				dmg += m->damageVs (ourArmor);
				hp += m->hp;
			}
		return dmg * hp > 0 && 3 * dmg * hp >= 2 * ourDmg * ourHp;
	};
	// берег давно не видели (меньше половины суши в 7 клетках — за 3 хода): врага у него не знаем — опасно, пока
	// не посмотрит подлодка или разведчик (шаг 13, ч. 2: десант «вне памяти врага» на Gen_Archipelago гиб у его армии)
	auto stale = [&] (const cPosition& p) {
		int land = 0, seen = 0;
		for (int dy = -7; dy <= 7; ++dy)
			for (int dx = -7; dx <= 7; ++dx)
			{
				const cPosition c = p + cPosition (dx, dy);
				if (!isLand (map, c)) continue;
				++land;
				if (world.seenTurn (c) >= turn_ - 3) ++seen;
			}
		return land > 0 && seen * 2 < land;
	};
	cPosition best (-1, -1);
	long long bestKey = std::numeric_limits<long long>::max();
	int candidates = 0;
	int bestDanger = 0;
	bool bestStale = false;
	for (int dy = -kBeachRadius; dy <= kBeachRadius; ++dy)
		for (int dx = -kBeachRadius; dx <= kBeachRadius; ++dx)
		{
			const cPosition p = target + cPosition (dx, dy);
			if (!map.isValidPosition (p) || (!map.isWater (p) && !map.isCoast (p))) continue;
			if (p != t.getPosition() && !view.possiblePlace (t, p)) continue;
			if (world.regionOf (t, p) != tr) continue;
			int landN = 0;
			for (int ey = -1; ey <= 1; ++ey)
				for (int ex = -1; ex <= 1; ++ex)
				{
					const cPosition n = p + cPosition (ex, ey);
					if ((ex == 0 && ey == 0) || !isLand (map, n) || world.tankRegionOf (n) != region) continue;
					if (map.possiblePlaceVehicle (tankData, n, &player)) ++landN;
				}
			if (landN < 2) continue;
			++candidates;
			// зона огня — 2; досягаемость врага за его ход (дальность + ход) или враг у берега сильнее груза — 1
			int danger = world.groundFire (p) > 0 ? 2 : ((world.groundThreat (p) > 0 || world.groundStrike (p) > 0 || contested (p)) ? 1 : 0);
			bool isStale = false;
			if (danger == 0 && !recon && stale (p))
			{
				danger = 1;
				isStale = true;
			}
			const long long d = dist2 (p, target);
			// у известной базы врага — в kBeachRing клетках (не в её обороне); иначе — ближе к цели
			const long long near = known ? std::abs (d - static_cast<long long> (kBeachRing) * kBeachRing) : d;
			const long long key = danger * 1000000000000LL + near * 1000 - landN;
			if (key < bestKey)
			{
				bestKey = key;
				best = p;
				bestDanger = danger;
				bestStale = isStale;
			}
		}
	if (best.x() < 0) return false;
	const bool same = landing_.beach == best;
	landing_.beachDanger = bestDanger;
	landing_.beachStale = bestStale;
	landing_.beach = best;
	landing_.target = target;
	landing_.targetRegion = region;
	landing_.beachFails = 0;
	if (same) return true; // тот же берег — без повтора в журнале
	event ("beach", {{"beach", {best.x(), best.y()}}, {"target", {target.x(), target.y()}}, {"region", region},
		{"regionCells", world.tankRegionSize (region)}, {"enemyKnown", known}, {"enemyBuildings", bestN}, {"candidates", candidates},
		{"danger", bestDanger}, {"stale", bestStale}, {"foes", foes.size()},
		{"recon", recon}, {"why", known ? "beach near the enemy buildings, out of their fire" : "enemy not seen: recon by scouts at the land opposite our base"}});
	return true;
}

//------------------------------------------------------------------------------
void cNavy::noteUnloadSpot (const cPosition& p)
{
	if (std::find (unloadSpots_.begin(), unloadSpots_.end(), p) != unloadSpots_.end()) return;
	unloadSpots_.push_back (p);
	if (unloadSpots_.size() > 4) unloadSpots_.erase (unloadSpots_.begin());
}

//------------------------------------------------------------------------------
int cNavy::unload (const cModel& model, const cVehicle& t, std::vector<cOrder>& out, const char* why)
{
	const cMap& map = *model.getMap();
	std::vector<const cVehicle*> stored (t.storedUnits.begin(), t.storedUnits.end());
	std::sort (stored.begin(), stored.end(), [] (const cVehicle* a, const cVehicle* b) { return a->getId() < b->getId(); });
	std::vector<cPosition> used;
	int n = 0;
	for (const cVehicle* u : stored)
	{
		for (int dy = -1; dy <= 1; ++dy)
		{
			bool done = false;
			for (int dx = -1; dx <= 1 && !done; ++dx)
			{
				const cPosition c = t.getPosition() + cPosition (dx, dy);
				if ((dx == 0 && dy == 0) || !isLand (map, c)) continue;
				if (std::find (used.begin(), used.end(), c) != used.end() || reservedCell (c)) continue;
				if (!t.canExitTo (c, map, u->getStaticUnitData())) continue;
				cOrder o;
				o.type = eOrderType::ActivateUnit;
				o.playerId = playerId_;
				o.unitId = t.getId();
				o.unitId2 = u->getId();
				o.position = c;
				o.level = "operational";
				o.reason = std::string ("navy: ") + why;
				out.push_back (std::move (o));
				used.push_back (c);
				reserved_.push_back (c);
				++n;
				done = true;
			}
			if (done) break;
		}
	}
	return n;
}

//------------------------------------------------------------------------------
// у транспорта — суша области цели
static bool besideRegion (const cMap& map, const cWorldModel& world, const cPosition& p, int region)
{
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
		{
			const cPosition n = p + cPosition (dx, dy);
			if ((dx == 0 && dy == 0) || !isLand (map, n)) continue;
			if (region < 0 || world.tankRegionOf (n) == region) return true;
		}
	return false;
}

//------------------------------------------------------------------------------
void cNavy::landingPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, const std::vector<const cVehicle*>& transports, const std::vector<unsigned int>& busy,
	const std::vector<const cVehicle*>& armyFree, int escortsReady, std::vector<cOrder>& out)
{
	const cMap& map = *model.getMap();
	auto isBusy = [&] (unsigned int id) { return std::find (busy.begin(), busy.end(), id) != busy.end(); };
	const cVehicle* t = nullptr;
	for (const cVehicle* v : transports)
		if (v->getId() == landing_.transport) t = v;
	if (t == nullptr)
	{
		if (landing_.transport != 0)
			event ("landing", {{"from", landingStateName()}, {"to", "idle"}, {"why", "transport lost"}, {"transport", landing_.transport},
				{"cargo", landing_.cargo.size()}});
		landing_ = sLanding();
		if (!transports.empty())
		{
			t = transports.front();
			landing_.transport = t->getId();
		}
	}
	counts_.cargo = 0;
	if (t == nullptr) return;
	// груз: в транспорте и выбранные живые на своей суше (погибшие и ушедшие с суши — из списка)
	{
		std::vector<unsigned int> keep;
		for (unsigned int id : landing_.cargo)
		{
			const cVehicle* u = model.getVehicleFromID (id);
			if (u == nullptr || u->getOwner() != &player) continue;
			const bool aboard = std::find (t->storedUnits.begin(), t->storedUnits.end(), u) != t->storedUnits.end();
			if (!aboard && (u->isUnitLoaded() || world.tankRegionOf (u->getPosition()) != homeRegion_)) continue;
			keep.push_back (id);
		}
		for (const cVehicle* s : t->storedUnits)
			if (std::find (keep.begin(), keep.end(), s->getId()) == keep.end()) keep.push_back (s->getId());
		std::sort (keep.begin(), keep.end());
		landing_.cargo.swap (keep);
	}
	for (unsigned int id : landing_.cargo) claimed_.insert (id);
	counts_.cargo = static_cast<int> (landing_.cargo.size());
	const int aboard = static_cast<int> (t->storedUnits.size());
	int value = t->data.getBuildCost();
	nlohmann::json cargoTypes = nlohmann::json::array();
	for (const cVehicle* s : t->storedUnits)
	{
		value += s->data.getBuildCost();
		cargoTypes.push_back (s->getStaticUnitData().getDefaultName());
	}
	const cPosition home = yardPos_.x() >= 0 ? yardPos_ : baseCenter_;

	if (mode_ != eMode::Sea)
	{
		// высадка не нужна: транспорт — у верфи; кто не сел — свободен
		if (landing_.state != eLand::Idle) setLanding (eLand::Idle, "fleet program is not 'sea'");
		landing_.cargo.clear();
		for (const cVehicle* s : t->storedUnits) landing_.cargo.push_back (s->getId());
		if (!isBusy (t->getId()) && cheb (t->getPosition(), home) > 6)
			moveTo (*t, view, world, freeNear (view, world, *t, home, 2, 6, 0), eThreatLayer::Fire, "sea transport waits at the shipyard", out);
		return;
	}
	if (landing_.state == eLand::Idle) setLanding (eLand::Board, "fleet program 'sea': gather a landing force");

	switch (landing_.state)
	{
		case eLand::Board:
		{
			if (landing_.pickup.x() < 0 || (t->getPosition() != landing_.pickup && (turn_ - landing_.since) % 10 == 9))
			{
				landing_.pickup = choosePickup (model, player, view, world, defense, *t, rally_.x() >= 0 ? rally_ : baseCenter_);
				if (landing_.pickup.x() >= 0) event ("pickup", {{"pickup", {landing_.pickup.x(), landing_.pickup.y()}}});
			}
			if (landing_.pickup.x() < 0) break;
			const bool atPickup = t->getPosition() == landing_.pickup;
			if (!atPickup && !isBusy (t->getId()))
				if (!moveTo (*t, view, world, landing_.pickup, eThreatLayer::Fire, "sea transport to the pickup", out))
					landing_.pickup = cPosition (-1, -1); // не доплыть — другой причал в следующем ходу
			// волна: врага не видели — разведка (до 2 разведчиков, обзор 9: найти базу врага), иначе — бойцы
			if (aboard == 0) landing_.recon = !enemyFocusKnown_;
			if (landing_.recon && counts_.reconWaves >= kReconWaves && aboard == 0)
			{
				// разведка высадкой уже была — врага так и не нашли: разведчиков не тратить, ищут подлодки
				landing_.cargo.clear();
				break;
			}
			if (landing_.recon)
			{
				std::vector<std::tuple<long long, unsigned int>> cands;
				for (const auto& vp : player.getVehicles())
				{
					const cVehicle& u = *vp;
					if (roles.roleOf (u) != eRole::Scout || u.isUnitLoaded() || u.isDisabled() || u.isUnitMoving()) continue;
					if (claimed_.count (u.getId()) || isBusy (u.getId()) || hpPct (u) < 80) continue;
					if (world.tankRegionOf (u.getPosition()) != homeRegion_) continue; // разведчик быстр — с любого места своей суши
					cands.emplace_back (dist2 (u.getPosition(), landing_.pickup), u.getId());
				}
				// разведчиков нет — разведку ведут 2 танка (дешёвые) от причала
				if (cands.empty())
					for (const cVehicle* u : armyFree)
					{
						if (claimed_.count (u->getId()) || isBusy (u->getId()) || hpPct (*u) < 80 || roles.roleOf (*u) != eRole::Tank) continue;
						if (cheb (u->getPosition(), landing_.pickup) > kCargoReach) continue;
						cands.emplace_back (dist2 (u->getPosition(), landing_.pickup) + 1000000LL, u->getId());
					}
				std::sort (cands.begin(), cands.end());
				for (size_t i = 0; i < cands.size() && landing_.cargo.size() < 1; ++i)
				{
					landing_.cargo.push_back (std::get<1> (cands[i]));
					claimed_.insert (std::get<1> (cands[i]));
				}
				std::sort (landing_.cargo.begin(), landing_.cargo.end());
			}
			// бойцов уже возили kTankWaves раз — больше не возить (против армии у базы врага десант гибнет без толку)
			if (!landing_.recon && counts_.tankWaves >= kTankWaves && aboard == 0)
			{
				landing_.cargo.clear();
				break;
			}
			// груз: свободные бойцы на своей суше, почти целые, ближе к причалу; дома остаются kHomeKeep
			if (!landing_.recon)
			{
				int want = kCargoMax - static_cast<int> (landing_.cargo.size());
				const int spare = static_cast<int> (armyFree.size()) - kHomeKeep;
				want = std::min (want, spare);
				std::vector<std::tuple<int, long long, unsigned int, const cVehicle*>> cands;
				for (const cVehicle* u : armyFree)
				{
					if (claimed_.count (u->getId()) || isBusy (u->getId()) || hpPct (*u) < 80) continue;
					if (cheb (u->getPosition(), landing_.pickup) > kCargoReach) continue;
					const eRole r = roles.roleOf (*u);
					const int pri = (r == eRole::Tank || r == eRole::Assault) ? 0 : 1;
					cands.emplace_back (pri, dist2 (u->getPosition(), landing_.pickup), u->getId(), u);
				}
				std::sort (cands.begin(), cands.end(), [] (const auto& a, const auto& b) {
					return std::tie (std::get<0> (a), std::get<1> (a), std::get<2> (a)) < std::tie (std::get<0> (b), std::get<1> (b), std::get<2> (b));
				});
				for (size_t i = 0; i < cands.size() && static_cast<int> (i) < want; ++i)
				{
					landing_.cargo.push_back (std::get<2> (cands[i]));
					claimed_.insert (std::get<2> (cands[i]));
				}
				std::sort (landing_.cargo.begin(), landing_.cargo.end());
			}
			// к транспорту: рядом — транспорт берёт сам; иначе — к клетке суши у причала (в конце пути — войти)
			for (unsigned int id : landing_.cargo)
			{
				const cVehicle* u = model.getVehicleFromID (id);
				if (u == nullptr || u->isUnitLoaded() || isBusy (id) || u->isUnitMoving()) continue;
				if (atPickup && t->isNextTo (u->getPosition()) && t->canLoad (u))
				{
					cOrder o;
					o.type = eOrderType::Load;
					o.playerId = playerId_;
					o.unitId = t->getId();
					o.unitId2 = id;
					o.level = "operational";
					o.reason = "navy: sea transport takes " + u->getStaticUnitData().getDefaultName() + " aboard";
					out.push_back (std::move (o));
					continue;
				}
				cPosition best (-1, -1);
				long long bestD = std::numeric_limits<long long>::max();
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const cPosition c = landing_.pickup + cPosition (dx, dy);
						if ((dx == 0 && dy == 0) || !isLand (map, c) || world.tankRegionOf (c) != homeRegion_ || defense.exitCell (c)) continue;
						if (c != u->getPosition() && (!view.possiblePlace (*u, c) || reservedCell (c))) continue;
						const long long d = dist2 (c, u->getPosition());
						if (d < bestD)
						{
							bestD = d;
							best = c;
						}
					}
				if (best.x() < 0 || best == u->getPosition()) continue; // ждать транспорт у причала
				auto path = world.safePath (*u, view, best, eThreatLayer::Fire);
				if (path.empty()) continue;
				const size_t full = path.size();
				cutToTurn (*u, view, path);
				const bool getIn = atPickup && path.size() == full;
				if (pushPath (*u, view, std::move (path), getIn ? "aboard the sea transport" : "to the sea transport pickup", out, getIn ? t->getId() : kNoUnit) && getIn)
				{
					out.back().moveEnd = eMoveEnd::GetIn;
					out.back().reason = "navy: aboard the sea transport";
				}
			}
			// отплыть: полный транспорт; или в нём ≥ 3 и ждали kBoardTurns; или больше брать некого
			const int waited = turn_ - landing_.since;
			const bool full = aboard >= kCargoMax;
			const bool go = full || (aboard >= 3 && waited >= kBoardTurns)
				|| (aboard >= 1 && static_cast<int> (landing_.cargo.size()) == aboard && waited >= 2 * kBoardTurns)
				|| (landing_.recon && aboard >= 1);
			if (!go) break;
			if (value >= kEscortValue && escortsReady == 0)
			{
				if (!landing_.escortWaitNoted)
					event ("landing", {{"what", "wait for escort"}, {"value", value}, {"why", "the loaded transport is worth its cargo: no armed ship to escort it"}});
				landing_.escortWaitNoted = true;
				break;
			}
			if (!chooseBeach (model, player, view, world, *t, landing_.recon))
			{
				if (waited % 10 == 0) event ("landing", {{"what", "no beach"}, {"why", "no coast of other land reachable by the transport"}});
				break;
			}
			// груз ценится как сумма (П14): плыть — только к берегу вне досягаемости врага, где он не сильнее груза
			if (landing_.beachDanger > 0)
			{
				if (turn_ - landing_.waitNoted >= 10 && !landing_.beachStale)
				{
					event ("landing", {{"what", "wait for a safe beach"}, {"value", value}, {"danger", landing_.beachDanger},
						{"beach", {landing_.beach.x(), landing_.beach.y()}}, {"why", "every beach is in the enemy reach or a stronger enemy is near: gunboats clear the coast first"}});
					landing_.waitNoted = turn_;
				}
				break;
			}
			if (landing_.recon) ++counts_.reconWaves;
			else ++counts_.tankWaves;
			setLanding (eLand::Sail, landing_.recon ? "recon: a scout looks for the enemy base" : (full ? "transport full" : "cargo waited long enough"),
				{{"aboard", aboard}, {"types", cargoTypes}, {"value", value}, {"escorts", escortsReady},
					{"beach", {landing_.beach.x(), landing_.beach.y()}}, {"targetRegion", landing_.targetRegion}});
			break;
		}
		case eLand::Sail:
		{
			if (aboard == 0)
			{
				setLanding (eLand::Return, "cargo gone");
				break;
			}
			if (hpPct (*t) < 50)
			{
				setLanding (eLand::Return, "transport damaged: back home with the cargo", {{"hp", t->data.getHitpoints()}});
				break;
			}
			if (besideRegion (map, world, t->getPosition(), landing_.targetRegion))
			{
				setLanding (eLand::Unload, "at the beach");
				break;
			}
			if (landing_.beach.x() < 0 || landing_.beachFails >= 3 || (turn_ - landing_.since) % 8 == 7 || world.groundThreat (landing_.beach) > 0
				|| world.groundFire (landing_.beach) > 0)
				if (!chooseBeach (model, player, view, world, *t, landing_.recon)) break;
			if (landing_.beachDanger > 0)
			{
				setLanding (eLand::Return, "the beach became dangerous: back with the cargo", {{"danger", landing_.beachDanger}});
				break;
			}
			if (isBusy (t->getId())) break;
			auto path = world.safePath (*t, view, landing_.beach, eThreatLayer::Reach);
			if (path.empty()) path = toVec (cPathCalculator (*t, view, landing_.beach, false).calcPath());
			const size_t full = path.size();
			cutToTurn (*t, view, path);
			if (path.empty())
			{
				++landing_.beachFails;
				break;
			}
			const bool arrives = path.size() == full && landing_.beachDanger == 0;
			// не доплывает в этот ход (или берег стал опасен) — встать на последней клетке пути вне досягаемости врага
			if (!arrives)
				while (!path.empty() && (world.groundThreat (path.back()) > 0 || world.groundFire (path.back()) > 0)) path.pop_back();
			if (path.empty()) break; // ждать вне досягаемости
			landing_.moveEnd = path.back();
			if (pushPath (*t, view, std::move (path), "sea transport sails to the beach", out))
			{
				out.back().reason += " (cargo " + std::to_string (aboard) + ", value " + std::to_string (value) + ")";
				if (arrives) wantPass_ = true; // доплывёт в этом заходе — выгрузка в следующем
			}
			break;
		}
		case eLand::Return:
		{
			if (landing_.pickup.x() < 0) landing_.pickup = choosePickup (model, player, view, world, defense, *t, rally_.x() >= 0 ? rally_ : baseCenter_);
			if (landing_.pickup.x() >= 0 && cheb (t->getPosition(), landing_.pickup) <= 1)
			{
				landing_.cargo.clear();
				for (const cVehicle* s : t->storedUnits) landing_.cargo.push_back (s->getId());
				landing_.escortWaitNoted = false;
				setLanding (eLand::Board, "back at the pickup: next wave");
				break;
			}
			if (!isBusy (t->getId())) moveTo (*t, view, world, landing_.pickup, eThreatLayer::Fire, "sea transport returns for the next wave", out);
			break;
		}
		default: break;
	}
	// выгрузка — в этом же заходе, если транспорт уже у берега
	if (landing_.state == eLand::Unload)
	{
		const int n = unload (model, *t, out, "unload on the enemy land");
		if (n > 0)
		{
			counts_.landed += n;
			landing_.lastUnload = turn_;
			noteUnloadSpot (t->getPosition());
			wantPass_ = true;
			event ("landing", {{"what", "unload"}, {"vehicles", n}, {"at", {t->getPosition().x(), t->getPosition().y()}},
				{"targetRegion", landing_.targetRegion}});
		}
		if (t->storedUnits.empty() || static_cast<int> (t->storedUnits.size()) == n
			|| (n == 0 && turn_ - std::max (landing_.since, landing_.lastUnload) >= 2))
		{
			++counts_.waves;
			landing_.cargo.clear();
			setLanding (eLand::Return, n > 0 || t->storedUnits.empty() ? "cargo landed" : "cannot unload here");
		}
		// высаженные — не груз: армия своей суши их не берёт (у них другая суша), микро — по общим правилам
		for (const cVehicle* s : t->storedUnits) claimed_.insert (s->getId());
	}
}

//------------------------------------------------------------------------------
void cNavy::plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cBaseDefense& defense, const std::vector<unsigned int>& busyIn, const std::vector<const cVehicle*>& armyFree,
	std::vector<cOrder>& out)
{
	claimed_.clear();
	reserved_.clear();
	wantPass_ = false;
	const cMap& map = *model.getMap();
	const auto& ud = *model.getUnitsData();
	std::vector<unsigned int> busy = busyIn;
	buildThreats (model, world);
	auto isBusy = [&] (unsigned int id) { return std::find (busy.begin(), busy.end(), id) != busy.end(); };
	auto markBusy = [&] (unsigned int id) { busy.push_back (id); };
	const size_t out0 = out.size();
	std::vector<const cVehicle*> corvettes, guns, subs, transports, others;
	for (const auto& vp : player.getVehicles())
	{
		const cVehicle& v = *vp;
		const eRole r = roles.roleOf (v);
		if (!isNavalRole (r)) continue;
		claimed_.insert (v.getId());
		if (v.isUnitLoaded() || v.isDisabled()) continue;
		switch (r)
		{
			case eRole::Corvette: corvettes.push_back (&v); break;
			case eRole::NavalGun: guns.push_back (&v); break;
			case eRole::Submarine: subs.push_back (&v); break;
			case eRole::SeaTransport: transports.push_back (&v); break;
			default: others.push_back (&v); break;
		}
	}
	const cBuilding* dock = nullptr;
	for (unsigned int id : docks_)
		if (const cBuilding* b = model.getBuildingFromID (id); b != nullptr && !b->isDisabled())
		{
			dock = b;
			break;
		}
	const cPosition home = yardPos_.x() >= 0 ? yardPos_ : baseCenter_;
	// после каждого приказа флота — машина занята
	auto took = [&] (size_t before) {
		for (size_t i = before; i < out.size(); ++i) markBusy (out[i].unitId);
	};

	// 1) док (П14): внутри — ремонт за металл подбазы и выход починенных; раненые корабли — в док
	if (dock != nullptr)
	{
		int room = static_cast<int> (dock->getStaticUnitData().storageUnitsMax) - static_cast<int> (dock->storedUnits.size());
		std::vector<const cVehicle*> inside (dock->storedUnits.begin(), dock->storedUnits.end());
		std::sort (inside.begin(), inside.end(), [] (const cVehicle* a, const cVehicle* b) { return a->getId() < b->getId(); });
		const bool metal = dock->subBase != nullptr && dock->subBase->getResourcesStored().metal > 0;
		for (const cVehicle* s : inside)
		{
			claimed_.insert (s->getId());
			if (s->data.getHitpoints() < s->data.getHitpointsMax())
			{
				if (!metal) continue;
				cOrder o;
				o.type = eOrderType::Repair;
				o.playerId = playerId_;
				o.unitId = dock->getId();
				o.unitId2 = s->getId();
				o.level = "micro";
				o.reason = "navy: repair the ship in the dock";
				out.push_back (std::move (o));
				++counts_.dockOrders;
				continue;
			}
			for (const auto& c : ringOf (dock->getPosition(), dock->getIsBig()))
			{
				if (!map.isValidPosition (c) || reservedCell (c) || !dock->canExitTo (c, map, s->getStaticUnitData())) continue;
				cOrder o;
				o.type = eOrderType::ActivateUnit;
				o.playerId = playerId_;
				o.unitId = dock->getId();
				o.unitId2 = s->getId();
				o.position = c;
				o.level = "micro";
				o.reason = "navy: repaired ship leaves the dock";
				out.push_back (std::move (o));
				reserved_.push_back (c);
				++counts_.dockOrders;
				break;
			}
		}
		for (auto* group : {&corvettes, &guns, &subs, &transports, &others})
			for (const cVehicle* v : *group)
			{
				if (room <= 0 || isBusy (v->getId()) || hpPct (*v) >= kDockHpPct) continue;
				if (v->getId() == landing_.transport && !v->storedUnits.empty()) continue; // груз — сначала на берег
				if (dock->isNextTo (v->getPosition()))
				{
					if (!dock->canLoad (v)) continue;
					cOrder o;
					o.type = eOrderType::Load;
					o.playerId = playerId_;
					o.unitId = dock->getId();
					o.unitId2 = v->getId();
					o.level = "micro";
					o.reason = "navy: damaged ship into the dock";
					out.push_back (std::move (o));
					markBusy (v->getId());
					--room;
					++counts_.dockOrders;
					continue;
				}
				cPosition best (-1, -1);
				long long bestD = std::numeric_limits<long long>::max();
				for (const auto& c : ringOf (dock->getPosition(), dock->getIsBig()))
				{
					if (!view.isValidPosition (c) || !view.possiblePlace (*v, c) || reservedCell (c)) continue;
					const long long d = dist2 (c, v->getPosition());
					if (d < bestD)
					{
						bestD = d;
						best = c;
					}
				}
				const size_t b0 = out.size();
				if (moveTo (*v, view, world, best, eThreatLayer::Fire, "damaged ship to the dock", out))
				{
					took (b0);
					--room;
					++counts_.dockOrders;
				}
			}
	}

	// 2) высадка (П14): транспорт и груз
	int escortsReady = 0;
	for (const cVehicle* v : corvettes)
		if (hpPct (*v) >= kDockHpPct) ++escortsReady;
	for (const cVehicle* v : guns)
		if (hpPct (*v) >= kDockHpPct) ++escortsReady;
	{
		const size_t b0 = out.size();
		landingPlan (model, player, view, world, roles, defense, transports, busy, armyFree, escortsReady, out);
		took (b0);
	}
	const bool sailing = landing_.state == eLand::Sail || landing_.state == eLand::Unload;
	const cVehicle* transport = model.getVehicleFromID (landing_.transport);
	const cPosition escortAt = sailing && transport != nullptr ? (landing_.moveEnd.x() >= 0 && landing_.state == eLand::Sail ? landing_.moveEnd : transport->getPosition())
															  : cPosition (-1, -1);
	auto escort = [&] (const cVehicle& e, const char* why) {
		if (escortAt.x() < 0) return false;
		if (cheb (e.getPosition(), escortAt) <= 2) return true; // рядом — стоять, выстрелы на ответный огонь
		const cPosition c = freeNear (view, world, e, escortAt, 1, 3, 1);
		const size_t b0 = out.size();
		if (moveTo (e, view, world, c, eThreatLayer::Strike, why, out)) took (b0);
		return true;
	};
	// раненый без дока — к своей верфи
	auto limp = [&] (const cVehicle& v) {
		if (dock != nullptr || hpPct (v) >= kRetreatHpPct) return false;
		if (cheb (v.getPosition(), home) <= 5) return true;
		const size_t b0 = out.size();
		if (moveTo (v, view, world, freeNear (view, world, v, home, 2, 5, 0), eThreatLayer::Fire, "damaged ship back to the shipyard", out)) took (b0);
		return true;
	};

	// 3) цель обстрела (П11): здание врага (или видимая машина) у нашей воды — ценность, потом близость к верфи;
	//    вооружённые здания с дальностью не меньше нашей (ракетные пушки) — не цель: бьют первыми
	{
		auto value = [&] (const sEnemyMemory& m) -> int {
			const sID id (m.typeFirst, m.typeSecond);
			if (!ud.isValidId (id) || m.owner < 0) return 0;
			const auto& sd = ud.getStaticUnitData (id);
			if (m.building)
			{
				if (ud.getDynamicUnitData (id).getBuildCost() <= 2) return 0; // соединители, дороги, мины
				if (m.armed() && m.range >= kGunboatReach) return 0;
				if (sd.canBuild == "Ship" || std::find (sd.storeUnitsTypes.begin(), sd.storeUnitsTypes.end(), "Ship") != sd.storeUnitsTypes.end()) return 6;
				if (sd.buildingData.canMineMaxRes > 0) return 5;
				if (sd.produceEnergy > 0 || !sd.canBuild.empty()) return 4;
				if (sd.buildingData.canResearch || m.armed()) return 3;
				return 2;
			}
			return 0; // машины — не цель дальнего похода (уезжают); в дальности их бьёт фокус-огонь
		};
		// у цели есть клетка нашего моря в дальности канонерки, где враг не достанет корабль
		auto safeCell = [&] (const sEnemyMemory& m) {
			for (int dy = -kGunboatReach; dy <= kGunboatReach + (m.big ? 1 : 0); ++dy)
				for (int dx = -kGunboatReach; dx <= kGunboatReach + (m.big ? 1 : 0); ++dx)
				{
					const cPosition c = m.pos + cPosition (dx, dy);
					if (defense.seaLabelOf (c) != homeSea_ || distToMem (c, m) > static_cast<long long> (kGunboatReach) * kGunboatReach) continue;
					if (seaDanger (c) == 0) return true;
				}
			return false;
		};
		const auto it = world.enemies().find (strikeTarget_);
		if (strikeTarget_ != 0 && (it == world.enemies().end() || value (it->second) <= 0)) strikeTarget_ = 0;
		else if (strikeTarget_ != 0 && !safeCell (it->second))
		{
			if (++strikeUnsafe_ >= 3) strikeTarget_ = 0; // три хода подряд без безопасной клетки — другая цель
		}
		else
			strikeUnsafe_ = 0;
		if (strikeTarget_ == 0 && !guns.empty() && homeSea_ >= 0)
		{
			long long bestKey = std::numeric_limits<long long>::min();
			for (const auto& kv : world.enemies())
			{
				const int v = value (kv.second);
				if (v <= 0 || !safeCell (kv.second)) continue;
				const long long key = v * 100000LL - cheb (kv.second.pos, home);
				if (key > bestKey)
				{
					bestKey = key;
					strikeTarget_ = kv.first;
				}
			}
			if (strikeTarget_ != 0)
			{
				strikeUnsafe_ = 0;
				const auto& m = world.enemies().at (strikeTarget_);
				event ("strike target", {{"id", strikeTarget_}, {"type", ud.getStaticUnitData (sID (m.typeFirst, m.typeSecond)).getDefaultName()},
					{"pos", {m.pos.x(), m.pos.y()}}, {"value", value (m)}, {"why", "most valuable enemy target with a safe bombard cell in our sea (out of the enemy reach)"}});
			}
		}
		if (strikeTarget_ != 0) strikePos_ = world.enemies().at (strikeTarget_).pos;
		else strikePos_ = cPosition (-1, -1);
	}

	// 4) подлодки (П12): охота на корабли, транспорты, верфь и постройки на воде врага (выстрел раскрывает —
	//    только по ним); иначе корректировка цели обстрела; иначе разведка моря
	{
		std::vector<const sEnemyMemory*> naval;
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (m.owner < 0 || m.plane) continue;
			if (!m.building && m.ship && world.age (m) == 0) naval.push_back (&m);
			else if (m.building && world.age (m) <= 1 && (map.isWater (m.pos) || map.isCoast (m.pos))) naval.push_back (&m);
		}
		bool spotter = false;
		bool watcher = false;
		for (const cVehicle* s : subs)
		{
			if (isBusy (s->getId())) continue;
			// в дальности сейчас — выстрел (ценнее: транспорт с грузом и корабль — раньше зданий)
			if (s->data.getShots() > 0 && s->data.getAmmo() > 0)
			{
				const sEnemyMemory* pick = nullptr;
				for (const sEnemyMemory* m : naval)
					if (s->canAttackObjectAt (m->pos, view, false)
						&& (pick == nullptr || (m->building ? 1 : 0) < (pick->building ? 1 : 0)
							|| ((m->building ? 1 : 0) == (pick->building ? 1 : 0) && m->hp < pick->hp)))
						pick = m;
				if (pick != nullptr)
				{
					cOrder o;
					o.type = eOrderType::Attack;
					o.playerId = playerId_;
					o.unitId = s->getId();
					o.position = pick->pos;
					o.level = "micro";
					o.reason = "navy: sub torpedoes " + ud.getStaticUnitData (sID (pick->typeFirst, pick->typeSecond)).getDefaultName();
					out.push_back (std::move (o));
					markBusy (s->getId());
					++counts_.subOrders;
					wantPass_ = true; // второй выстрел — в следующем заходе
					continue;
				}
			}
			if (limp (*s)) continue;
			// морская цель в досягаемости хода — подойти на выстрел (скрыта: опасность зон огня — не помеха)
			{
				std::vector<const sEnemyMemory*> near;
				const int reach = s->data.getRange() + s->data.getSpeedMax() / 4 + 2;
				for (const sEnemyMemory* m : naval)
					if (distToMem (s->getPosition(), *m) <= static_cast<long long> (reach) * reach) near.push_back (m);
				if (!near.empty())
				{
					const sStandoff st = findStandoff (model, view, world, *s, near, s->data.getRange(), true, 2, reserved_);
					if (!st.path.empty())
					{
						const size_t b0 = out.size();
						if (pushPath (*s, view, st.path, "sub closes in on an enemy ship", out, st.attackId))
						{
							took (b0);
							++counts_.subOrders;
							continue;
						}
					}
				}
			}
			// берег высадки давно не видели — подлодка (скрыта, обзор 7) встаёт у него: свежая картина врага на берегу
			if (!watcher && landing_.state == eLand::Board && landing_.beachStale && landing_.beach.x() >= 0)
			{
				watcher = true;
				if (cheb (s->getPosition(), landing_.beach) <= 3) continue; // на месте — смотреть
				const cPosition c = freeNear (view, world, *s, landing_.beach, 1, 4, 2);
				const size_t b0 = out.size();
				if (moveTo (*s, view, world, c, eThreatLayer::None, "sub watches the landing beach", out))
				{
					took (b0);
					++counts_.subOrders;
					continue;
				}
			}
			// корректировщик (обзор 7, скрыта): у цели обстрела — канонеркам видно, куда бить
			if (!spotter && strikePos_.x() >= 0 && !guns.empty())
			{
				spotter = true;
				if (cheb (s->getPosition(), strikePos_) <= kSpotRange) continue; // на месте — стоять
				cPosition best (-1, -1);
				long long bestD = std::numeric_limits<long long>::max();
				const int sr = world.regionOf (*s, s->getPosition());
				for (int dy = -kSpotRange; dy <= kSpotRange; ++dy)
					for (int dx = -kSpotRange; dx <= kSpotRange; ++dx)
					{
						const cPosition c = strikePos_ + cPosition (dx, dy);
						if (!view.isValidPosition (c) || !view.possiblePlace (*s, c) || reservedCell (c) || world.regionOf (*s, c) != sr) continue;
						const long long d = dist2 (c, s->getPosition());
						if (d < bestD)
						{
							bestD = d;
							best = c;
						}
					}
				const size_t b0 = out.size();
				if (moveTo (*s, view, world, best, eThreatLayer::None, "sub spots the strike target for gunboats", out))
				{
					took (b0);
					++counts_.subOrders;
				}
				continue;
			}
			// разведка: никогда не виденная клетка воды области подлодки (сетка 3) — ближе к подлодке, потом к цели высадки
			{
				const int sr = world.regionOf (*s, s->getPosition());
				auto it = scoutGoal_.find (s->getId());
				cPosition goal = it != scoutGoal_.end() ? it->second : cPosition (-1, -1);
				if (goal.x() < 0 || world.seenTurn (goal) >= 0 || cheb (goal, s->getPosition()) <= 1)
				{
					goal = cPosition (-1, -1);
					const cPosition focus = enemyFocus_;
					long long bestKey = std::numeric_limits<long long>::max();
					for (int y = 1; y < H_; y += 3)
						for (int x = 1; x < W_; x += 3)
						{
							const cPosition c (x, y);
							if (world.seenTurn (c) >= 0 || !map.isWater (c) || world.regionOf (*s, c) != sr) continue;
							bool taken = false; // цель другой подлодки рядом — искать в другом месте
							for (const auto& [oid, og] : scoutGoal_)
								if (oid != s->getId() && cheb (og, c) <= 8) taken = true;
							if (taken) continue;
							// к врагу (фокус), из равных — ближе к подлодке
							const long long key = static_cast<long long> (cheb (c, focus)) * 4 + cheb (c, s->getPosition());
							if (key < bestKey)
							{
								bestKey = key;
								goal = c;
							}
						}
					if (goal.x() >= 0) scoutGoal_[s->getId()] = goal;
					else scoutGoal_.erase (s->getId());
				}
				if (goal.x() < 0) continue;
				const size_t b0 = out.size();
				if (moveTo (*s, view, world, goal, eThreatLayer::None, "sub scouts the sea", out))
				{
					took (b0);
					++counts_.subOrders;
				}
				else
					scoutGoal_.erase (s->getId());
			}
		}
	}

	// 5) канонерки и ракетные корабли (П11): обстрел цели с клетки в дальности вне «удара с подъезда» врага;
	//    при высадке первые две — охрана транспорта; нет цели — у верфи
	{
		int escorts = 0;
		for (const cVehicle* g : guns)
		{
			if (isBusy (g->getId())) continue;
			if (limp (*g)) continue;
			if (sailing && escorts < 2)
			{
				++escorts;
				if (escort (*g, "gunboat escorts the sea transport")) continue;
			}
			if (strikeTarget_ != 0)
			{
				const sEnemyMemory& m = world.enemies().at (strikeTarget_);
				const int range = g->data.getRange();
				const int gr = world.regionOf (*g, g->getPosition());
				cPosition best (-1, -1);
				long long bestKey = std::numeric_limits<long long>::max();
				for (int dy = -range; dy <= range + (m.big ? 1 : 0); ++dy)
					for (int dx = -range; dx <= range + (m.big ? 1 : 0); ++dx)
					{
						const cPosition c = m.pos + cPosition (dx, dy);
						if (!view.isValidPosition (c) || distToMem (c, m) > static_cast<long long> (range) * range) continue;
						if (c != g->getPosition() && (!view.possiblePlace (*g, c) || reservedCell (c))) continue;
						if (world.regionOf (*g, c) != gr) continue;
						if (seaDanger (c) > 0) continue; // канонерка не лезет под «удар с подъезда»: бьёт издали
						const long long key = static_cast<long long> (cheb (c, g->getPosition())) * 100000 + static_cast<long long> (c.y()) * W_ + c.x();
						if (key < bestKey)
						{
							bestKey = key;
							best = c;
						}
					}
				if (best == g->getPosition())
				{
					markBusy (g->getId());
					continue; // на месте: огонь — фокус-огнём правил, когда цель видна (корректирует подлодка)
				}
				if (best.x() >= 0)
				{
					auto path = world.safePath (*g, view, best, eThreatLayer::Strike);
					if (!path.empty())
					{
						cutToTurn (*g, view, path);
						const unsigned int fire = path.empty() ? kNoUnit : endMoveTarget (model, *g, path, attackableTargets (model, world, *g));
						const size_t b0 = out.size();
						if (pushPath (*g, view, std::move (path), "gunboat to the bombard cell", out, fire))
						{
							took (b0);
							++counts_.bombardOrders;
							continue;
						}
					}
				}
			}
			if (cheb (g->getPosition(), home) > 6)
			{
				const size_t b0 = out.size();
				if (moveTo (*g, view, world, freeNear (view, world, *g, home, 2, 5, 0), eThreatLayer::Fire, "gunboat guards the shipyard", out)) took (b0);
			}
		}
	}

	// 6) корветы (П13): видимая подлодка врага — бить; половина — охрана верфи и платформ (шаг через ход:
	//    обнаружение — только при шаге), прочие — охрана транспорта
	{
		std::vector<const sEnemyMemory*> subsSeen;
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (m.building || !m.ship || world.age (m) != 0) continue;
			const sID id (m.typeFirst, m.typeSecond);
			if (ud.isValidId (id) && (ud.getStaticUnitData (id).isStealthOn & eTerrainFlag::Sea) != 0) subsSeen.push_back (&m);
		}
		const int nGuard = sailing ? std::max (1, static_cast<int> (corvettes.size()) / 2) : static_cast<int> (corvettes.size());
		// что охранять: верфь; постройки на платформах (П14: подлодки бьют постройки на воде) — середина их
		std::vector<cPosition> guardAnchors {home};
		{
			long long sx = 0, sy = 0, n = 0;
			for (const auto& b : player.getBuildings())
			{
				const auto& sd = b->getStaticUnitData();
				if (sd.surfacePosition != eSurfacePosition::Ground || !sd.canBuild.empty() || sd.storageUnitsMax > 0) continue;
				if (!map.isWater (b->getPosition()) && !map.isCoast (b->getPosition())) continue;
				sx += b->getPosition().x();
				sy += b->getPosition().y();
				++n;
			}
			if (n > 0) guardAnchors.emplace_back (static_cast<int> (sx / n), static_cast<int> (sy / n));
		}
		int idx = 0;
		for (const cVehicle* c : corvettes)
		{
			const int i = idx++;
			if (isBusy (c->getId())) continue;
			if (limp (*c)) continue;
			if (!subsSeen.empty())
			{
				std::vector<const sEnemyMemory*> near;
				for (const sEnemyMemory* m : subsSeen)
					if (cheb (m->pos, c->getPosition()) <= 15) near.push_back (m);
				if (!near.empty())
				{
					const sStandoff st = findStandoff (model, view, world, *c, near, c->data.getRange(), true, 2, reserved_);
					const size_t b0 = out.size();
					if (!st.path.empty() && pushPath (*c, view, st.path, "corvette hunts the detected sub", out, st.attackId))
					{
						took (b0);
						++counts_.corvetteOrders;
						continue;
					}
				}
			}
			if (i >= nGuard && escort (*c, "corvette escorts the sea transport (subs)")) continue;
			const cPosition anchor = guardAnchors[static_cast<size_t> (i) % guardAnchors.size()];
			if (cheb (c->getPosition(), anchor) > kCorvetteGuardR)
			{
				const size_t b0 = out.size();
				if (moveTo (*c, view, world, freeNear (view, world, *c, anchor, 2, 3, 0), eThreatLayer::Fire, "corvette to guard the shipyard", out))
				{
					took (b0);
					++counts_.corvetteOrders;
				}
				continue;
			}
			if ((turn_ + static_cast<int> (c->getId())) % 2 != 0) continue; // стоять: выстрелы на ответный огонь
			// шаг на соседнюю клетку у верфи — проверка обнаружения
			static const cPosition dirs[8] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
			for (int k = 0; k < 8; ++k)
			{
				const cPosition n = c->getPosition() + dirs[(k + turn_ / 2) % 8];
				if (!view.isValidPosition (n) || !view.possiblePlace (*c, n) || reservedCell (n) || cheb (n, anchor) > kCorvetteGuardR) continue;
				if (world.groundFire (n) > 0) continue;
				const size_t b0 = out.size();
				if (pushPath (*c, view, {n}, "corvette patrol step (detects subs)", out))
				{
					took (b0);
					++counts_.corvetteOrders;
				}
				break;
			}
		}
	}

	// 7) прочие корабли — у верфи
	for (const cVehicle* o : others)
	{
		if (isBusy (o->getId()) || cheb (o->getPosition(), home) <= 6) continue;
		const size_t b0 = out.size();
		if (moveTo (*o, view, world, freeNear (view, world, *o, home, 2, 5, 0), eThreatLayer::Fire, "ship waits at the shipyard", out)) took (b0);
	}
	(void) out0;
}

//------------------------------------------------------------------------------
std::vector<cOrder> cNavy::planPass (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles, int pass)
{
	std::vector<cOrder> out;
	wantPass_ = false;
	reserved_.clear();
	const cMap& map = *model.getMap();
	const auto& ud = *model.getUnitsData();
	// транспорт доплыл в прошлом заходе — выгрузка (выгрузка не тратит ход машин)
	if (landing_.state == eLand::Sail || landing_.state == eLand::Unload)
		if (const cVehicle* t = model.getVehicleFromID (landing_.transport); t != nullptr && t->getOwner() == &player && !t->isUnitMoving()
			&& !t->storedUnits.empty() && besideRegion (map, world, t->getPosition(), landing_.targetRegion))
		{
			if (landing_.state == eLand::Sail) setLanding (eLand::Unload, "at the beach", {{"pass", pass}});
			const int n = unload (model, *t, out, "unload on the enemy land");
			if (n > 0)
			{
				counts_.landed += n;
				landing_.lastUnload = turn_;
				noteUnloadSpot (t->getPosition());
				event ("landing", {{"what", "unload"}, {"vehicles", n}, {"pass", pass}, {"at", {t->getPosition().x(), t->getPosition().y()}},
					{"targetRegion", landing_.targetRegion}});
				if (n == static_cast<int> (t->storedUnits.size()))
				{
					++counts_.waves;
					landing_.cargo.clear();
					setLanding (eLand::Return, "cargo landed", {{"pass", pass}});
				}
				wantPass_ = true;
			}
		}
	// подлодки: второй выстрел по видимому кораблю или постройке на воде врага
	const auto sp = sharedPlayer (model, player.getId());
	const cMapView view (model.getMap(), sp);
	for (const auto& vp : player.getVehicles())
	{
		const cVehicle& s = *vp;
		if (roles.roleOf (s) != eRole::Submarine || s.isUnitLoaded() || s.isDisabled() || s.isUnitMoving() || s.isAttacking()) continue;
		if (s.data.getShots() <= 0 || s.data.getAmmo() <= 0) continue;
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (m.owner < 0 || m.plane || world.age (m) != 0) continue;
			if (!(m.ship || (m.building && (map.isWater (m.pos) || map.isCoast (m.pos))))) continue;
			const cUnit* u = model.getUnitFromID (m.id);
			if (u == nullptr || !view.canSeeUnit (*u) || !s.canAttackObjectAt (u->getPosition(), view, false)) continue;
			cOrder o;
			o.type = eOrderType::Attack;
			o.playerId = player.getId();
			o.unitId = s.getId();
			o.position = u->getPosition();
			o.level = "micro";
			o.reason = std::string ("pass ") + std::to_string (pass) + ": navy: sub torpedoes " + ud.getStaticUnitData (sID (m.typeFirst, m.typeSecond)).getDefaultName();
			out.push_back (std::move (o));
			wantPass_ = true;
			break;
		}
	}
	return out;
}

//------------------------------------------------------------------------------
bool cNavy::landedGoal (const cMapView& view, const cWorldModel& world, const cVehicle& v, cPosition& goal, bool explore)
{
	const int vr = world.regionOf (v, v.getPosition());
	if (vr < 0) return false;
	// «мягкая» цель (для бойцов): здание не вооружено и вооружённых врагов рядом нет (машины — память до 8 ходов)
	auto soft = [&] (const sEnemyMemory& t) {
		if (t.armed()) return false;
		for (const auto& kv : world.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (!m.hitsGround() || m.plane || (!m.building && world.age (m) > 8)) continue;
			if (dist2 (m.pos, t.pos) <= static_cast<long long> (kSoftRadius) * kSoftRadius) return false;
		}
		return true;
	};
	// виденное здание врага на её суше — ближайшее; встать рядом (клетка, куда машина встанет)
	const sEnemyMemory* near = nullptr;
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.building || m.owner < 0) continue;
		bool reach = false;
		for (const auto& c : ringOf (m.pos, m.big))
			if (world.regionOf (v, c) == vr) reach = true;
		if (!reach || (!explore && !soft (m))) continue;
		if (near == nullptr || dist2 (m.pos, v.getPosition()) < dist2 (near->pos, v.getPosition())) near = &m;
	}
	if (near != nullptr)
	{
		goal = cPosition (-1, -1);
		long long bd = std::numeric_limits<long long>::max();
		for (int r = 1; r <= 3 && goal.x() < 0; ++r)
			for (int dy = -r; dy <= r + (near->big ? 1 : 0); ++dy)
				for (int dx = -r; dx <= r + (near->big ? 1 : 0); ++dx)
				{
					const cPosition c = near->pos + cPosition (dx, dy);
					if (!view.isValidPosition (c) || !view.possiblePlace (v, c) || world.regionOf (v, c) != vr) continue;
					const long long d = dist2 (c, v.getPosition());
					if (d < bd)
					{
						bd = d;
						goal = c;
					}
				}
		if (goal.x() >= 0) return true;
	}
	// боец без «мягкой» цели — держит берег, но не на клетках у места выгрузки: иначе следующим выйти некуда
	if (!explore)
	{
		goal = v.getPosition();
		auto nearSpot = [&] (const cPosition& c) {
			for (const cPosition& s : unloadSpots_)
				if (cheb (c, s) <= 1) return true;
			return false;
		};
		for (const cPosition& s : unloadSpots_)
		{
			if (cheb (s, v.getPosition()) > 1) continue;
			cPosition best (-1, -1);
			long long bd = std::numeric_limits<long long>::max();
			for (int dy = -3; dy <= 3; ++dy)
				for (int dx = -3; dx <= 3; ++dx)
				{
					const cPosition c = s + cPosition (dx, dy);
					if (!view.isValidPosition (c) || nearSpot (c) || world.regionOf (v, c) != vr || !view.possiblePlace (v, c)) continue;
					if (world.groundFire (c) > 0) continue;
					const long long d = dist2 (c, v.getPosition());
					if (d < bd)
					{
						bd = d;
						best = c;
					}
				}
			if (best.x() >= 0) goal = best;
			break;
		}
		return true;
	}
	// разведка: невиданная клетка её суши, ближе к фокусу врага (сетка 3), — держать, пока не увидят
	auto it = landedGoal_.find (v.getId());
	if (it != landedGoal_.end() && world.seenTurn (it->second) < 0 && it->second != v.getPosition())
	{
		goal = it->second;
		return true;
	}
	goal = cPosition (-1, -1);
	long long bestKey = std::numeric_limits<long long>::max();
	for (int y = 1; y < H_; y += 3)
		for (int x = 1; x < W_; x += 3)
		{
			const cPosition c (x, y);
			if (world.seenTurn (c) >= 0 || world.regionOf (v, c) != vr || !view.possiblePlace (v, c)) continue;
			const long long key = static_cast<long long> (cheb (c, enemyFocus_)) * 4 + cheb (c, v.getPosition());
			if (key < bestKey)
			{
				bestKey = key;
				goal = c;
			}
		}
	if (goal.x() < 0)
	{
		landedGoal_.erase (v.getId());
		return false;
	}
	landedGoal_[v.getId()] = goal;
	return true;
}

//------------------------------------------------------------------------------
nlohmann::json cNavy::toJson() const
{
	return {{"mode", modeName()}, {"why", modeWhy_}, {"corvettes", counts_.corvettes}, {"guns", counts_.guns}, {"subs", counts_.subs},
		{"transports", counts_.transports}, {"other", counts_.other}, {"fleetMetal", counts_.fleetMetal}, {"shipyards", counts_.shipyards},
		{"docks", counts_.docks}, {"landing", landingStateName()}, {"cargo", counts_.cargo}, {"landed", counts_.landed},
		{"waves", counts_.waves}, {"strikeTarget", strikeTarget_}, {"coastTargets", coastTargets_}, {"enemySubs", enemySubs_},
		{"enemyShipMetal", enemyShipMetal_}, {"beach", {landing_.beach.x(), landing_.beach.y()}}, {"targetRegion", landing_.targetRegion}};
}

} // namespace ai
