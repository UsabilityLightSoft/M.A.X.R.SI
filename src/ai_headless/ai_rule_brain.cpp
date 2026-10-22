/***************************************************************************
 * ai_rule_brain.cpp — алгоритмический мозг: микро (этап 3) + заглушки
 * оперативки/стратегии (этап 4).
 ***************************************************************************/

#include "ai_rule_brain.h"

#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/id.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"
#include "utility/log.h"
#include "utility/mathtools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace ai
{

namespace
{

// --- вспомогательные структуры ---------------------------------------------

struct sEnemy
{
	const cUnit* unit = nullptr;
	cPosition pos;
	int hitpoints = 0;
	int hitpointsMax = 0;
	int damage = 0;
	int armor = 0;
	int range = 0;
	int shots = 0;
};

// shared_ptr<const cPlayer> для cMapView (туман войны)
std::shared_ptr<const cPlayer> sharedPlayerOf (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id)
			return p;
	return nullptr;
}

// собрать видимых врагов (техника + здания чужих игроков в зоне видимости)
void collectVisibleEnemies (const cPlayer& self, const cModel& model, const cMapView& view,
	std::vector<sEnemy>& out)
{
	const auto& map = model.getMap();
	for (const auto& player : model.getPlayerList())
	{
		if (player->getId() == self.getId()) continue;
		for (const auto& v : player->getVehicles())
		{
			if (!view.canSeeUnit (*v)) continue;
			sEnemy e;
			e.unit = v.get();
			e.pos = v->getPosition();
			e.hitpoints = v->data.getHitpoints();
			e.hitpointsMax = v->data.getHitpointsMax();
			e.damage = v->data.getDamage();
			e.armor = v->data.getArmor();
			e.range = v->data.getRange();
			e.shots = v->data.getShots();
			out.push_back (e);
		}
		for (const auto& b : player->getBuildings())
		{
			if (!view.canSeeUnit (*b)) continue;
			sEnemy e;
			e.unit = b.get();
			e.pos = b->getPosition();
			e.hitpoints = b->data.getHitpoints();
			e.hitpointsMax = b->data.getHitpointsMax();
			e.damage = b->data.getDamage();
			e.armor = b->data.getArmor();
			e.range = b->data.getRange();
			e.shots = b->data.getShots();
			out.push_back (e);
		}
	}
}

// ближайший враг
const sEnemy* nearestEnemy (const std::vector<sEnemy>& enemies, const cPosition& from)
{
	const sEnemy* best = nullptr;
	int bestDist = std::numeric_limits<int>::max();
	for (const auto& e : enemies)
	{
		const int d = (e.pos - from).l2NormSquared();
		if (d < bestDist)
		{
			bestDist = d;
			best = &e;
		}
	}
	return best;
}

// урон за выстрел с учётом брони цели (мин. 1 — как в cUnit::calcHealth)
int effectiveDamagePerShot (int damage, int armor)
{
	return std::max (1, damage - armor);
}

// юнит свободен для приказа (не занят стройкой/движением/загрузкой/минами)
bool isIdle (const cVehicle& v)
{
	if (v.isDisabled()) return false;
	if (v.isUnitLoaded()) return false;
	if (v.isUnitMoving() || v.getMoveJob() != nullptr) return false;
	if (v.isUnitBuildingABuilding() || v.isUnitClearing()
		|| v.isUnitLayingMines() || v.isUnitClearingMines()) return false;
	return true;
}

// боевой юнит: может атаковать, есть дальность и выстрелы
bool isCombatUnit (const cVehicle& v)
{
	const auto& sd = v.getStaticUnitData();
	const auto& dd = v.data;
	return sd.canAttack > 0 && dd.getRange() > 0 && dd.getShots() > 0;
}

// Скоординированный фокус-огонь на уровне игрока: жадное назначение целей.
// Возвращает для каждого атакующего (в порядке `attackers`) индекс цели в `enemies`
// или -1, если в радиусе атаки целей нет. Детерминировано (без RNG).
//
// Приоритет цели (жадный выбор каждый шаг):
//   1) цель, которую можно УБИТЬ суммарным уроном доступных атакующих (концентрация огня);
//   2) раненые (hp < max) — добивание, не даём отойти/отремонтироваться;
//   3) по убыванию угрозы (damage × shots × range / близость к нашим силам).
// Внутри тира опаснее враг — раньше; при равной угрозе — меньше остаток hp.
std::vector<int> coordinateFocusFire (const std::vector<const cVehicle*>& attackers,
	const std::vector<sEnemy>& enemies, const cMapView& view)
{
	const int A = static_cast<int> (attackers.size());
	const int E = static_cast<int> (enemies.size());
	std::vector<int> target (A, -1);
	if (A == 0 || E == 0) return target;

	// матрица достижимости «атакующий → цель» (цель в радиусе атаки)
	std::vector<std::vector<bool>> canHit (A, std::vector<bool> (E, false));
	for (int a = 0; a < A; ++a)
		for (int e = 0; e < E; ++e)
			canHit[a][e] = attackers[a]->canAttackObjectAt (enemies[e].pos, view, false);

	// угроза каждого врага (damage×shots×range, делим на близость к нашим силам)
	std::vector<double> threat (E, 0.0);
	for (int e = 0; e < E; ++e)
	{
		long long minDist = std::numeric_limits<long long>::max();
		for (int a = 0; a < A; ++a)
		{
			const long long d = (enemies[e].pos - attackers[a]->getPosition()).l2NormSquared();
			if (d < minDist) minDist = d;
		}
		threat[e] = static_cast<double> (enemies[e].damage) * enemies[e].shots
			* (enemies[e].range + 1)
			/ (1.0 + std::sqrt (static_cast<double> (std::max (1LL, minDist))));
	}

	std::vector<bool> attackerFree (A, true);
	std::vector<int> remainingHp (E);
	for (int e = 0; e < E; ++e) remainingHp[e] = enemies[e].hitpoints;

	// «убиваемость» считаем суммой эффективного урона доступных атакующих
	// (эффективная живучесть = hp с поправкой на броню через effDamage/выстрел).
	while (true)
	{
		int bestE = -1;
		double bestScore = -1.0;
		for (int e = 0; e < E; ++e)
		{
			if (remainingHp[e] <= 0) continue;
			long long totalDmg = 0;
			bool anyFree = false;
			for (int a = 0; a < A; ++a)
			{
				if (!attackerFree[a] || !canHit[a][e]) continue;
				anyFree = true;
				// один приказ Attack = один выстрел (см. cAttackJob::run), поэтому
				// доступный урон — по одному выстрелу с каждого свободного атакующего.
				totalDmg += effectiveDamagePerShot (attackers[a]->data.getDamage(), enemies[e].armor);
			}
			if (!anyFree) continue;
			const bool killable = totalDmg >= remainingHp[e];
			const bool wounded = enemies[e].hitpoints < enemies[e].hitpointsMax;
			const double hpRatio = enemies[e].hitpointsMax > 0
				? static_cast<double> (remainingHp[e]) / enemies[e].hitpointsMax : 1.0;
			// тир 1 — убиваемое (концентрация); тир 2 — раненые (добивание);
			// внутри тира — угроза (опасных вперёд), остаток hp — финальный тай-брейк.
			const double score = (killable ? 1e12 : 0.0)
				+ (wounded ? 1e9 : 0.0)
				+ threat[e] * 1e3
				+ (1.0 - hpRatio);
			if (score > bestScore)
			{
				bestScore = score;
				bestE = e;
			}
		}
		if (bestE < 0) break;

		// лучший атакующий для цели: больше эффективного урона за выстрел,
		// при равенстве — ближе, затем меньший id (детерминизм)
		int bestA = -1;
		long long bestEff = -1;
		long long bestDist = 0;
		unsigned int bestId = 0;
		for (int a = 0; a < A; ++a)
		{
			if (!attackerFree[a] || !canHit[a][bestE]) continue;
			const long long eff = effectiveDamagePerShot (attackers[a]->data.getDamage(), enemies[bestE].armor);
			const long long d = (enemies[bestE].pos - attackers[a]->getPosition()).l2NormSquared();
			const unsigned int id = attackers[a]->getId();
			if (bestA < 0 || eff > bestEff || (eff == bestEff && (d < bestDist || (d == bestDist && id < bestId))))
			{
				bestEff = eff;
				bestDist = d;
				bestId = id;
				bestA = a;
			}
		}
		if (bestA < 0)
		{
			// не должно случиться (anyFree гарантирует), но страхуемся от зацикливания
			remainingHp[bestE] = 0;
			continue;
		}

		attackerFree[bestA] = false;
		target[bestA] = bestE;
		// один приказ Attack = один выстрел: списываем урон одного выстрела
		remainingHp[bestE] -= effectiveDamagePerShot (attackers[bestA]->data.getDamage(), enemies[bestE].armor);
	}

	return target;
}

// конвертация forward_list-пути (front = следующий шаг) в вектор приказа
std::vector<cPosition> pathToVector (const std::forward_list<cPosition>& path)
{
	std::vector<cPosition> res;
	for (const auto& p : path)
		res.push_back (p);
	return res;
}

// --- отход за строй + ремонт в бою (микро) ----------------------------------

constexpr int kRetreatBehind = 4;  // на сколько клеток за линию здоровых союзников
constexpr int kRetreatShort = 6;   // короткий шаг в тыл, когда нет строя
constexpr int kRetreatMax = 10;    // максимум клеток отхода (Чебышёв) — отход КОРОТКИЙ
constexpr int kSnapRadius = 6;     // радиус привязки к проходимой клетке

// нужно ли отступать: hp% против порога, растущего при опасности.
bool shouldRetreat (const cVehicle& v, const std::vector<sEnemy>& enemies)
{
	const int hpMax = v.data.getHitpointsMax();
	if (hpMax <= 0) return false;
	const int hpPct = v.data.getHitpoints() * 100 / hpMax;

	if (hpPct < 30) return true; // критический урон — всегда отходим

	int threshold = 50; // базовый порог
	if (v.isBeingAttacked()) threshold = 70; // под обстрелом — раньше
	for (const auto& e : enemies)
	{
		const long long r = e.range;
		if ((e.pos - v.getPosition()).l2NormSquared() <= r * r)
		{
			threshold = std::max (threshold, 70); // враг достаёт нас
			break;
		}
	}
	return hpPct < threshold;
}

// знаковое направление (8 сторон) из вектора
cPosition signDirOf (const cPosition& v)
{
	return cPosition (v.x() > 0 ? 1 : (v.x() < 0 ? -1 : 0),
		v.y() > 0 ? 1 : (v.y() < 0 ? -1 : 0));
}

// ближайшая к near проходимая клетка (расширяющееся кольцо, детерминированно)
bool snapPassable (const cVehicle& v, const cMapView& view, const cPosition& near,
	int maxR, cPosition& out)
{
	int bestDist = std::numeric_limits<int>::max();
	bool any = false;
	cPosition best;
	for (int r = 0; r <= maxR; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
				const cPosition p = near + cPosition (dx, dy);
				if (!view.isValidPosition (p)) continue;
				if (!view.possiblePlace (v, p)) continue;
				const int d = (p - near).l2NormSquared();
				if (d < bestDist) { bestDist = d; best = p; any = true; }
			}
	if (!any) return false;
	out = best;
	return true;
}

// Точка отхода: КОРОТКО, за спины здоровых союзников (не до базы).
// found=false → вызывающий откатывается на старое поведение (к точке высадки).
cPosition findRetreatTarget (const cVehicle& self, const cPlayer& player,
	const std::vector<sEnemy>& enemies, const cMapView& view, bool& found)
{
	const cPosition selfPos = self.getPosition();
	const cPosition landing = player.getLandingPos();

	// центроид видимых врагов
	cPosition enemyC (-1, -1);
	{
		long long sx = 0, sy = 0; int n = 0;
		for (const auto& e : enemies) { sx += e.pos.x(); sy += e.pos.y(); ++n; }
		if (n > 0) enemyC = cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n));
	}

	// центроид ЗДОРОВЫХ боевых союзников (линия строя)
	cPosition allyC (-1, -1);
	{
		long long sx = 0, sy = 0; int n = 0;
		for (const auto& v : player.getVehicles())
		{
			if (v.get() == &self) continue;
			if (!isCombatUnit (*v)) continue;
			if (v->data.getHitpoints() * 2 < v->data.getHitpointsMax()) continue; // только здоровые
			sx += v->getPosition().x(); sy += v->getPosition().y(); ++n;
		}
		if (n > 0) allyC = cPosition (static_cast<int> (sx / n), static_cast<int> (sy / n));
	}

	// направление «в тыл» (от фронта)
	cPosition rear;
	if (enemyC.x() >= 0 && allyC.x() >= 0)      rear = signDirOf (allyC - enemyC); // за линию союзников
	else if (enemyC.x() >= 0)                   rear = signDirOf (selfPos - enemyC); // прочь от врага
	else                                        rear = signDirOf (landing - selfPos); // к своему тылу

	if (rear.x() == 0 && rear.y() == 0) { found = false; return cPosition (-1, -1); }

	// целевая клетка: за спинами союзников, иначе короткий шаг в тыл
	cPosition target;
	if (enemyC.x() >= 0 && allyC.x() >= 0)
		target = allyC + rear * kRetreatBehind;
	else
		target = selfPos + rear * kRetreatShort;

	// ограничение: отход КОРОТКИЙ (не дальше kRetreatMax клеток по Чебышёву)
	if (std::max (std::abs (target.x() - selfPos.x()), std::abs (target.y() - selfPos.y())) > kRetreatMax)
		target = selfPos + rear * kRetreatMax;

	// защита: не отходим В СТОРОНУ врага
	if (enemyC.x() >= 0
		&& (target - enemyC).l2NormSquared() <= (selfPos - enemyC).l2NormSquared())
	{
		const cPosition away = signDirOf (selfPos - enemyC);
		if (away.x() == 0 && away.y() == 0) { found = false; return cPosition (-1, -1); }
		target = selfPos + away * kRetreatShort;
	}

	if (!snapPassable (self, view, target, kSnapRadius, target)) { found = false; return cPosition (-1, -1); }
	found = true;
	return target;
}

