/***************************************************************************
 * ai_bulldozer.cpp — бульдозер у ИИИ (И1). Описание — в заголовке.
 ***************************************************************************/

#include "ai_bulldozer.h"

#include "ai_roles.h"
#include "ai_world_model.h"

#include "game/data/base/base.h"
#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <tuple>

namespace ai
{

namespace
{

// --- константы (И1, выбраны по переписи обломков партий эталона В2: у базы — 15–50 клеток и 100–260 металла, на поле —
// 10–90 клеток и до 800 металла) ---
constexpr int kBaseRing = 3;        // обломки ближе (Чебышёв) к зданию главной подбазы — мешают стройке у базы
constexpr int kBaseMetalR = 12;     // прочие обломки ближе к центру базы — металл у базы (бульдозер базы собирает и их, разряд 2)
constexpr int kBaseMetalMin = 10;   // бульдозер у базы — и без мешающих обломков, когда металла у базы столько
constexpr int kSiteRadius = 18;     // = kMaxSiteRadius правил: площадки шахт — не дальше от высадки
constexpr int kFieldRadius = 12;    // обломки поля — не дальше от своей боевой машины
constexpr int kFieldMinArmy = 6;    // бульдозер армии — при армии от стольких машин (ремонтники — с 6, ai_roles)
constexpr int kFieldMinMetal = 12;  // и металла в обломках поля у армии — не меньше
constexpr int kFieldTwoArmy = 20;   // второй бульдозер армии — при армии от стольких машин
constexpr int kFieldTwoMetal = 40;  // и металле поля
constexpr int kMaxBulldozers = 3;
constexpr int kMinFieldRubble = 2;  // обломки дешевле — не цель на поле (у базы — любые: мешают стройке)
constexpr int kDeliverMin = 10;     // отдать металл, когда в грузе столько (или собирать нечего, или груз полон)
constexpr int kFeedReach = 12;      // ремонтник или грузовик ждёт бульдозер с металлом не дальше (Чебышёв)
constexpr int kFeedMin = 4;         // бульдозер «с металлом» — от стольких в грузе
constexpr int kReceiverReach = 24;  // получатель — не дальше (Чебышёв)
constexpr int kBaseDock = 10;       // бульдозер у базы (сдать в склад) — не дальше от здания главной подбазы
constexpr int kEscortHold = 4;      // бульдозер армии без обломков держится у хвоста армии в стольких клетках
constexpr int kSummaryEvery = 25;   // сводка в журнал — раз в столько ходов
constexpr eThreatLayer kLayer = eThreatLayer::Strike; // бульдозер хрупкий (hp 14, броня 4): вне «удара с подъезда» врага

int cheb (const cPosition& a, const cPosition& b)
{
	return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y()));
}

std::vector<cPosition> areaOf (const cPosition& p, bool big)
{
	if (!big) return {p};
	return {p, p + cPosition (1, 0), p + cPosition (0, 1), p + cPosition (1, 1)};
}

bool inArea (const cPosition& c, const cPosition& p, bool big)
{
	return c.x() >= p.x() && c.y() >= p.y() && c.x() <= p.x() + (big ? 1 : 0) && c.y() <= p.y() + (big ? 1 : 0);
}

bool isBulldozer (const cVehicle& v)
{
	return v.getStaticData().canClearArea;
}

// бульдозер может получить приказ: не выключен, не в транспорте, не расчищает
bool freeBulldozer (const cVehicle& v)
{
	return isBulldozer (v) && !v.isDisabled() && !v.isUnitLoaded() && !v.isUnitClearing() && !v.isUnitBuildingABuilding();
}

std::vector<cPosition> toVec (const std::forward_list<cPosition>& path)
{
	return std::vector<cPosition> (path.begin(), path.end());
}

// путь до хода (как у правил): столько клеток, сколько машина пройдёт за ход
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

// путь к dest в обход угроз (как threatAwarePath правил), до хода
std::vector<cPosition> pathTo (const cVehicle& v, const cMapView& view, const cWorldModel& world, const cPosition& dest)
{
	auto path = toVec (cPathCalculator (v, view, dest, false).calcPath());
	if (!path.empty() && world.pathUnsafe (v, path, kLayer))
	{
		auto alt = world.safePath (v, view, dest, kLayer);
		if (!alt.empty()) path = std::move (alt);
	}
	cutToTurn (v, view, path);
	return path;
}

cOrder makeOrder (eOrderType type, int playerId, unsigned int unitId, std::string reason, const char* level = "micro")
{
	cOrder o;
	o.type = type;
	o.playerId = playerId;
	o.unitId = unitId;
	o.level = level;
	o.reason = std::move (reason);
	return o;
}

