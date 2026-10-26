/***************************************************************************
 * ai_red_fair.cpp — честный налётчик «red2» (шаг 14б, Владыка 26.10.2026: «враг нашего ИИИ — люди,
 * которые хорошо знают игру»). Что и зачем — в ai_red_brain.h; тактика сценариев — общая с red1
 * (ai_red_brain.cpp), здесь — то, чего у краш-теста нет:
 *  - высадка по правилам окна высадки (windowlandingunitselection.cpp: только наземные машины без
 *    пехоты, на те же стартовые кредиты): конструктор и инженер с грузом металла, скаут, танки;
 *  - свой конструктор строит здания налёта у главной подбазы (её ведут замороженные правила base2),
 *    инженер тянет соединители, если здание встало отдельно (верфь в воде);
 *  - заводы налёта выпускают отряд сценария на металле базы, потерянных — заменяют;
 *  - разведка: база врага неизвестна — скаут и AWAC обследуют карту, пока не увидят его здания;
 *  - ангар: самолёт без боезапаса или с прочностью ниже 60% летит в ангар, перезаряжается и чинится.
 ***************************************************************************/

#include "ai_red_brain.h"

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
#include "utility/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <forward_list>
#include <limits>

namespace ai
{

namespace
{

int dist2 (const cPosition& a, const cPosition& b)
{
	return (a - b).l2NormSquared();
}

nlohmann::json posJson (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

std::vector<cPosition> toVector (const std::forward_list<cPosition>& path)
{
	return {path.begin(), path.end()};
}

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

const cStaticUnitData* dataByName (const cModel& model, const std::string& name)
{
	for (const auto& d : model.getUnitsData()->getStaticUnitsData())
		if (d.getDefaultName() == name) return &d;
	return nullptr;
}

// главная подбаза — с наибольшим числом зданий (её ведут правила base2; порядок — по первому зданию)
const cSubBase* mainSubBase (const cPlayer& player)
{
	std::vector<std::pair<const cSubBase*, int>> counts;
	for (const auto& b : player.getBuildings())
	{
		if (b->subBase == nullptr) continue;
		auto it = std::find_if (counts.begin(), counts.end(), [&] (const auto& c) { return c.first == b->subBase; });
		if (it == counts.end()) counts.emplace_back (b->subBase, 1);
		else ++it->second;
	}
	const cSubBase* best = nullptr;
	int bestN = 0;
	for (const auto& c : counts)
		if (c.second > bestN)
		{
			bestN = c.second;
			best = c.first;
		}
	return best;
}

std::vector<cPosition> footprint (const cPosition& q, bool big)
{
	if (!big) return {q};
	return {q, q + cPosition (1, 0), q + cPosition (0, 1), q + cPosition (1, 1)};
}

// соседи площадки по стороне (через них здания соединяются в подбазу)
std::vector<cPosition> sideNeighbours (const cPosition& q, bool big)
{
	const int s = big ? 2 : 1;
	std::vector<cPosition> out;
	for (int i = 0; i < s; ++i)
	{
		out.push_back (q + cPosition (i, -1));
		out.push_back (q + cPosition (i, s));
		out.push_back (q + cPosition (-1, i));
		out.push_back (q + cPosition (s, i));
	}
	return out;
}

bool hasBuildingNamed (const cMap& map, const cPosition& p, const std::string& name)
{
	for (const cBuilding* b : map.getField (p).getBuildings())
		if (b->getStaticUnitData().getDefaultName() == name) return true;
	return false;
}

// клетка выезда из здания (завод, ангар): рядом, свободна для машины, и у неё есть свободный сосед
cPosition exitCell (const cModel& model, const cBuilding& b, const cStaticUnitData& vd, const cPlayer& player)
{
	const cMap& map = *model.getMap();
	for (int r = 1; r <= 3; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				const cPosition p = b.getPosition() + cPosition (dx, dy);
				if (!map.isValidPosition (p) || !b.isNextTo (p) || !map.possiblePlaceVehicle (vd, p, &player)) continue;
				bool freeNeighbour = false;
				for (const cPosition d : {cPosition (1, 0), cPosition (-1, 0), cPosition (0, 1), cPosition (0, -1)})
					freeNeighbour = freeNeighbour || (map.isValidPosition (p + d) && map.possiblePlaceVehicle (vd, p + d, &player));
				if (freeNeighbour) return p;
			}
	return cPosition (-1, -1);
}

// конструктор закончил стройку — клетка, куда он съедет (как findVehicleEscape у base2)
cPosition builderEscape (const cModel& model, const cVehicle& v)
{
	const cMap& map = *model.getMap();
	const cMapView view (model.getMap(), nullptr);
	for (int r = 1; r <= 3; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				const cPosition p = v.getPosition() + cPosition (dx, dy);
				if (!map.isValidPosition (p) || !v.isNextTo (p) || !map.possiblePlace (v, p, false)) continue;
				bool boxed = true;
				for (const cPosition d : {cPosition (1, 0), cPosition (-1, 0), cPosition (0, 1), cPosition (0, -1)})
					boxed = boxed && !(map.isValidPosition (p + d) && view.possiblePlace (v, p + d));
				if (!boxed) return p;
			}
	return cPosition (-1, -1);
}

constexpr int kEcoReserve = 15; // металла базы, который стройщики налёта не трогают

bool isPlaneType (const std::string& t)
{
	return t == "awac" || t == "bomber" || t == "air_transport";
}

} // namespace

//------------------------------------------------------------------------------
std::vector<std::pair<std::string, int>> redLandingPurchase (eRedScenario s, bool fair)
{
	std::vector<std::pair<std::string, int>> out;
	if (!fair)
	{
		for (const auto& t : redLandingUnits (s)) out.emplace_back (t, 0);
		return out;
	}
	// окно высадки человеку: только наземные машины без пехоты (factorGround > 0, !isHuman), цена — buildCost,
	// груз — 1 кредит за 5 единиц. Конструктор 24 + 60 металла (12), инженер 18 + 40 (8), скаут 9 = 71
	out = {{"konstrukt", 60}, {"pionier", 40}, {"scout", 0}};
	if (s == eRedScenario::Combo || s == eRedScenario::Sea) // + 3 танка (36) = 107 из 150; остальное — кредиты на улучшения
		for (int i = 0; i < 3; ++i) out.emplace_back ("tank", 0);
	return out;
}

std::vector<std::string> redFairForce (eRedScenario s)
{
	switch (s)
	{
		case eRedScenario::Rear: return {"awac", "air_transport", "bomber", "bomber", "commando"};
		case eRedScenario::Sea: return {"sea_transport", "gunboat", "gunboat"};
		case eRedScenario::Subs: return {"sub", "sub", "sub"};
		default: return {"awac", "air_transport", "air_transport", "bomber", "commando"};
	}
}

std::vector<std::string> redFairBuildings (eRedScenario s)
{
	if (s == eRedScenario::Sea || s == eRedScenario::Subs) return {"shipfactory"};
	return {"airfactory", "hangar", "habitat", "training"};
}

//------------------------------------------------------------------------------
bool cRedBrain::owned (unsigned int id) const
{
	if (id == kNoUnit) return false;
	if (const auto it = raiders_.find (id); it != raiders_.end() && !it->second.lost) return true;
	return claimedBuildings_.count (id) != 0;
}

void cRedBrain::fairBegin (const cModel& model)
{
	const cPlayer* me = model.getPlayer (playerId_);
	base_ = home_;
	baseKnown_ = false;
	// купленное при высадке: по каждому типу — машины с наибольшими id (бесплатный набор высаживается первым)
	std::map<std::string, int> want;
	for (const auto& [t, cargo] : redLandingPurchase (scenario_, true)) ++want[t];
	for (const auto& [t, n] : want)
	{
		std::vector<unsigned int> ids;
		for (const auto& v : me->getVehicles())
			if (v->getStaticUnitData().getDefaultName() == t) ids.push_back (v->getId());
		std::sort (ids.rbegin(), ids.rend());
		for (int k = 0; k < n && k < static_cast<int> (ids.size()); ++k)
		{
			raiders_[ids[k]] = {ids[k], t, turn_, false};
			if (t == "konstrukt") builderId_ = ids[k];
			if (t == "pionier") engineerId_ = ids[k];
		}
	}
	for (const auto& t : redFairBuildings (scenario_)) sites_.push_back ({t});
	const int n = model.getMap()->getSize().x();
	mapN_ = n;
	explored_.assign (static_cast<std::size_t> (n) * n, 0);
}

// новые машины отряда (свои заводы налёта; base2 самолётов, кораблей и коммандос не строит) и построенные здания
void cRedBrain::claimNew (const cModel& model)
{
	const cPlayer* me = model.getPlayer (playerId_);
	if (me == nullptr) return;
	const auto force = redFairForce (scenario_);
	for (const auto& v : me->getVehicles())
	{
		const unsigned int id = v->getId();
		if (raiders_.count (id) != 0) continue;
		const std::string t = v->getStaticUnitData().getDefaultName();
		if (std::find (force.begin(), force.end(), t) == force.end()) continue;
		const bool hadCommando = own (model, "commando") != nullptr;
		raiders_[id] = {id, t, turn_, false};
		stepEvent ("produced", {{"unit", id}, {"type", t}, {"at", posJson (v->getPosition())}, {"turn", turn_}});
		if (t == "air_transport")
		{
			// место погибшего транспорта (0 — для коммандос, 1 — для танков) или новое
			bool placed = false;
			for (auto& slot : airTransports_)
				if (model.getVehicleFromID (slot) == nullptr)
				{
					slot = id;
					placed = true;
					break;
				}
			if (!placed) airTransports_.push_back (id);
			if (tankFerry_ == 9) tankFerry_ = 0;
		}
		if (t == "commando" && !hadCommando)
		{
			// новый коммандос (прежний погиб) — снова доставка авиатранспортом
			ferryState_ = 0;
			dropAt_ = cPosition (0, 0);
			cmdTriedTurn_ = -1;
		}
	}
	for (std::size_t i = 0; i < sites_.size(); ++i)
	{
		auto& s = sites_[i];
		if (s.id != kNoUnit)
		{
			if (model.getBuildingFromID (s.id) == nullptr)
			{
				stepEvent ("raid_building_lost", {{"type", s.type}, {"unit", s.id}, {"turn", turn_}});
				claimedBuildings_.erase (s.id);
				s = sRaidSite{s.type};
				if (connFor_ == static_cast<int> (i))
				{
					connPath_.clear();
					connFor_ = -1;
				}
			}
			continue;
		}
		if (!s.chosen) continue;
		for (const auto& b : me->getBuildings())
			if (b->getPosition() == s.site && b->getStaticUnitData().getDefaultName() == s.type)
			{
				s.id = b->getId();
				claimedBuildings_.insert (s.id);
				stage ("built_" + s.type, true, {{"unit", s.id}, {"at", posJson (s.site)}, {"turn", turn_}});
				break;
			}
	}
}

// --- разведка ---------------------------------------------------------------------------------
void cRedBrain::markExplored (const cModel& model, const cMapView& view)
{
	const int n = mapN_;
	if (n <= 0) return;
	(void) model;
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x)
			if (view.isPositionVisible (cPosition (x, y))) explored_[static_cast<std::size_t> (y) * n + x] = 1;
}

void cRedBrain::explore (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	markExplored (model, view);
	if (baseKnown_) return;
	const int n = mapN_;
	const cMap& map = *model.getMap();
	std::vector<const cVehicle*> scouts;
	if (const cVehicle* s = own (model, "scout")) scouts.push_back (s);
	if (const cVehicle* a = own (model, "awac"); a != nullptr && service_.count (a->getId()) == 0) scouts.push_back (a);
	// морской отряд собран, а базы не видно (скауту не доехать) — ищут сами корабли
	if (forceReady_)
		for (const std::string t : {"sub", "gunboat"})
			for (const cVehicle* s : ownAll (model, t)) scouts.push_back (s);
	const auto explored = [&] (const cPosition& p) { return explored_[static_cast<std::size_t> (p.y()) * n + p.x()] != 0; };
	for (const cVehicle* e : scouts)
	{
		if (e->isUnitMoving() || e->isUnitLoaded() || e->isDisabled() || e->data.getSpeed() <= 0) continue;
		const bool plane = e->getStaticUnitData().factorAir > 0;
		const bool ship = e->getStaticUnitData().factorSea > 0 && e->getStaticUnitData().factorGround <= 0;
		auto it = exploreTo_.find (e->getId());
		const bool keep = it != exploreTo_.end() && !explored (it->second) && dist2 (e->getPosition(), it->second) > 4;
		std::vector<cPosition> cands;
		if (keep)
			cands.push_back (it->second);
		else
		{
			// точки сетки 8×8, ещё не бывшие в обзоре; у дома — дороже (враг далеко), в сторону точки напротив
			// дома через середину карты — дешевле (догадка игрока: высадки обычно по разные стороны карты)
			const cPosition mirror (n - 1 - home_.x(), n - 1 - home_.y());
			struct sWp
			{
				double cost;
				cPosition p;
			};
			std::vector<sWp> wps;
			for (int y = 4; y < n; y += 8)
				for (int x = 4; x < n; x += 8)
				{
					const cPosition p (x, y);
					if (explored (p) || exploreBad_.count ({x, y}) != 0 || map.isBlocked (p)) continue;
					if (!plane && !ship && map.isWater (p)) continue; // скаут по воде втрое медленнее — только суша и берег
					if (ship && !map.isWater (p)) continue;
					bool taken = false;
					for (const auto& [uid, t] : exploreTo_)
						taken = taken || (uid != e->getId() && t == p);
					if (taken) continue;
					const double dHome = std::sqrt (static_cast<double> (dist2 (p, home_)));
					const double cost = std::sqrt (static_cast<double> (dist2 (p, e->getPosition()))) + 0.8 * std::max (0.0, 40.0 - dHome)
						+ 0.5 * std::sqrt (static_cast<double> (dist2 (p, mirror)));
					wps.push_back ({cost, p});
				}
			std::sort (wps.begin(), wps.end(), [] (const sWp& a, const sWp& b) {
				return a.cost != b.cost ? a.cost < b.cost : (a.p.y() != b.p.y() ? a.p.y() < b.p.y() : a.p.x() < b.p.x());
			});
			for (std::size_t i = 0; i < wps.size() && i < 6; ++i) cands.push_back (wps[i].p);
		}
		bool moved = false;
		for (const auto& p : cands)
		{
			if (moveToward (model, view, *e, p, out, std::string (e->getStaticUnitData().getDefaultName()) + " explores: where is the enemy base?", plane))
			{
				exploreTo_[e->getId()] = p;
				moved = true;
				break;
			}
			exploreBad_.insert ({p.x(), p.y()}); // не добраться
		}
		if (!moved) exploreTo_.erase (e->getId());
	}
}

// --- хозяйство налёта: конструктор, инженер, заводы -------------------------------------------
bool cRedBrain::finishIfBuilt (const cModel& model, const cVehicle& v, std::vector<cOrder>& out)
{
	if (!v.isUnitBuildingABuilding()) return false;
	if (v.getBuildTurns() == 0 && !v.bandPosition)
	{
		const cPosition esc = builderEscape (model, v);
		if (esc.x() >= 0)
		{
			cOrder o = order (eOrderType::FinishBuild, v.getId(), "raid builder: finish building");
			o.position = esc;
			out.push_back (std::move (o));
		}
	}
	return true;
}

// металл стройщику — из главной подбазы через соседнее здание; не рядом — подъехать к базе
bool cRedBrain::refill (const cModel& model, const cMapView& view, const cVehicle& v, int want, std::vector<cOrder>& out)
{
	const cPlayer* me = model.getPlayer (playerId_);
	const cSubBase* main = mainSubBase (*me);
	if (main == nullptr || want <= 0) return false;
	// хозяйству base2 оставить запас: налёт не должен уморить базу (фабрику, энергию) голодом
	const int stored = main->getResourcesStored().metal - kEcoReserve;
	for (const auto& b : me->getBuildings())
		if (b->subBase == main && b->isNextTo (v.getPosition()))
		{
			if (stored <= 0) return false; // ждать металла
			cOrder o = order (eOrderType::Transfer, b->getId(), "raid builder takes metal from the base");
			o.unitId2 = v.getId();
			o.transferValue = std::min (want, stored);
			o.resType = eResourceType::Metal;
			out.push_back (std::move (o));
			return true;
		}
	// ближайшая свободная клетка у здания главной подбазы
	const cMap& map = *model.getMap();
	cPosition best (-1, -1);
	int bestD = std::numeric_limits<int>::max();
	for (const auto& b : me->getBuildings())
	{
		if (b->subBase != main) continue;
		for (const auto& c : sideNeighbours (b->getPosition(), b->getIsBig()))
		{
			if (!map.isValidPosition (c) || !view.possiblePlace (v, c)) continue;
			const int d = dist2 (c, v.getPosition());
			if (d < bestD)
			{
				bestD = d;
				best = c;
			}
		}
	}
	return best.x() >= 0 && moveToward (model, view, v, best, out, "raid builder goes to the base for metal", false);
}

// металл можно взять прямо сейчас: рядом здание главной подбазы, на складе есть металл
bool cRedBrain::refillHere (const cModel& model, const cVehicle& v) const
{
	const cPlayer* me = model.getPlayer (playerId_);
	const cSubBase* main = mainSubBase (*me);
	// быстрая стройка за металл базы — только когда металл копится (запас хозяйства + 40)
	if (main == nullptr || main->getResourcesStored().metal < kEcoReserve + 40) return false;
	for (const auto& b : me->getBuildings())
		if (b->subBase == main && b->isNextTo (v.getPosition())) return true;
	return false;
}

// пока отряд собирается — танки налёта охраняют дом: бьют видимого врага у базы (человек не даст скауту
// врага расстрелять конструктор у трёх своих танков)
void cRedBrain::guardHome (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	const cPlayer* me = model.getPlayer (playerId_);
	std::vector<cPosition> anchors = {home_};
	for (const auto& b : me->getBuildings()) anchors.push_back (b->getPosition());
	const auto nearHome = [&] (const cPosition& p) {
		for (const auto& a : anchors)
			if (dist2 (a, p) <= 12 * 12) return true;
		return false;
	};
	for (const cVehicle* t : ownAll (model, "tank"))
	{
		if (t->isUnitLoaded() || t->isUnitMoving() || t->isDisabled()) continue;
		if (fireInRange (model, view, *t, 1, out, "guard")) continue;
		if (t->data.getSpeed() <= 0) continue;
		unsigned int best = kNoUnit;
		int bestD = std::numeric_limits<int>::max();
		for (const auto& [id, s] : seen_)
		{
			if (!s.visible || s.building || !nearHome (s.pos)) continue;
			const int d = dist2 (s.pos, t->getPosition());
			if (d < bestD)
			{
				bestD = d;
				best = id;
			}
		}
		if (best == kNoUnit) continue;
		auto path = toVector (cPathCalculator (*t, view, seen_[best].pos, true).calcPath());
		if (path.empty()) continue;
		const std::size_t full = path.size();
		cutToTurn (*t, view, path);
		while (!path.empty() && reserved_.count ({path.back().x(), path.back().y()}) != 0) path.pop_back();
		if (path.empty()) continue;
		cOrder o = order (eOrderType::Move, t->getId(), "tank guards home: " + seen_[best].type);
		o.path = path;
		o.position = path.back();
		if (path.size() == full)
			if (const cUnit* u = model.getUnitFromID (best))
			{
				o.unitId2 = best;
				o.moveEnd = eMoveEnd::Attack;
				shots_.push_back ({t->getId(), best, seen_[best].type, u->data.getHitpoints(), "guard"});
			}
		reserved_.insert ({path.back().x(), path.back().y()});
		out.push_back (std::move (o));
		wantPass_ = true;
	}
}

bool cRedBrain::chooseSite (const cModel& model, const cMapView& view, sRaidSite& s)
{
	(void) view;
	const cMap& map = *model.getMap();
	const cPlayer* me = model.getPlayer (playerId_);
	const cStaticUnitData* data = dataByName (model, s.type);
	if (data == nullptr) return false;
	const cSubBase* main = mainSubBase (*me);
	const int n = mapN_;
	const bool sea = data->factorSea > 0 && data->factorGround <= 0;
	const bool big = data->buildingData.isBig;
	// расстояние (по 4 соседям) до клеток главной подбазы — сколько соединителей нужно
	std::vector<int> gap (static_cast<std::size_t> (n) * n, 1000);
	std::deque<cPosition> q;
	for (const auto& b : me->getBuildings())
		if (main != nullptr && b->subBase == main)
			for (const auto& c : b->getPositions())
			{
				gap[static_cast<std::size_t> (c.y()) * n + c.x()] = 0;
				q.push_back (c);
			}
	if (q.empty()) return false; // базы ещё нет
	while (!q.empty())
	{
		const cPosition c = q.front();
		q.pop_front();
		const int g = gap[static_cast<std::size_t> (c.y()) * n + c.x()];
		if (g >= 10) continue;
		for (const cPosition d : {cPosition (1, 0), cPosition (-1, 0), cPosition (0, 1), cPosition (0, -1)})
		{
			const cPosition nc = c + d;
			if (!map.isValidPosition (nc)) continue;
			int& ng = gap[static_cast<std::size_t> (nc.y()) * n + nc.x()];
			if (ng > g + 1)
			{
				ng = g + 1;
				q.push_back (nc);
			}
		}
	}
	const int R = sea ? 24 : 14;
	long long bestKey = std::numeric_limits<long long>::max();
	cPosition best (-1, -1);
	for (int dy = -R; dy <= R; ++dy)
		for (int dx = -R; dx <= R; ++dx)
		{
			const cPosition p = home_ + cPosition (dx, dy);
			if (!sea && std::max (std::abs (dx), std::abs (dy)) < 3) continue; // ядро базы base2 — не занимать
			if (badSites_.count ({p.x(), p.y()}) != 0) continue;
			bool ok = true;
			for (const auto& c : footprint (p, big))
			{
				ok = ok && map.isValidPosition (c) && map.possiblePlaceBuilding (*data, c, me, nullptr);
				if (ok && !sea) ok = map.getResource (c).value == 0; // ресурсы — под шахты base2
			}
			if (!ok) continue;
			// не налезать на площадки других зданий налёта (выезд заводов)
			for (const auto& o : sites_)
				if (&o != &s && o.chosen && std::abs (o.site.x() - p.x()) < 3 && std::abs (o.site.y() - p.y()) < 3) ok = false;
			if (!ok) continue;
			int g = 1000;
			int exits = 0;
			for (const auto& c : sideNeighbours (p, big))
			{
				if (!map.isValidPosition (c)) continue;
				g = std::min (g, gap[static_cast<std::size_t> (c.y()) * n + c.x()]);
				const bool freeCell = map.getField (c).getBuildings().empty() && !map.isBlocked (c);
				exits += freeCell && (sea ? map.isWater (c) : (!map.isWater (c) && !map.isCoast (c))) ? 1 : 0;
			}
			if (g > 8 || exits < (sea ? 2 : 3)) continue;
			const long long key = static_cast<long long> (g) * 100000 + dist2 (p, home_);
			if (key < bestKey)
			{
				bestKey = key;
				best = p;
			}
		}
	if (best.x() < 0) return false;
	s.site = best;
	s.chosen = true;
	s.waited = 0;
	stepEvent ("site_chosen", {{"type", s.type}, {"at", posJson (best)}, {"gap", bestKey / 100000}, {"turn", turn_}});
	return true;
}

void cRedBrain::builderStep (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	const cVehicle* v = builderId_ != kNoUnit ? model.getVehicleFromID (builderId_) : nullptr;
	if (v == nullptr)
	{
		bool pending = false;
		for (const auto& s : sites_) pending = pending || s.id == kNoUnit;
		if (!pending) return;
		// конструктор налёта погиб — взять у хозяйства свободный (base2 построит себе новый)
		const cPlayer* me = model.getPlayer (playerId_);
		for (const auto& c : me->getVehicles())
			if (c->getStaticUnitData().getDefaultName() == "konstrukt" && raiders_.count (c->getId()) == 0 && !c->isUnitBuildingABuilding()
				&& !c->isUnitMoving() && !c->isUnitLoaded())
			{
				builderId_ = c->getId();
				raiders_[builderId_] = {builderId_, "konstrukt", turn_, false};
				stepEvent ("builder_replaced", {{"unit", builderId_}, {"turn", turn_}});
				for (auto& s : sites_)
					if (s.id == kNoUnit) s.waited = 0;
				return;
			}
		if (stagesDone_.count ("builder_lost") == 0) stage ("builder_lost", true, {{"turn", turn_}});
		return;
	}
	if (v->isDisabled() || finishIfBuilt (model, *v, out) || v->isUnitMoving() || v->getMoveJob() != nullptr) return;
	sRaidSite* next = nullptr;
	for (auto& s : sites_)
		if (s.id == kNoUnit)
		{
			next = &s;
			break;
		}
	if (next == nullptr) return;
	const cPlayer* me = model.getPlayer (playerId_);
	const cStaticUnitData* data = dataByName (model, next->type);
	if (data == nullptr) return;
	if (!next->chosen && !chooseSite (model, view, *next))
	{
		if (++next->fails == 10) stage ("site_" + next->type, false, {{"why", "нет площадки у базы"}, {"turn", turn_}});
		return;
	}
	const bool big = data->buildingData.isBig;
	const int cost = me->getLastUnitData (data->ID)->getBuildCost();
	// скорость стройки: быстрая (×2 металла), если металла хватает, — человек не ждёт 20 ходов авиазавод
	std::array<int, 3> rounds{}, costs{};
	v->calcTurboBuild (rounds, costs, cost);
	const int cap = v->getStaticUnitData().storageResMax;
	const int stored = v->getStoredResources();
	const bool turboOk = costs[1] > 0 && rounds[1] > 0 && costs[1] <= cap;
	if (stored < cost || (turboOk && stored < costs[1] && refillHere (model, *v)))
	{
		refill (model, view, *v, (turboOk ? costs[1] : std::min (cap, cost + 20)) - stored, out);
		return;
	}
	const int speed = turboOk && stored >= costs[1] ? 1 : 0;
	const auto cells = footprint (next->site, big);
	const bool onSite = std::find (cells.begin(), cells.end(), v->getPosition()) != cells.end();
	if (onSite)
	{
		bool free = true;
		for (const auto& c : cells) free = free && model.getMap()->possiblePlaceBuilding (*data, c, me, v);
		if (free)
		{
			cOrder o = order (eOrderType::Build, v->getId(), "raid builder builds " + next->type);
			o.buildingType = {data->ID.firstPart, data->ID.secondPart};
			o.position = next->site;
			o.buildSpeed = speed;
			out.push_back (std::move (o));
			stepEvent ("build", {{"type", next->type}, {"at", posJson (next->site)}, {"metal", v->getStoredResources()}, {"speed", speed},
				{"turns", rounds[speed]}, {"turn", turn_}});
			return;
		}
	}
	// площадку заняли или не подъехать за 6 ходов — другая
	if (++next->waited > 6 || (onSite && next->waited > 2))
	{
		badSites_.insert ({next->site.x(), next->site.y()});
		next->chosen = false;
		++next->fails;
		stepEvent ("site_dropped", {{"type", next->type}, {"at", posJson (next->site)}, {"turn", turn_}});
		return;
	}
	if (onSite) return;
	// ближайшая клетка площадки, куда конструктор встанет (на воде — тоже: он плавает)
	cPosition best (-1, -1);
	int bestD = std::numeric_limits<int>::max();
	for (const auto& c : cells)
		if (view.possiblePlace (*v, c) && dist2 (c, v->getPosition()) < bestD)
		{
			bestD = dist2 (c, v->getPosition());
			best = c;
		}
	if (best.x() >= 0) moveToward (model, view, *v, best, out, "raid builder to the site: " + next->type, false);
}

// путь соединителей (по 4 соседям) от здания налёта к главной подбазе
bool cRedBrain::planLink (const cModel& model, const sRaidSite& s)
{
	const cMap& map = *model.getMap();
	const cPlayer* me = model.getPlayer (playerId_);
	const cSubBase* main = mainSubBase (*me);
	const cStaticUnitData* conn = dataByName (model, "connector");
	const cBuilding* b = model.getBuildingFromID (s.id);
	if (main == nullptr || conn == nullptr || b == nullptr) return false;
	const int n = mapN_;
	std::vector<char> goal (static_cast<std::size_t> (n) * n, 0);
	for (const auto& mb : me->getBuildings())
		if (mb->subBase == main)
			for (const auto& c : sideNeighbours (mb->getPosition(), mb->getIsBig()))
				if (map.isValidPosition (c)) goal[static_cast<std::size_t> (c.y()) * n + c.x()] = 1;
	std::vector<int> from (static_cast<std::size_t> (n) * n, -2);
	std::deque<cPosition> q;
	const auto placeable = [&] (const cPosition& c) {
		if (hasBuildingNamed (map, c, "connector")) return true;
		return map.possiblePlaceBuilding (*conn, c, me, map.getField (c).getVehicle());
	};
	for (const auto& c : sideNeighbours (b->getPosition(), b->getIsBig()))
		if (map.isValidPosition (c) && placeable (c))
		{
			from[static_cast<std::size_t> (c.y()) * n + c.x()] = -1;
			q.push_back (c);
		}
	while (!q.empty())
	{
		const cPosition c = q.front();
		q.pop_front();
		if (goal[static_cast<std::size_t> (c.y()) * n + c.x()])
		{
			connPath_.clear();
			for (cPosition p = c;;)
			{
				connPath_.push_back (p);
				const int f = from[static_cast<std::size_t> (p.y()) * n + p.x()];
				if (f < 0) break;
				p = cPosition (f % n, f / n);
			}
			return true;
		}
		for (const cPosition d : {cPosition (1, 0), cPosition (-1, 0), cPosition (0, 1), cPosition (0, -1)})
		{
			const cPosition nc = c + d;
			if (!map.isValidPosition (nc) || from[static_cast<std::size_t> (nc.y()) * n + nc.x()] != -2 || !placeable (nc)) continue;
			from[static_cast<std::size_t> (nc.y()) * n + nc.x()] = c.y() * n + c.x();
			q.push_back (nc);
		}
	}
	return false;
}

void cRedBrain::engineerStep (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	const cPlayer* me = model.getPlayer (playerId_);
	const cSubBase* main = mainSubBase (*me);
	// какое здание налёта не в главной подбазе
	if (connFor_ < 0)
		for (std::size_t i = 0; i < sites_.size(); ++i)
		{
			auto& s = sites_[i];
			if (s.id == kNoUnit) continue;
			const cBuilding* b = model.getBuildingFromID (s.id);
			if (b == nullptr) continue;
			const bool in = main != nullptr && b->subBase == main;
			if (in && !s.linked)
			{
				s.linked = true;
				stage ("linked_" + s.type, true, {{"unit", s.id}, {"turn", turn_}});
			}
			if (in || s.linkTries >= 3) continue;
			++s.linkTries;
			if (planLink (model, s))
			{
				connFor_ = static_cast<int> (i);
				stepEvent ("link_planned", {{"type", s.type}, {"connectors", connPath_.size()}, {"turn", turn_}});
				break;
			}
			if (s.linkTries == 3) stage ("linked_" + s.type, false, {{"why", "нет пути соединителей к базе"}, {"turn", turn_}});
		}
	if (connFor_ < 0) return;
	const cVehicle* v = engineerId_ != kNoUnit ? model.getVehicleFromID (engineerId_) : nullptr;
	if (v == nullptr)
		// инженер налёта погиб — взять у хозяйства свободного
		for (const auto& c : me->getVehicles())
			if (c->getStaticUnitData().getDefaultName() == "pionier" && raiders_.count (c->getId()) == 0 && !c->isUnitBuildingABuilding()
				&& !c->isUnitMoving() && !c->isUnitLoaded())
			{
				engineerId_ = c->getId();
				raiders_[engineerId_] = {engineerId_, "pionier", turn_, false};
				stepEvent ("engineer_replaced", {{"unit", engineerId_}, {"turn", turn_}});
				return;
			}
	if (v == nullptr || v->isDisabled() || finishIfBuilt (model, *v, out) || v->isUnitMoving() || v->getMoveJob() != nullptr) return;
	const cMap& map = *model.getMap();
	const cStaticUnitData* conn = dataByName (model, "connector");
	cPosition target (-1, -1);
	for (const auto& c : connPath_)
		if (!hasBuildingNamed (map, c, "connector"))
		{
			target = c;
			break;
		}
	if (target.x() < 0 || (sites_[connFor_].id != kNoUnit && model.getBuildingFromID (sites_[connFor_].id) != nullptr
		&& model.getBuildingFromID (sites_[connFor_].id)->subBase == main))
	{
		connPath_.clear();
		connFor_ = -1; // проложено — проверка подбазы в следующем ходу
		return;
	}
	const int cost = me->getLastUnitData (conn->ID)->getBuildCost();
	if (v->getStoredResources() < cost)
	{
		refill (model, view, *v, v->getStaticUnitData().storageResMax - v->getStoredResources(), out);
		return;
	}
	if (v->getPosition() == target)
	{
		if (map.possiblePlaceBuilding (*conn, target, me, v))
		{
			cOrder o = order (eOrderType::Build, v->getId(), "raid engineer lays a connector");
			o.buildingType = {conn->ID.firstPart, conn->ID.secondPart};
			o.position = target;
			o.buildSpeed = 0;
			out.push_back (std::move (o));
		}
		else
		{
			connPath_.clear(); // клетку заняли — проложить заново
			connFor_ = -1;
		}
		return;
	}
	if (!moveToward (model, view, *v, target, out, "raid engineer to the connector cell", false))
	{
		connPath_.clear();
		connFor_ = -1;
	}
}

void cRedBrain::produce (const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* me = model.getPlayer (playerId_);
	const auto force = redFairForce (scenario_);
	std::map<std::string, int> need, have;
	for (const auto& t : force) ++need[t];
	for (const auto& [id, r] : raiders_)
		if (!r.lost && model.getVehicleFromID (id) != nullptr) ++have[r.type];
	std::vector<const cBuilding*> factories;
	for (const auto& s : sites_)
		if (s.id != kNoUnit)
			if (const cBuilding* b = model.getBuildingFromID (s.id); b != nullptr && !b->getStaticUnitData().canBuild.empty())
			{
				factories.push_back (b);
				for (std::size_t k = 0; k < b->getBuildListSize(); ++k)
					if (const auto* d = &model.getUnitsData()->getStaticUnitData (b->getBuildListItem (k).getType())) ++have[d->getDefaultName()];
			}
	for (const cBuilding* b : factories)
	{
		if (b->isDisabled()) continue;
		if (b->isBuildListEmpty())
		{
			for (const auto& t : force)
			{
				const cStaticUnitData* d = dataByName (model, t);
				if (d == nullptr || d->buildAs != b->getStaticUnitData().canBuild || have[t] >= need[t]) continue;
				cOrder o = order (eOrderType::ChangeBuildList, b->getId(), "raid factory builds " + t);
				o.buildList = {{d->ID.firstPart, d->ID.secondPart}};
				o.buildSpeed = 0;
				o.repeat = false;
				out.push_back (std::move (o));
				++have[t];
				stepEvent ("produce", {{"factory", b->getStaticUnitData().getDefaultName()}, {"type", t}, {"turn", turn_}});
				break;
			}
			continue;
		}
		if (b->getBuildListItem (0).getRemainingMetal() <= 0)
		{
			const auto& vd = model.getUnitsData()->getStaticUnitData (b->getBuildListItem (0).getType());
			const cPosition esc = exitCell (model, *b, vd, *me);
			if (esc.x() < 0) continue;
			cOrder o = order (eOrderType::FinishBuild, b->getId(), "raid factory rolls out " + vd.getDefaultName());
			o.position = esc;
			out.push_back (std::move (o));
			continue;
		}
		if (!b->isUnitWorking() && b->buildingCanBeStarted())
			out.push_back (order (eOrderType::StartWork, b->getId(), "raid factory starts"));
	}
}

void cRedBrain::checkForce (const cModel& model)
{
	if (forceReady_) return;
	std::map<std::string, int> need, have;
	for (const auto& t : redFairForce (scenario_)) ++need[t];
	for (const auto& [id, r] : raiders_)
		if (!r.lost && model.getVehicleFromID (id) != nullptr) ++have[r.type];
	bool ready = true;
	for (const auto& [t, k] : need) ready = ready && have[t] >= k;
	if (ready && (scenario_ == eRedScenario::Combo || scenario_ == eRedScenario::Sea) && have["tank"] < 3)
	{
		// танки высадки погибли (охраняя дом) — взять у армии хозяйства свободные (человек так и сделал бы)
		const cPlayer* me = model.getPlayer (playerId_);
		for (const auto& v : me->getVehicles())
		{
			if (have["tank"] >= 3) break;
			if (v->getStaticUnitData().getDefaultName() != "tank" || raiders_.count (v->getId()) != 0 || v->isUnitLoaded() || v->isUnitMoving()
				|| v->isDisabled())
				continue;
			raiders_[v->getId()] = {v->getId(), "tank", turn_, false};
			++have["tank"];
			stepEvent ("tank_taken", {{"unit", v->getId()}, {"turn", turn_}});
		}
	}
	if (scenario_ == eRedScenario::Combo || scenario_ == eRedScenario::Sea) ready = ready && have["tank"] >= 1;
	if (!ready) return;
	forceReady_ = true;
	nlohmann::json units = nlohmann::json::object();
	for (const auto& [t, k] : have) units[t] = k;
	stage ("force_ready", true, {{"units", units}, {"baseKnown", baseKnown_}, {"turn", turn_}});
}

// --- ангар: перезарядка и ремонт самолётов ---------------------------------------------------------
void cRedBrain::service (const cModel& model, const cMapView& view, std::vector<cOrder>& out)
{
	const cBuilding* hangar = nullptr;
	for (const auto& s : sites_)
		if (s.type == "hangar" && s.id != kNoUnit) hangar = model.getBuildingFromID (s.id);
	if (hangar == nullptr)
	{
		service_.clear();
		return;
	}
	const cPlayer* me = model.getPlayer (playerId_);
	const cMap& map = *model.getMap();
	for (const auto& [id, r] : raiders_)
	{
		if (r.lost || !isPlaneType (r.type)) continue;
		const cVehicle* v = model.getVehicleFromID (id);
		if (v == nullptr) continue;
		const auto& sd = v->getStaticUnitData();
		const bool armed = sd.canAttack != 0 && v->data.getAmmoMax() > 0;
		const bool needAmmo = armed && v->data.getAmmo() == 0;
		const bool needRepair = v->data.getHitpoints() * 10 < v->data.getHitpointsMax() * 6;
		const bool inHangar = v->isUnitLoaded() && std::find (hangar->storedUnits.begin(), hangar->storedUnits.end(), v) != hangar->storedUnits.end();
		if (inHangar)
		{
			service_.insert (id);
			const bool fullAmmo = !armed || v->data.getAmmo() >= v->data.getAmmoMax();
			const bool fullHp = v->data.getHitpoints() >= v->data.getHitpointsMax();
			if (!fullAmmo && hangar->canSupply (v, eSupplyType::REARM))
			{
				cOrder o = order (eOrderType::Rearm, hangar->getId(), "hangar rearms " + r.type);
				o.unitId2 = id;
				out.push_back (std::move (o));
				stepEvent ("rearm", {{"unit", id}, {"type", r.type}, {"turn", turn_}});
			}
			if (!fullHp && hangar->canSupply (v, eSupplyType::REPAIR))
			{
				cOrder o = order (eOrderType::Repair, hangar->getId(), "hangar repairs " + r.type);
				o.unitId2 = id;
				out.push_back (std::move (o));
				stepEvent ("repair", {{"unit", id}, {"type", r.type}, {"turn", turn_}});
			}
			if (fullAmmo && fullHp)
			{
				const cPosition esc = exitCell (model, *hangar, sd, *me);
				if (esc.x() >= 0)
				{
					cOrder o = order (eOrderType::ActivateUnit, hangar->getId(), r.type + " leaves the hangar");
					o.unitId2 = id;
					o.position = esc;
					out.push_back (std::move (o));
					stage ("serviced", true, {{"unit", id}, {"type", r.type}, {"turn", turn_}});
					service_.erase (id);
				}
			}
			continue;
		}
		if (v->isUnitLoaded() || !v->storedUnits.empty() || !(needAmmo || needRepair))
		{
			service_.erase (id);
			continue;
		}
		service_.insert (id);
		if (v->isUnitMoving() || v->getMoveJob() != nullptr) continue;
		if (hangar->isNextTo (v->getPosition()) && hangar->canLoad (v))
		{
			cOrder o = order (eOrderType::Load, hangar->getId(), r.type + " lands in the hangar");
			o.unitId2 = id;
			out.push_back (std::move (o));
			wantPass_ = true;
			continue;
		}
		cPosition best (-1, -1);
		int bestD = std::numeric_limits<int>::max();
		for (const auto& c : sideNeighbours (hangar->getPosition(), true))
			if (map.isValidPosition (c) && view.possiblePlace (*v, c) && reserved_.count ({c.x(), c.y()}) == 0 && dist2 (c, v->getPosition()) < bestD)
			{
				bestD = dist2 (c, v->getPosition());
				best = c;
			}
		if (best.x() < 0) continue;
		auto path = toVector (cPathCalculator (*v, view, best, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
		const std::size_t full = path.size();
		cutToTurn (*v, view, path);
		if (path.empty()) continue;
		cOrder o = order (eOrderType::Move, id, r.type + (needAmmo ? " out of ammo" : " damaged") + ": to the hangar");
		o.path = path;
		o.position = path.back();
		if (path.size() == full)
		{
			o.unitId2 = hangar->getId();
			o.moveEnd = eMoveEnd::GetIn;
		}
		reserved_.insert ({path.back().x(), path.back().y()});
		out.push_back (std::move (o));
		stepEvent ("to_hangar", {{"unit", id}, {"type", r.type}, {"ammo", v->data.getAmmo()}, {"hp", v->data.getHitpoints()},
			{"arrives", path.size() == full}, {"turn", turn_}});
	}
}

} // namespace ai