// Опасно ли ремонтнику приближаться к цели (не лезть под обстрел).
bool repairUnsafe (const cVehicle& repairer, const cVehicle& target,
	const std::vector<sEnemy>& enemies)
{
	if (repairer.isBeingAttacked()) return true;
	if (target.isBeingAttacked()) return true;
	for (const auto& e : enemies)
	{
		const long long r = e.range;
		if ((e.pos - target.getPosition()).l2NormSquared() <= r * r) return true;
		if ((e.pos - repairer.getPosition()).l2NormSquared() <= r * r) return true;
	}
	return false;
}

// приоритет ремонта: боевой юнит (танк) важнее; внутри — больше % нехватки hp;
// тай-брейк по id (детерминизм).
bool repairMoreUrgent (const cVehicle* a, const cVehicle* b)
{
	const int va = isCombatUnit (*a) ? 1 : 0;
	const int vb = isCombatUnit (*b) ? 1 : 0;
	if (va != vb) return va > vb;
	const int missA = a->data.getHitpointsMax() - a->data.getHitpoints();
	const int missB = b->data.getHitpointsMax() - b->data.getHitpoints();
	const int pa = a->data.getHitpointsMax() > 0 ? missA * 100 / a->data.getHitpointsMax() : 0;
	const int pb = b->data.getHitpointsMax() > 0 ? missB * 100 / b->data.getHitpointsMax() : 0;
	if (pa != pb) return pa > pb;
	return a->getId() < b->getId();
}