nlohmann::json posJ (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

// машина-получатель металла бульдозера: ремонтник или грузовик боеприпасов (наземный, металл в грузе)
bool receiverRole (eRole r)
{
	return r == eRole::Repair || r == eRole::Supply;
}

} // namespace

//------------------------------------------------------------------------------
bool bulldozerOn()
{
	static const bool on = [] {
		const char* v = std::getenv ("MAXR_BULLDOZER");
		return !(v != nullptr && v[0] == '0');
	}();
	return on;
}

//------------------------------------------------------------------------------
bool cBulldozers::baseBlocking (const cModel& model, const cPlayer& player, const cSubBase* main, const sRubbleMemory& r) const
{
	const cMap& map = *model.getMap();
	const cPosition landing = player.getLandingPos();
	for (const cPosition& c : areaOf (r.pos, r.big))
	{
		if (!map.isValidPosition (c)) continue;
		// место шахты: разведанный ресурс у базы (шахта ставится только на нём)
		if (cheb (c, landing) <= kSiteRadius && player.hasResourceExplored (c) && map.getResource (c).typ != eResourceType::None
			&& map.getResource (c).value > 0)
			return true;
	}
	if (main == nullptr) return false;
	// у зданий главной подбазы: здесь база растёт (площадки касаются её стороной)
	for (const cBuilding* b : main->getBuildings())
		for (const cPosition& bc : b->getPositions())
			for (const cPosition& c : areaOf (r.pos, r.big))
				if (cheb (bc, c) <= kBaseRing) return true;
	return false;
}

//------------------------------------------------------------------------------
void cBulldozers::update (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cSubBase* main, const cPosition& baseCenter, int armyCount, int turn)
{
	turn_ = turn;
	wantPass_ = false;
	claimed_.clear();
	jobs_.clear();
	feeds_.clear();
	baseSites_.clear();
	fieldSites_.clear();
	fedThisTurn_.clear();
	counts_ = {};
	const cMap& map = *model.getMap();

	// тип бульдозера, что строит малая фабрика (первый по данным)
	type_ = sID();
	for (const auto& sd : model.getUnitsData()->getStaticUnitsData())
		if (sd.ID.isAVehicle() && sd.vehicleData.canClearArea && sd.buildAs == "SmallGroundVehicle" && !sd.isAlien
			&& player.getLastUnitData (sd.ID) != nullptr)
		{
			type_ = sd.ID;
			break;
		}

	// 1) память обломков: видимые сейчас — запомнить; исчезнувшие с видимой клетки — забыть
	std::set<cPosition, sPosLess> seenNow;
	for (const auto& b : model.getNeutralBuildings())
	{
		if (!b->isRubble() || !player.canSeeUnit (*b, map)) continue;
		sRubbleMemory m;
		m.pos = b->getPosition();
		m.value = b->getRubbleValue();
		m.big = b->getIsBig();
		m.seenTurn = turn;
		rubble_[m.pos] = m;
		seenNow.insert (m.pos);
	}
	for (auto it = rubble_.begin(); it != rubble_.end();)
	{
		bool visible = false;
		for (const cPosition& c : areaOf (it->second.pos, it->second.big))
			if (map.isValidPosition (c) && player.canSeeAt (c)) visible = true;
		if (visible && seenNow.count (it->first) == 0)
			it = rubble_.erase (it);
		else
			++it;
	}

	// 2) сверка расчистки прошлого хода: обломков под бульдозером нет — расчищено, металл — в груз
	for (auto it = clearing_.begin(); it != clearing_.end();)
	{
		const cVehicle* v = model.getVehicleFromID (it->first);
		if (v == nullptr || v->getOwner() != &player)
		{
			it = clearing_.erase (it);
			continue;
		}
		if (v->isUnitClearing())
		{
			++it;
			continue; // большие — 4 хода
		}
		const sClearing& c = it->second;
		const cBuilding* still = map.getField (c.pos).getRubble();
		const bool gone = still == nullptr || still->getPosition() != c.pos;
		if (gone)
		{
			const int gained = std::max (0, v->getStoredResources() - c.cargoBefore);
			++totals_.cleared;
			totals_.cellsCleared += c.big ? 4 : 1;
			totals_.metalCollected += gained;
			totals_.metalLost += std::max (0, c.value - gained);
			for (const cPosition& cell : areaOf (c.pos, c.big))
				clearedBy_[cell] = turn;
			rubble_.erase (c.pos);
			events_.push_back ({{"event", "bulldozer"}, {"level", "micro"}, {"what", "cleared"}, {"unitId", it->first}, {"pos", posJ (c.pos)},
				{"big", c.big}, {"value", c.value}, {"gained", gained}, {"cargo", v->getStoredResources()}, {"site", c.site}});
		}
		it = clearing_.erase (it);
	}

	// 3) свои бульдозеры, ремонтники и грузовики
	std::vector<const cVehicle*> own;
	bool support = false;
	for (const auto& v : player.getVehicles())
	{
		if (isBulldozer (*v)) own.push_back (v.get());
		if (receiverRole (roles.roleOf (*v)) && v->getStaticUnitData().storeResType == eResourceType::Metal) support = true;
	}
	counts_.bulldozers = static_cast<int> (own.size());
	{
		std::set<unsigned int> alive;
		for (const cVehicle* b : own) alive.insert (b->getId());
		for (const unsigned int id : alive_)
			if (alive.count (id) == 0) ++totals_.lost;
		alive_ = std::move (alive);
	}
	counts_.known = static_cast<int> (rubble_.size());

	// 4) обломки у базы, мешающие стройке, и на поле у армии
	std::vector<const cVehicle*> fighters;
	for (const auto& v : player.getVehicles())
		if (isArmyRole (roles.roleOf (*v)) && !v->isUnitLoaded()) fighters.push_back (v.get());
	std::vector<std::tuple<int, int, int, int, int>> base; // разряд (0 — место шахты, 1 — у зданий базы, 2 — металл у базы), дальность до
	                                                       // центра базы, −металл, y, x
	int baseMetalOnly = 0; // металла в обломках разряда 2
	std::vector<std::tuple<int, int, int, int>> field;     // −металл, дальность до ближайшей своей боевой машины, y, x
	for (const auto& [pos, r] : rubble_)
	{
		bool onRes = false;
		for (const cPosition& c : areaOf (pos, r.big))
			if (map.isValidPosition (c) && player.hasResourceExplored (c) && map.getResource (c).typ != eResourceType::None
				&& map.getResource (c).value > 0)
				onRes = true;
		const int dc = baseCenter.x() >= 0 ? cheb (pos, baseCenter) : 0;
		if (baseBlocking (model, player, main, r))
		{
			base.emplace_back (onRes ? 0 : 1, dc, -r.value, pos.y(), pos.x());
			++counts_.baseRubble;
			counts_.baseMetal += r.value;
			if (onRes) ++counts_.onResource;
			continue;
		}
		if (r.value < kMinFieldRubble) continue;
		if (baseCenter.x() >= 0 && dc <= kBaseMetalR)
		{
			base.emplace_back (2, dc, -r.value, pos.y(), pos.x());
			baseMetalOnly += r.value;
			continue;
		}
		int nearest = std::numeric_limits<int>::max();
		for (const cVehicle* f : fighters)
			nearest = std::min (nearest, cheb (f->getPosition(), pos));
		if (nearest > kFieldRadius) continue;
		field.emplace_back (-r.value, nearest, pos.y(), pos.x());
		++counts_.fieldRubble;
		counts_.fieldMetal += r.value;
	}
	std::sort (base.begin(), base.end());
	std::sort (field.begin(), field.end());
	for (const auto& t : base) baseSites_.push_back (cPosition (std::get<4> (t), std::get<3> (t)));
	for (const auto& t : field) fieldSites_.push_back (cPosition (std::get<3> (t), std::get<2> (t)));

	// 5) нужды (в): у базы — один, когда обломки мешают стройке; при армии — по армии и металлу поля
	counts_.baseMetalOnly = baseMetalOnly;
	counts_.wantedBase = (counts_.baseRubble > 0 || baseMetalOnly >= kBaseMetalMin) ? 1 : 0;
	if (armyCount >= kFieldMinArmy && support && counts_.fieldMetal >= kFieldMinMetal)
		counts_.wantedField = armyCount >= kFieldTwoArmy && counts_.fieldMetal >= kFieldTwoMetal ? 2 : 1;

	// 6) ремонтник или грузовик ждёт металл от бульдозера рядом (не едет на базу)
	for (const auto& r : player.getVehicles())
	{
		if (!receiverRole (roles.roleOf (*r)) || r->isUnitLoaded()) continue;
		for (const cVehicle* b : own)
			if (!b->isDisabled() && !b->isUnitLoaded() && b->getStoredResources() >= kFeedMin && cheb (b->getPosition(), r->getPosition()) <= kFeedReach
				&& world.threatFor (*b, b->getPosition(), kLayer) == 0)
			{
				feeds_.insert (r->getId());
				break;
			}
	}

	(void) view;
}

//------------------------------------------------------------------------------
// сводка за партию — в ходу, где итоги изменились (последняя в журнале — итог партии), и раз в kSummaryEvery ходов
void cBulldozers::noteSummary()
{
	if (counts_.bulldozers == 0 && totals_.built == 0) return;
	if (totals_ == lastTotals_ && turn_ - lastSummary_ < kSummaryEvery) return;
	lastSummary_ = turn_;
	lastTotals_ = totals_;
	events_.push_back (summaryJson());
}