// --- кэш разведки за ход ---------------------------------------------------
// Узкое место: findNearestUnexplored делал ПОЛНЫЙ скан карты (O(клетки)) на
// КАЖДЫЙ боевой юнит и разведчика КАЖДЫЙ ход — O(юниты×клетки). Здесь список
// неразведанных клеток строится ОДИН раз за ход (общий для всех юнитов игрока),
// а проходимость кэшируется по типу стат. данных юнита (cMap::possiblePlace
// зависит только от стат. данных + владельца + состояния карты — у всех юнитов
// игрока владелец один, поэтому по указателю стат. данных кэш корректен).
// Результат findNearestUnexplored БАЙТ-В-БАЙТ совпадает со старым полным
// сканом: тот же порядок обхода (x-major), те же проверки, то же строгое
// сравнение дистанции (<, поэтому ничьи — за первой клеткой в порядке скана).

struct sScoutCache
{
	std::vector<cPosition> unexplored;                                            // неразведанные клетки (порядок полного скана)
	std::unordered_map<const cStaticUnitData*, std::vector<char>> passable;       // проходимость по типу юнита
};

void buildScoutCache (const cPlayer& owner, const cMap& map, sScoutCache& cache)
{
	const cPosition size = map.getSize();
	cache.unexplored.clear();
	cache.unexplored.reserve (static_cast<size_t> (size.x()) * static_cast<size_t> (size.y()));
	for (int x = 0; x < size.x(); ++x)
		for (int y = 0; y < size.y(); ++y)
		{
			const cPosition p (x, y);
			if (!owner.hasResourceExplored (p))
				cache.unexplored.push_back (p);
		}
	cache.passable.clear();
}

const std::vector<char>& scoutPassable (sScoutCache& cache, const cVehicle& unit, const cMapView& view)
{
	auto it = cache.passable.find (&unit.getStaticUnitData());
	if (it == cache.passable.end())
	{
		std::vector<char> b (cache.unexplored.size(), 0);
		for (size_t i = 0; i < cache.unexplored.size(); ++i)
			b[i] = view.possiblePlace (unit, cache.unexplored[i]) ? 1 : 0;
		it = cache.passable.emplace (&unit.getStaticUnitData(), std::move (b)).first;
	}
	return it->second;
}

// ближайшая неразведанная проходимая клетка (для surveyor и боевого «скаута»)
cPosition findNearestUnexplored (const cVehicle& surveyor, const cMapView& view,
	sScoutCache& cache, bool& found)
{
	const auto& pass = scoutPassable (cache, surveyor, view);
	found = false;
	cPosition best;
	int bestDist = std::numeric_limits<int>::max();
	const cPosition from = surveyor.getPosition();
	for (size_t i = 0; i < cache.unexplored.size(); ++i)
	{
		if (!pass[i]) continue;
		const cPosition& p = cache.unexplored[i];
		const int d = (p - from).l2NormSquared();
		if (d < bestDist)
		{
			bestDist = d;
			best = p;
			found = true;
		}
	}
	return best;
}

// --- оперативка/стратегия (этап 4) -----------------------------------------

// число своих зданий данного типа
int countBuildings (const cPlayer& player, const sID& type)
{
	int n = 0;
	for (const auto& b : player.getBuildings())
		if (b->getStaticUnitData().ID == type) ++n;
	return n;
}

// число своих юнитов данного типа
int countVehicles (const cPlayer& player, const sID& type)
{
	int n = 0;
	for (const auto& v : player.getVehicles())
		if (v->getStaticUnitData().ID == type) ++n;
	return n;
}

// свободная клетка для здания возле точки near (расширяющееся кольцо)
// big-здания занимают 2x2 — проверяем весь след.
cPosition findBuildSpot (const cModel& model, const cPlayer& player,
	const cStaticUnitData& buildingData, const cPosition& near)
{
	const auto& map = model.getMap();
	const cPosition size = map->getSize();
	const bool big = buildingData.buildingData.isBig;
	for (int r = 0; r < 40; ++r)
	{
		for (int dy = -r; dy <= r; ++dy)
		{
			for (int dx = -r; dx <= r; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue; // только кольцо
				const cPosition p = near + cPosition (dx, dy);
				if (!map->isValidPosition (p)) continue;
				if (!map->possiblePlaceBuilding (buildingData, p, &player)) continue;
				if (big)
				{
					if (!map->possiblePlaceBuilding (buildingData, p + cPosition (1, 0), &player)) continue;
					if (!map->possiblePlaceBuilding (buildingData, p + cPosition (0, 1), &player)) continue;
					if (!map->possiblePlaceBuilding (buildingData, p + cPosition (1, 1), &player)) continue;
				}
				return p;
			}
		}
	}
	return cPosition (-1, -1);
}