//------------------------------------------------------------------------------
int cBulldozers::toBuild (int have) const
{
	if (type_ == sID()) return 0;
	const int want = std::min (kMaxBulldozers, counts_.wantedBase + counts_.wantedField);
	return std::max (0, want - have);
}

//------------------------------------------------------------------------------
bool cBulldozers::feedsNear (const cVehicle& receiver) const
{
	return feeds_.count (receiver.getId()) != 0;
}

//------------------------------------------------------------------------------
nlohmann::json cBulldozers::summaryJson() const
{
	return {{"event", "bulldozer"}, {"level", "operational"}, {"what", "summary"}, {"turn", turn_}, {"bulldozers", counts_.bulldozers},
		{"wantedBase", counts_.wantedBase}, {"wantedField", counts_.wantedField}, {"baseRubble", counts_.baseRubble},
		{"baseMetal", counts_.baseMetal}, {"fieldRubble", counts_.fieldRubble}, {"fieldMetal", counts_.fieldMetal}, {"known", counts_.known},
		{"built", totals_.built}, {"clearOrders", totals_.clearOrders}, {"cleared", totals_.cleared}, {"cellsCleared", totals_.cellsCleared},
		{"metalCollected", totals_.metalCollected}, {"metalLost", totals_.metalLost}, {"transfers", totals_.transfers},
		{"metalToRepair", totals_.metalToRepair}, {"metalToSupply", totals_.metalToSupply}, {"metalToBase", totals_.metalToBase},
		{"repairsFromIt", totals_.repairsFromIt}, {"rearmsFromIt", totals_.rearmsFromIt}, {"buildsOnCleared", totals_.buildsOnCleared},
		{"repairerHeld", totals_.repairerHeld}, {"lost", totals_.lost}, {"onResource", counts_.onResource},
		{"baseMetalOnly", counts_.baseMetalOnly}};
}

//------------------------------------------------------------------------------
// кому отдать металл. «Голодный» (hungry) — ремонтник или грузовик с запасом ниже половины, ближе kReceiverReach: по доле запаса,
// потом ближе, потом id. Груз полон или собирать нечего (anyRoom) — и любой ремонтник или грузовик с местом, а потом склад базы
// (место есть): у базы (kBaseDock) — всегда, дальше — только в этих случаях. kNoUnit — некому
unsigned int cBulldozers::chooseReceiver (const cPlayer& player, const cWorldModel& world, const cRoleBook& roles, const cSubBase* main,
	const cVehicle& b, bool anyRoom, bool& hungry, bool& building) const
{
	hungry = false;
	building = false;
	unsigned int best = kNoUnit;
	std::tuple<int, long long, int, unsigned int> bestKey {2, 0, 0, 0};
	for (const auto& r : player.getVehicles())
	{
		if (!receiverRole (roles.roleOf (*r)) || r->isUnitLoaded() || r->isDisabled() || r->isUnitBuildingABuilding()) continue;
		const auto& sd = r->getStaticUnitData();
		if (sd.storeResType != eResourceType::Metal || sd.storageResMax <= 0) continue;
		const int room = sd.storageResMax - r->getStoredResources();
		const bool low = r->getStoredResources() * 2 < sd.storageResMax;
		if (room <= 0 || (!low && !(anyRoom && room >= std::min (b.getStoredResources(), kDeliverMin)))) continue;
		const int d = cheb (r->getPosition(), b.getPosition());
		if (d > kReceiverReach || world.groundFire (r->getPosition()) > 0) continue;
		const std::tuple<int, long long, int, unsigned int> key {low ? 0 : 1, static_cast<long long> (r->getStoredResources()) * 1000 / sd.storageResMax,
			d, r->getId()};
		if (key < bestKey)
		{
			bestKey = key;
			best = r->getId();
		}
	}
	if (best != kNoUnit)
	{
		hungry = std::get<0> (bestKey) == 0;
		return best;
	}
	// склад базы: место в складе есть; бульдозер у базы — или груз полон, или собирать нечего
	if (main == nullptr || main->getResourcesStored().metal >= main->getMaxResourcesStored().metal) return kNoUnit;
	const cBuilding* nearest = nullptr;
	int nd = std::numeric_limits<int>::max();
	for (const cBuilding* bb : main->getBuildings())
	{
		const int d = cheb (bb->getPosition(), b.getPosition());
		if (d < nd)
		{
			nd = d;
			nearest = bb;
		}
	}
	if (nearest == nullptr || (nd > kBaseDock && !anyRoom)) return kNoUnit;
	building = true;
	return nearest->getId();
}

//------------------------------------------------------------------------------
// приказ Transfer бульдозер → получатель (рядом): сколько влезет; учёт — в сводку. Возвращает, сколько отдано (0 — нельзя)
int cBulldozers::pushTransfer (const cModel& model, const cPlayer& player, const cRoleBook& roles, const cVehicle& b, unsigned int receiver,
	int pass, std::vector<cOrder>& out)
{
	const cUnit* r = model.getUnitFromID (receiver);
	if (r == nullptr || r->getOwner() != &player || !r->isNextTo (b.getPosition())) return 0;
	int room = 0;
	const char* to = "base";
	if (const auto* rb = dynamic_cast<const cBuilding*> (r))
	{
		if (rb->subBase == nullptr) return 0;
		room = rb->subBase->getMaxResourcesStored().metal - rb->subBase->getResourcesStored().metal;
	}
	else
	{
		const auto* rv = static_cast<const cVehicle*> (r);
		if (rv->isUnitBuildingABuilding() || rv->isUnitClearing() || rv->getStaticUnitData().storeResType != eResourceType::Metal) return 0;
		room = rv->getStaticUnitData().storageResMax - rv->getStoredResources();
		to = roles.roleOf (*rv) == eRole::Repair ? "repair" : "supply";
	}
	const int give = std::min (b.getStoredResources(), room);
	if (give <= 0) return 0;
	cOrder o = makeOrder (eOrderType::Transfer, player.getId(), b.getId(),
		(pass > 1 ? "pass " + std::to_string (pass) + ": " : std::string()) + "bulldozer: metal from rubble to " + to);
	o.unitId2 = receiver;
	o.transferValue = give;
	o.resType = eResourceType::Metal;
	out.push_back (std::move (o));
	++totals_.transfers;
	const std::string toName (to);
	if (toName == "repair") totals_.metalToRepair += give;
	else if (toName == "supply") totals_.metalToSupply += give;
	else totals_.metalToBase += give;
	if (toName != "base")
	{
		fedMetal_[receiver] += give;
		fedThisTurn_.insert (receiver);
	}
	events_.push_back ({{"event", "bulldozer"}, {"level", "micro"}, {"what", "transfer"}, {"unitId", b.getId()}, {"to", to},
		{"receiver", receiver}, {"value", give}, {"pass", pass}});
	return give;
}

//------------------------------------------------------------------------------
// приказ Clear: бульдозер стоит на обломках; запомнить для сверки в следующем ходу
void cBulldozers::pushClear (const cPlayer& player, const cVehicle& b, const cBuilding& rubble, int cargo, const char* site, int pass,
	std::vector<cOrder>& out)
{
	cOrder o = makeOrder (eOrderType::Clear, player.getId(), b.getId(),
		(pass > 1 ? "pass " + std::to_string (pass) + ": " : std::string()) + "bulldozer: clear rubble (" + site + ")");
	o.position = rubble.getPosition();
	out.push_back (std::move (o));
	sClearing c;
	c.pos = rubble.getPosition();
	c.big = rubble.getIsBig();
	c.value = rubble.getRubbleValue();
	c.cargoBefore = cargo;
	c.site = site;
	clearing_[b.getId()] = c;
	++totals_.clearOrders;
	events_.push_back ({{"event", "bulldozer"}, {"level", "micro"}, {"what", "clear"}, {"unitId", b.getId()}, {"pos", posJ (c.pos)},
		{"big", c.big}, {"value", c.value}, {"cargo", cargo}, {"site", site}, {"pass", pass}});
}

//------------------------------------------------------------------------------
// обломки под бульдозером можно расчищать: вне угрозы, большие — их клетки свободны; металл влезает в груз (cargo — груз после
// передач этого захода), или груз пуст, или металл отдать некому (часть пропадёт, но больше его не взять)
const cBuilding* cBulldozers::clearableUnder (const cMapView& view, const cWorldModel& world, const cVehicle& b, int cargo,
	bool canDeliver) const
{
	const cBuilding* rubble = view.getField (b.getPosition()).getRubble();
	if (rubble == nullptr) return nullptr;
	for (const cPosition& c : areaOf (rubble->getPosition(), rubble->getIsBig()))
	{
		if (!view.isValidPosition (c) || world.threatFor (b, c, kLayer) > 0) return nullptr;
		if (c != b.getPosition() && !view.possiblePlace (b, c)) return nullptr;
	}
	const int cap = b.getStaticUnitData().storageResMax;
	if (cargo >= cap) return nullptr; // груз полон — металл обломков пропал бы весь
	if (cargo > 0 && cargo + rubble->getRubbleValue() > cap && canDeliver) return nullptr; // сначала отдать металл
	return rubble;
}