// своё здание-источник ресурса (для пополнения конструктора)
const cBuilding* findResourceSource (const cPlayer& player, eResourceType resType)
{
	const cBuilding* best = nullptr;
	int bestAmt = -1;
	for (const auto& b : player.getBuildings())
	{
		if (b->getStaticUnitData().storeResType != resType) continue;
		const int amt = b->getStoredResources();
		if (amt > bestAmt)
		{
			bestAmt = amt;
			best = b.get();
		}
	}
	return best;
}

// клетка «зажата» — ни один из 4 ортогональных соседей не проходим (юнит не выедет)
bool isBoxedIn (const cModel& model, const cMapView& view, const cVehicle& v, const cPosition& pos)
{
	const cPosition dirs[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	for (const auto& d : dirs)
	{
		const cPosition n = pos + d;
		if (!model.getMap()->isValidPosition (n)) continue;
		if (view.possiblePlace (v, n)) return false;
	}
	return true;
}

// соседняя свободная клетка для выезда техники (завершение постройки здания)
cPosition findVehicleEscape (const cModel& model, const cVehicle& v)
{
	const cMapView view (model.getMap(), nullptr);
	for (int r = 1; r <= 3; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				const cPosition p = v.getPosition() + cPosition (dx, dy);
				if (!model.getMap()->isValidPosition (p)) continue;
				if (!v.isNextTo (p)) continue;
				if (!model.getMap()->possiblePlace (v, p, false)) continue;
				if (isBoxedIn (model, view, v, p)) continue;
				return p;
			}
	return cPosition (-1, -1);
}

// соседняя свободная клетка для выезда юнита из фабрики
cPosition findFactoryEscape (const cModel& model, const cBuilding& factory,
	const cStaticUnitData& vehicleData, const cPlayer& player)
{
	const auto& map = model.getMap();
	for (int r = 1; r <= 3; ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				const cPosition p = factory.getPosition() + cPosition (dx, dy);
				if (!map->isValidPosition (p)) continue;
				if (!factory.isNextTo (p)) continue;
				if (!map->possiblePlaceVehicle (vehicleData, p, &player)) continue;
				// не выдавать юнит в запертую клетку: нужен свободный сосед для выезда
				bool hasFreeNeighbor = false;
				const cPosition dirs[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (const auto& d : dirs)
				{
					const cPosition n = p + d;
					if (!map->isValidPosition (n)) continue;
					if (map->possiblePlaceVehicle (vehicleData, n, &player)) { hasFreeNeighbor = true; break; }
				}
				if (!hasFreeNeighbor) continue;
				return p;
			}
	return cPosition (-1, -1);
}

// --- экономика: константы типов и чистые хелперы (этап 4) -------------------

const sID kIdFactory (1, 11);
const sID kIdMine (1, 22);
const sID kIdEnergy (1, 7);   // energy_big (6 энергии); energy_small (1) всегда в базе
const sID kIdHabitat (1, 19);
const sID kIdResearch (1, 27);
const sID kIdTank (0, 32);
const sID kIdKonstrukt (0, 19);

constexpr int kTargetMines = 3;
constexpr int kTargetConstructors = 3; // конструкторов в парке (для параллельной экономики)

// число конструкторов, прямо сейчас строящих здание данного типа (ещё не завершили)
int countConstructorsBuilding (const cPlayer& player, const sID& type)
{
	int n = 0;
	for (const auto& v : player.getVehicles())
		if (v->isUnitBuildingABuilding() && v->getBuildingType() == type) ++n;
	return n;
}

// Чистая функция выбора следующего здания по эффективным счётчикам
// (построено + строится + назначено в текущем проходе). Детерминирована, без RNG.
// Порядок: 1 фабрика → 3 шахты (нужна нефть) → энергия → до 3 фабрик (рост потока
// танков) → жильё → исследование → фабрики 4-5+ с энергобалансом
// (1 energy_small + 6·energy_big покрывает шахты+исслед.+фабрики).
sID chooseNextBuilding (int nFactory, int nMine, int nEnergy, int nHabitat, int nResearch)
{
	if (nFactory < 1) return kIdFactory;
	if (nMine < kTargetMines) return kIdMine;   // 3 шахты ДО энергии: нужна нефть (иначе engine зависает в checkOil/checkEnergy)
	if (nEnergy < 1) return kIdEnergy;
	if (nFactory < 3) return kIdFactory;         // наращиваем до 3 фабрик ДО жилья/исследования (рост потока танков)
	if (nHabitat < 1) return kIdHabitat;
	if (nResearch < 1) return kIdResearch;

	// дальше фабрики 4-5+ с энергобалансом (1 energy_small + 6·energy_big
	// покрывает шахты+исслед.+фабрики; следующая фабрика не влезет — добавить энергии)
	const int energyTotal = 1 + 6 * nEnergy;
	const int consumers = nMine + nResearch + nFactory;
	if (consumers + 1 > energyTotal) return kIdEnergy;
	return kIdFactory;
}

// Максимальная скорость стройки, которую конструктор может себе позволить ПРЯМО
// СЕЙЧАС. cVehicle::calcTurboBuild выдаёт rounds[s]>0 только если текущего
// металла хватает на эту скорость (каждые +4 металла — минус 1 ход), поэтому
// «максимальное s с rounds[s]>0» и есть доступный турбо-режим. Вызывать только
// при getStoredResources() >= buildCost (иначе costs[] не инициализируется).
int maxFeasibleBuildSpeed (const cVehicle& constructor, int buildCost)
{
	std::array<int, 3> rounds{}, costs{};
	constructor.calcTurboBuild (rounds, costs, buildCost);
	for (int s = 2; s >= 1; --s)
		if (rounds[s] > 0)
			return s;
	return 0;
}

} // namespace

//------------------------------------------------------------------------------
std::vector<cOrder> cRuleBrain::decideTurn (int playerId, const cModel& model)
{
	std::vector<cOrder> orders;
	strategicDecisions (playerId, model, orders);
	operationalDecisions (playerId, model, orders);
	microDecisions (playerId, model, orders);
	return orders;
}

//------------------------------------------------------------------------------
// Стратегия (этап 4): фокус исследований + техпуть. Пока — статичные правила.
void cRuleBrain::strategicDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;

	// есть работающие исследовательские центры — направляем их:
	// урон → броня → дальность → прочность → дальше по убыванию пользы.
	const int total = player->getResearchCentersWorkingTotal();
	if (total <= 0) return;

	using RA = cResearch::eResearchArea;
	const std::array<RA, 8> priority = {
		RA::AttackResearch, RA::ArmorResearch, RA::RangeResearch, RA::HitpointsResearch,
		RA::ShotsResearch, RA::SpeedResearch, RA::ScanResearch, RA::CostResearch};

	cOrder o;
	o.type = eOrderType::ChangeResearch;
	o.playerId = playerId;
	o.level = "strategy";
	o.reason = "research focus: damage>armor>range";
	int remaining = total;
	for (RA area : priority)
	{
		const int idx = static_cast<int> (area);
		const int assign = remaining > 0 ? 1 : 0; // пока по 1 центру на область по приоритету
		o.researchAreas[idx] = assign;
		if (assign) --remaining;
	}
	out.push_back (std::move (o));
}