//------------------------------------------------------------------------------
void cBulldozers::plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
	const cSubBase* main, const cPosition& armyAnchor, bool armyOut, std::vector<cPosition>& reserved, std::vector<cOrder>& out)
{
	std::set<unsigned int> ordered;
	for (const auto& o : out) ordered.insert (o.unitId);
	const int playerId = player.getId();
	std::vector<const cVehicle*> own;
	for (const auto& v : player.getVehicles())
		if (isBulldozer (*v)) own.push_back (v.get());
	// у базы — бульдозер с наименьшим id (один), остальные — при армии; у базы нечего — все при армии
	bool baseTaken = baseSites_.empty();
	std::vector<cPosition> taken; // обломки, взятые другим бульдозером в этом ходу
	for (const auto& kv : clearing_) taken.push_back (kv.second.pos);
	auto isTaken = [&] (const cPosition& p) { return std::find (taken.begin(), taken.end(), p) != taken.end(); };
	auto isReserved = [&] (const cPosition& p) { return std::find (reserved.begin(), reserved.end(), p) != reserved.end(); };
	auto pushMove = [&] (const cVehicle& b, std::vector<cPosition> path, const std::string& reason) {
		reserved.push_back (path.back());
		cOrder o = makeOrder (eOrderType::Move, playerId, b.getId(), reason);
		o.path = std::move (path);
		o.position = o.path.back();
		out.push_back (std::move (o));
	};

	for (const cVehicle* bp : own)
	{
		const cVehicle& b = *bp;
		if (b.isUnitClearing())
		{
			claimed_.insert (b.getId()); // расчищает — не трогать
			continue;
		}
		if (!freeBulldozer (b) || b.isUnitMoving() || b.getMoveJob() != nullptr || ordered.count (b.getId()) != 0) continue;
		claimed_.insert (b.getId());
		const cPosition up = b.getPosition();
		const int cap = b.getStaticUnitData().storageResMax;
		const bool forBase = !baseTaken;
		if (forBase) baseTaken = true;
		const std::vector<cPosition>& sites = forBase ? baseSites_ : fieldSites_;
		const char* siteName = forBase ? "base" : "field";

		// 0) под угрозой — уйти (как стройщик): хрупкая машина
		if (world.threatFor (b, up, kLayer) > 0)
		{
			cPosition safe;
			if (world.nearestSafeCell (b, view, up, 10, player.getLandingPos(), safe, kLayer) && safe != up)
				if (auto path = pathTo (b, view, world, safe); !path.empty())
				{
					pushMove (b, std::move (path), "bulldozer: away from enemy");
					continue;
				}
		}

		int cargo = b.getStoredResources();
		// 1) голодный ремонтник или грузовик рядом — отдать сразу (Transfer не тратит ход)
		bool hungry = false, toBuilding = false;
		unsigned int receiver = cargo > 0 ? chooseReceiver (player, world, roles, main, b, cargo >= cap, hungry, toBuilding) : kNoUnit;
		if (receiver != kNoUnit)
			if (const cUnit* r = model.getUnitFromID (receiver); r != nullptr && r->isNextTo (up) && (hungry || cargo >= cap || cargo >= kDeliverMin))
				cargo -= pushTransfer (model, player, roles, b, receiver, 1, out);

		// цели: обломки, чей металл влезает в груз (fit), и все прочие (any) — ближайшие безопасные, не взятые другим
		cPosition fitGoal (-1, -1), fitSite (-1, -1), anyGoal (-1, -1), anySite (-1, -1);
		if (cargo < cap)
		{
			long long fitKey = std::numeric_limits<long long>::max(), anyKey = fitKey;
			for (size_t k = 0; k < sites.size() && k < 16; ++k)
			{
				const cPosition s = sites[k];
				if (isTaken (s)) continue;
				const auto it = rubble_.find (s);
				if (it == rubble_.end()) continue;
				const bool big = it->second.big;
				bool ok = true;
				for (const cPosition& c : areaOf (s, big))
					if (!view.isValidPosition (c) || world.threatFor (b, c, kLayer) > 0 || (c != up && !view.possiblePlace (b, c))) ok = false;
				if (!ok) continue;
				cPosition entry (-1, -1);
				long long ed = std::numeric_limits<long long>::max();
				for (const cPosition& c : areaOf (s, big))
				{
					if (isReserved (c) && c != up) continue;
					const long long d2 = (c - up).l2NormSquared();
					if (d2 < ed)
					{
						ed = d2;
						entry = c;
					}
				}
				if (entry.x() < 0) continue;
				// порядок сайтов — приоритет (у базы: на ресурсе, ближе к центру базы; поле: металл, близость к армии), дальность — вторым
				const long long key = static_cast<long long> (k) * 4 + std::min<long long> (3, ed / 100);
				if (key < anyKey)
				{
					anyKey = key;
					anyGoal = entry;
					anySite = s;
				}
				if (cargo + it->second.value <= cap && key < fitKey)
				{
					fitKey = key;
					fitGoal = entry;
					fitSite = s;
				}
			}
		}
		// металл класть некуда (вмещающих обломков нет) — отдать любому получателю или в склад базы
		if (cargo > 0 && fitSite.x() < 0)
			receiver = chooseReceiver (player, world, roles, main, b, true, hungry, toBuilding);
		const bool canDeliver = receiver != kNoUnit && cargo > 0;

		// 2) стоит на обломках — расчистить (металл влезает, или груз пуст, или отдать некому)
		if (const cBuilding* rubble = clearableUnder (view, world, b, cargo, canDeliver))
		{
			pushClear (player, b, *rubble, cargo, siteName, 1, out);
			reserved.push_back (up);
			continue;
		}

		// 3) голодному — с грузом от kFeedMin (его держат у армии, feedsNear); вмещающих обломков нет или груз полон — любому получателю
		const bool deliverNow = canDeliver && ((hungry && cargo >= kFeedMin) || fitSite.x() < 0 || cargo >= cap);
		if (deliverNow)
		{
			const cUnit* recv = model.getUnitFromID (receiver);
			cPosition best (-1, -1);
			long long bestD = std::numeric_limits<long long>::max();
			for (const cPosition& rc : recv->getPositions())
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const cPosition c = rc + cPosition (dx, dy);
						if (!view.isValidPosition (c) || !recv->isNextTo (c)) continue;
						if (c != up && !view.possiblePlace (b, c)) continue;
						if (world.threatFor (b, c, kLayer) > 0 || (isReserved (c) && c != up)) continue;
						const long long d2 = (c - up).l2NormSquared();
						if (d2 < bestD)
						{
							bestD = d2;
							best = c;
						}
					}
			if (best.x() >= 0 && best != up)
				if (auto path = pathTo (b, view, world, best); !path.empty())
				{
					if (recv->isNextTo (path.back()))
					{
						sJob j;
						j.task = eTask::Deliver;
						j.receiver = receiver;
						j.since = turn_;
						jobs_[b.getId()] = j;
						wantPass_ = true;
					}
					pushMove (b, std::move (path), std::string ("bulldozer: to ") + (toBuilding ? "base" : "receiver") + " with metal");
					continue;
				}
		}

		// 4) к обломкам: вмещающим, а отдать некому — к любым
		const cPosition goal = fitSite.x() >= 0 ? fitGoal : (!canDeliver ? anyGoal : cPosition (-1, -1));
		const cPosition goalSite = fitSite.x() >= 0 ? fitSite : anySite;
		if (goal.x() >= 0)
		{
			if (auto path = pathTo (b, view, world, goal); !path.empty())
			{
				taken.push_back (goalSite);
				if (inArea (path.back(), goalSite, rubble_.at (goalSite).big))
				{
					sJob j;
					j.task = forBase ? eTask::ClearBase : eTask::ClearField;
					j.target = goalSite;
					j.since = turn_;
					jobs_[b.getId()] = j;
					wantPass_ = true;
				}
				pushMove (b, std::move (path), std::string ("bulldozer: to rubble (") + siteName + ")");
				continue;
			}
		}

		// 5) дела нет: при армии — у хвоста армии (вне «удара с подъезда»), иначе — у центра базы
		const cPosition anchor = (!forBase && armyOut && armyAnchor.x() >= 0) ? armyAnchor : player.getLandingPos();
		const int hold = (!forBase && armyOut) ? kEscortHold : 6;
		if (anchor.x() >= 0 && cheb (up, anchor) > hold)
		{
			cPosition near;
			if (world.nearestSafeCell (b, view, anchor, hold, up, near, kLayer) && near != up && !isReserved (near))
				if (auto path = pathTo (b, view, world, near); !path.empty())
				{
					pushMove (b, std::move (path), (!forBase && armyOut) ? "bulldozer: follows army" : "bulldozer: to base");
					continue;
				}
		}
		reserved.push_back (up);
	}
}

//------------------------------------------------------------------------------
std::vector<cOrder> cBulldozers::planPass (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
	const cRoleBook& roles, const cSubBase* main, int pass)
{
	std::vector<cOrder> out;
	wantPass_ = false;
	// доехавшие: расчистить или отдать металл
	for (auto it = jobs_.begin(); it != jobs_.end();)
	{
		const cVehicle* b = model.getVehicleFromID (it->first);
		if (b == nullptr || b->getOwner() != &player || !freeBulldozer (*b) || b->isUnitMoving())
		{
			it = jobs_.erase (it);
			continue;
		}
		const sJob& j = it->second;
		if (j.task == eTask::Deliver)
		{
			pushTransfer (model, player, roles, *b, j.receiver, pass, out);
		}
		else if (j.task == eTask::ClearBase || j.task == eTask::ClearField)
		{
			if (const cBuilding* rubble = clearableUnder (view, world, *b, b->getStoredResources(), false);
				rubble != nullptr && inArea (b->getPosition(), j.target, rubble->getIsBig()))
				pushClear (player, *b, *rubble, b->getStoredResources(), j.task == eTask::ClearBase ? "base" : "field", pass, out);
		}
		it = jobs_.erase (it);
	}
	// получатели металла бульдозера: ремонт и перезарядка соседей сразу (металл пришёл в прошлом заходе)
	for (const unsigned int id : fedThisTurn_)
	{
		const cVehicle* r = model.getVehicleFromID (id);
		if (r == nullptr || r->getOwner() != &player || r->isDisabled() || r->isUnitLoaded() || r->getStoredResources() <= 0) continue;
		const bool repair = r->getStaticUnitData().canRepair;
		const bool rearm = r->getStaticUnitData().canRearm;
		std::vector<std::tuple<long long, unsigned int>> needy;
		for (const auto& v : player.getVehicles())
		{
			if (v.get() == r) continue;
			if (repair && r->canSupply (v.get(), eSupplyType::REPAIR))
				needy.emplace_back (static_cast<long long> (v->data.getHitpoints()) * 1000 / std::max (1, v->data.getHitpointsMax()), v->getId());
			else if (rearm && r->canSupply (v.get(), eSupplyType::REARM))
				needy.emplace_back (static_cast<long long> (v->data.getAmmo()) * 1000 / std::max (1, v->data.getAmmoMax()), v->getId());
		}
		std::sort (needy.begin(), needy.end());
		int metal = r->getStoredResources();
		for (const auto& [share, vid] : needy)
		{
			(void) share;
			if (metal <= 0) break;
			const cVehicle* d = model.getVehicleFromID (vid);
			cOrder o = makeOrder (repair ? eOrderType::Repair : eOrderType::Rearm, player.getId(), id,
				"pass " + std::to_string (pass) + (repair ? ": repair with metal from rubble" : ": rearm with metal from rubble"));
			o.unitId2 = vid;
			out.push_back (std::move (o));
			if (repair)
			{
				const int hpPerMetal = std::max (1, 4 * d->data.getHitpointsMax() / std::max (1, d->data.getBuildCost()));
				metal -= (d->data.getHitpointsMax() - d->data.getHitpoints() + hpPerMetal - 1) / hpPerMetal;
			}
			else
				--metal;
		}
	}
	fedThisTurn_.clear();
	// после расчистки и передачи — ничего больше не ждём; новые задания — в следующем ходу
	(void) roles;
	return out;
}