//------------------------------------------------------------------------------
// Оперативка (этап 4): стройка, производство, снабжение конструкторов.
void cRuleBrain::operationalDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;

	const auto& unitsData = model.getUnitsData();

	// 1) запустить все свои здания, что могут работать, но не работают
	for (const auto& b : player->getBuildings())
	{
		if (b->getStaticData().canWork && !b->isUnitWorking())
		{
			cOrder o;
			o.type = eOrderType::StartWork;
			o.playerId = playerId;
			o.unitId = b->getId();
			o.level = "op";
			o.reason = "start work";
			out.push_back (std::move (o));
		}
	}

	// 2) конструкторы: каждый строит СВОЁ здание (параллельно), пополняясь металлом
	//    под турбо-стройку. Приоритет — чистая функция chooseNextBuilding.
	const int nFactory = countBuildings (*player, kIdFactory);
	const int nMine = countBuildings (*player, kIdMine);
	const int nEnergy = countBuildings (*player, kIdEnergy);
	const int nHabitat = countBuildings (*player, kIdHabitat);
	const int nResearch = countBuildings (*player, kIdResearch);

	// уже строящиеся (не завершённые) здания — чтобы свободные конструкторы
	// не дублировали их и не простаивали
	const int ipFactory = countConstructorsBuilding (*player, kIdFactory);
	const int ipMine = countConstructorsBuilding (*player, kIdMine);
	const int ipEnergy = countConstructorsBuilding (*player, kIdEnergy);
	const int ipHabitat = countConstructorsBuilding (*player, kIdHabitat);
	const int ipResearch = countConstructorsBuilding (*player, kIdResearch);

	// назначенные в этом проходе (для параллельных конструкторов — разные здания)
	int asFactory = 0, asMine = 0, asEnergy = 0, asHabitat = 0, asResearch = 0;

	for (const auto& v : player->getVehicles())
	{
		if (!v->getStaticUnitData().canBuild.empty() && v->getStaticUnitData().canBuild == "BigBuilding")
		{
			// конструктор в процессе стройки
			if (v->isUnitBuildingABuilding())
			{
				// постройка завершена — выбираем точку выезда (иначе здание не появится)
				if (v->getBuildTurns() == 0)
				{
					const cPosition esc = findVehicleEscape (model, *v);
					if (esc.x() >= 0)
					{
						cOrder o;
						o.type = eOrderType::FinishBuild;
						o.playerId = playerId;
						o.unitId = v->getId();
						o.position = esc;
						o.level = "op";
						o.reason = "finish building";
						out.push_back (std::move (o));
					}
				}
				continue;
			}
			if (v->isUnitMoving() || v->getMoveJob() != nullptr) continue;

			// выбрать следующее здание по эффективным счётчикам
			// (построено + строится + назначено) — детерминированно, без RNG
			const sID buildWhat = chooseNextBuilding (
				nFactory + ipFactory + asFactory,
				nMine + ipMine + asMine,
				nEnergy + ipEnergy + asEnergy,
				nHabitat + ipHabitat + asHabitat,
				nResearch + ipResearch + asResearch);

			const auto& bdata = unitsData->getStaticUnitData (buildWhat);
			const int buildCost = player->getLastUnitData (buildWhat)->getBuildCost();
			const int cap = v->getStaticUnitData().storageResMax;

			// Пополняемся ДО ПОЛНОЙ вместимости (60) — это (а) гарантирует
			// корректность cVehicle::calcTurboBuild (нужен stored >= buildCost) и
			// (б) включает турбо-режим (каждые +4 металла — минус 1 ход).
			if (v->getStoredResources() < cap)
			{
				const cBuilding* src = findResourceSource (*player, eResourceType::Metal);
				if (src != nullptr && src->getStoredResources() > 0)
				{
					cOrder o;
					o.type = eOrderType::Transfer;
					o.playerId = playerId;
					o.unitId = src->getId();
					o.unitId2 = v->getId();
					o.transferValue = std::min (src->getStoredResources(),
						cap - v->getStoredResources());
					o.resType = eResourceType::Metal;
					o.level = "op";
					o.reason = "refill constructor";
					out.push_back (std::move (o));
				}
				continue;
			}

			// стройка: турбо (1/2) берётся по текущему металлу конструктора
			const int speed = maxFeasibleBuildSpeed (*v, buildCost);
			const cPosition spot = findBuildSpot (model, *player, bdata, player->getLandingPos());
			if (spot.x() < 0) continue;
			cOrder o;
			o.type = eOrderType::Build;
			o.playerId = playerId;
			o.unitId = v->getId();
			o.buildingType = {buildWhat.firstPart, buildWhat.secondPart};
			o.position = spot;
			o.buildSpeed = speed;
			o.level = "op";
			o.reason = "build " + bdata.getDefaultName() + (speed > 0 ? " (turbo)" : "");
			out.push_back (std::move (o));

			// отметить назначение, чтобы следующий свободный конструктор выбрал другое
			if (buildWhat == kIdFactory) ++asFactory;
			else if (buildWhat == kIdMine) ++asMine;
			else if (buildWhat == kIdEnergy) ++asEnergy;
			else if (buildWhat == kIdHabitat) ++asHabitat;
			else if (buildWhat == kIdResearch) ++asResearch;
		}
	}

	// 3) очереди производства на фабриках: сначала конструкторы (параллельная
	//    экономика), затем танки (повтор)
	const int nConstructors = countVehicles (*player, kIdKonstrukt);
	int constructorsQueued = 0;
	for (const auto& b : player->getBuildings())
		if (b->getStaticUnitData().ID == kIdFactory && !b->isBuildListEmpty()
			&& b->getBuildListItem (0).getType() == kIdKonstrukt)
			++constructorsQueued;

	for (const auto& b : player->getBuildings())
	{
		if (b->getStaticUnitData().ID != kIdFactory) continue;
		if (!b->isBuildListEmpty()) continue;
		cOrder o;
		o.type = eOrderType::ChangeBuildList;
		o.playerId = playerId;
		o.unitId = b->getId();
		o.buildSpeed = 1;
		if (nConstructors + constructorsQueued < kTargetConstructors)
		{
			o.buildList = {{kIdKonstrukt.firstPart, kIdKonstrukt.secondPart}};
			o.repeat = false;
			o.reason = "produce constructor";
			++constructorsQueued;
		}
		else
		{
			o.buildList = {{kIdTank.firstPart, kIdTank.secondPart}};
			o.repeat = true;
			o.reason = "produce tanks";
		}
		o.level = "op";
		out.push_back (std::move (o));
	}

	// 4) выезд готовых юнитов из фабрик (иначе они не появляются)
	for (const auto& b : player->getBuildings())
	{
		if (b->getStaticUnitData().ID != kIdFactory) continue;
		if (b->isBuildListEmpty()) continue;
		if (b->getBuildListItem (0).getRemainingMetal() > 0) continue;
		const auto& vehicleData = unitsData->getStaticUnitData (b->getBuildListItem (0).getType());
		const cPosition esc = findFactoryEscape (model, *b, vehicleData, *player);
		if (esc.x() < 0) continue;
		cOrder o;
		o.type = eOrderType::FinishBuild;
		o.playerId = playerId;
		o.unitId = b->getId();
		o.position = esc;
		o.level = "op";
		o.reason = "roll out vehicle";
		out.push_back (std::move (o));
	}
}