//------------------------------------------------------------------------------
void cBulldozers::noteOrders (const cModel& model, const cPlayer& player, const std::vector<cOrder>& orders)
{
	const auto& ud = *model.getUnitsData();
	for (const auto& o : orders)
	{
		if (o.playerId != player.getId()) continue;
		if (o.type == eOrderType::Build || o.type == eOrderType::BuildPath)
		{
			const sID id (o.buildingType.firstPart, o.buildingType.secondPart);
			if (!ud.isValidId (id)) continue;
			const bool big = ud.getStaticUnitData (id).buildingData.isBig;
			for (const cPosition& c : areaOf (o.position, big))
			{
				const auto it = clearedBy_.find (c);
				if (it == clearedBy_.end()) continue;
				const auto key = std::make_tuple (o.position.y(), o.position.x(), o.unitId);
				if (buildsSeen_.insert (key).second)
				{
					++totals_.buildsOnCleared;
					events_.push_back ({{"event", "bulldozer"}, {"level", "op"}, {"what", "build on cleared"}, {"unitId", o.unitId},
						{"pos", posJ (o.position)}, {"building", std::to_string (id.firstPart) + "." + std::to_string (id.secondPart)},
						{"clearedTurn", it->second}});
				}
				break;
			}
		}
		else if (o.type == eOrderType::Repair || o.type == eOrderType::Rearm)
		{
			auto it = fedMetal_.find (o.unitId);
			if (it == fedMetal_.end() || it->second <= 0) continue;
			if (o.type == eOrderType::Repair)
			{
				++totals_.repairsFromIt;
				if (const cVehicle* d = model.getVehicleFromID (o.unitId2))
				{
					const int hpPerMetal = std::max (1, 4 * d->data.getHitpointsMax() / std::max (1, d->data.getBuildCost()));
					it->second -= std::max (1, (d->data.getHitpointsMax() - d->data.getHitpoints() + hpPerMetal - 1) / hpPerMetal);
				}
				else
					--it->second;
			}
			else
			{
				++totals_.rearmsFromIt;
				--it->second;
			}
		}
	}
}

//------------------------------------------------------------------------------
std::vector<nlohmann::json> cBulldozers::takeEvents()
{
	auto e = std::move (events_);
	events_.clear();
	return e;
}

} // namespace ai