//------------------------------------------------------------------------------
void cRuleBrain::microDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;

	const auto& map = model.getMap();
	cMapView view (map, sharedPlayerOf (model, playerId));

	// кэш разведки: список неразведанных клеток + проходимость по типу юнита
	// строится один раз за ход (общий для всех юнитов игрока).
	sScoutCache scoutCache;
	buildScoutCache (*player, *map, scoutCache);

	std::vector<sEnemy> enemies;
	collectVisibleEnemies (*player, model, view, enemies);

	// --- собрать боевые юниты и скоординировать фокус-огонь на уровне игрока ---
	std::vector<const cVehicle*> attackers;
	for (const auto& vehicle : player->getVehicles())
		if (isIdle (*vehicle) && isCombatUnit (*vehicle))
			attackers.push_back (vehicle.get());
	const std::vector<int> focus = coordinateFocusFire (attackers, enemies, view);

	for (const auto& vehicle : player->getVehicles())
	{
		if (!isIdle (*vehicle)) continue;

		const auto& sd = vehicle->getStaticUnitData();

		// --- боевой юнит ---
		if (isCombatUnit (*vehicle))
		{
			// 1) скоординированная цель в радиусе — атакуем (фокус-огонь)
			const auto it = std::find (attackers.begin(), attackers.end(), vehicle.get());
			if (it != attackers.end())
			{
				const int t = focus[static_cast<int> (it - attackers.begin())];
				if (t >= 0)
				{
					cOrder o;
					o.type = eOrderType::Attack;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.position = enemies[t].pos;
					o.level = "micro";
					o.reason = "focus-fire attack";
					out.push_back (std::move (o));
					continue;
				}
			}

			// 2) ранен/под обстрелом — КОРОТКИЙ отход за спины здоровых союзников
			//    (не до базы): юнит выходит из-под огня, но остаётся у фронта,
			//    где его может отремонтировать ремонтник. Стоим, если уже у цели.
			if (shouldRetreat (*vehicle, enemies))
			{
				bool found = false;
				cPosition dest = findRetreatTarget (*vehicle, *player, enemies, view, found);
				const bool toBase = !found;
				if (toBase)
					dest = player->getLandingPos(); // нет строя/направления — старый запасной путь
				cPathCalculator pc (*vehicle, view, dest, false);
				auto path = pc.calcPath();
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					o.level = "micro";
					o.reason = toBase ? "retreat to base" : "retreat behind line";
					out.push_back (std::move (o));
				}
				continue; // уже у цели отхода — стоим, не лезем обратно в бой
			}

			// 3) враги видны, но не в радиусе — сближаемся
			if (const sEnemy* n = nearestEnemy (enemies, vehicle->getPosition()))
			{
				cPathCalculator pc (*vehicle, view, n->pos, true);
				auto path = pc.calcPath();
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					o.level = "micro";
					o.reason = "approach enemy";
					out.push_back (std::move (o));
					continue;
				}
			}

			// 4) врагов не видно — выдвигаемся к ближайшей неразведанной клетке
			//    (разведка боем; на симметричной карте армии встретятся и вступят в бой)
			{
				bool found = false;
				const cPosition target = findNearestUnexplored (*vehicle, view, scoutCache, found);
				if (found)
				{
					cPathCalculator pc (*vehicle, view, target, false);
					auto path = pc.calcPath();
					if (!path.empty())
					{
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.path = pathToVector (path);
						o.level = "micro";
						o.reason = "advance (scout)";
						out.push_back (std::move (o));
					}
				}
			}
			continue;
		}

		// --- ремонтник ---
		if (sd.canRepair)
		{
			// повреждённые свои юниты по убыванию приоритета
			// (ценные боевые юниты/танки важнее, внутри — больше % урона).
			std::vector<cVehicle*> damaged;
			for (const auto& v : player->getVehicles())
			{
				if (v.get() == vehicle.get()) continue;
				if (v->data.getHitpoints() >= v->data.getHitpointsMax()) continue;
				damaged.push_back (v.get());
			}
			std::sort (damaged.begin(), damaged.end(), repairMoreUrgent);

			// чиним самого ценного/повреждённого, к которому можно безопасно подойти;
			// если цель под обстрелом — не лезем (пробуем следующую по приоритету).
			for (cVehicle* d : damaged)
			{
				if (vehicle->canSupply (d, eSupplyType::REPAIR))
				{
					cOrder o;
					o.type = eOrderType::Repair;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.unitId2 = d->getId();
					o.level = "micro";
					o.reason = "repair damaged unit";
					out.push_back (std::move (o));
					break;
				}
				if (repairUnsafe (*vehicle, *d, enemies)) continue; // под обстрелом — пробуем следующего
				cPathCalculator pc (*vehicle, view, *d, true);
				auto path = pc.calcPath();
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					o.level = "micro";
					o.reason = "approach damaged unit";
					out.push_back (std::move (o));
					break;
				}
			}
			continue;
		}

		// --- разведчик ---
		if (sd.vehicleData.canSurvey)
		{
			bool found = false;
			const cPosition target = findNearestUnexplored (*vehicle, view, scoutCache, found);
			if (found)
			{
				cPathCalculator pc (*vehicle, view, target, false);
				auto path = pc.calcPath();
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					o.level = "micro";
					o.reason = "survey unexplored";
					out.push_back (std::move (o));
				}
			}
			continue;
		}

		// конструкторы и прочие — оперативка (этап 4)
	}
}

} // namespace ai
