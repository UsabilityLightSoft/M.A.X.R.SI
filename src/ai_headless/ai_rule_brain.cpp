/***************************************************************************
 * ai_rule_brain.cpp — алгоритмический мозг: микро (этап 3), оперативка и
 * стратегия (этап 4), экономика без даровой подпитки движка (шаг 5 PLAN.md),
 * память о враге, карта угроз и тревога у базы (шаг 7 PLAN.md), операции
 * армии (шаг 8 PLAN.md, ai_army_ops), роли армии (шаг 9, ai_roles),
 * исследования и золото (шаг 10, ai_tech): центры по доходу, тема по пользе,
 * переработка золота, покупка улучшений, обновление машин в депо и зданий.
 ***************************************************************************/

#include "ai_rule_brain.h"

#include "ai_types.h"
#include "ai_upgrades.h"

#include "game/data/base/base.h"
#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/id.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/attackjob.h"
#include "game/logic/pathcalculator.h"
#include "utility/log.h"
#include "utility/mathtools.h"

#include "game/logic/turncounter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <tuple>
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
// шаг 9: кассетный удар задевает клетки ближе 2 (по сторонам) к цели — и свои машины и здания
bool ownWithinSplash (const cMap& map, const cPlayer* owner, const cPosition& p)
{
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

std::vector<int> coordinateFocusFire (const std::vector<const cVehicle*>& attackers,
	const std::vector<sEnemy>& enemies, const cMapView& view, const cMap* map = nullptr)
{
	const int A = static_cast<int> (attackers.size());
	const int E = static_cast<int> (enemies.size());
	std::vector<int> target (A, -1);
	if (A == 0 || E == 0) return target;

	// матрица достижимости «атакующий → цель» (цель в радиусе атаки); шаг 9 — кассетная
	// машина не бьёт по цели, у которой рядом свои
	std::vector<std::vector<bool>> canHit (A, std::vector<bool> (E, false));
	for (int a = 0; a < A; ++a)
	{
		const bool splash = map != nullptr && attackers[a]->getStaticUnitData().muzzleType == eMuzzleType::RocketCluster;
		for (int e = 0; e < E; ++e)
			canHit[a][e] = attackers[a]->canAttackObjectAt (enemies[e].pos, view, false)
				&& !(splash && ownWithinSplash (*map, attackers[a]->getOwner(), enemies[e].pos));
	}

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

// шаг 9: ремонтник и снабженец ищут машины не дальше (Чебышёв); раненый идёт к ремонтнику не дальше
constexpr int kRepairReach = 12;
constexpr int kRetreatToRepairer = 6;
// малая фабрика: когда добыча металла главной базы не меньше; таких фабрик не больше
constexpr int kSmallFactoryIncome = 10;
constexpr int kMaxSmallFactories = 1;

// путь до хода: столько клеток, сколько машина пройдёт за ход (шаг 9: сопровождение и
// ремонтник не уходят по длинному пути на несколько ходов — каждый ход задание заново)
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
// Узкое место: поиск неразведанной клетки делал ПОЛНЫЙ скан карты (O(клетки)) на
// КАЖДЫЙ боевой юнит и разведчика КАЖДЫЙ ход — O(юниты×клетки). Здесь список
// неразведанных клеток строится ОДИН раз за ход (общий для всех юнитов игрока),
// а проходимость кэшируется по типу стат. данных юнита (cMap::possiblePlace
// зависит только от стат. данных + владельца + состояния карты — у всех юнитов
// игрока владелец один, поэтому по указателю стат. данных кэш корректен).
// Выбор ближайшей клетки (pathToUnexplored) БАЙТ-В-БАЙТ совпадает со старым полным
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

std::vector<char>& scoutPassable (sScoutCache& cache, const cVehicle& unit, const cMapView& view)
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

constexpr int kSurveyorHome = 20;   // разведчик сначала разведывает этот радиус вокруг высадки

// Путь к ближайшей неразведанной проходимой клетке (для surveyor и боевого «скаута»);
// home — только клетки не дальше radius (Чебышёв) от home. Ближайшая может быть
// недостижима (вода, отрезанный берег): такая помечается в кэше хода и берётся
// следующая — до tries попыток (раньше юнит в этом случае стоял на месте).
// Шаг 7: только клетки без угрозы и путь в обход опасных клеток; если обойти
// нельзя — клетка отбрасывается (только для этого юнита) и берётся следующая.
std::vector<cPosition> pathToUnexplored (const cVehicle& v, const cMapView& view,
	sScoutCache& cache, const cWorldModel& world, eThreatLayer layer, const cPosition* home = nullptr, int radius = 0, int tries = 3)
{
	auto& pass = scoutPassable (cache, v, view);
	const cPosition from = v.getPosition();
	std::vector<int> unsafe; // клетки без безопасного пути — для этого юнита
	for (int t = 0; t < tries; ++t)
	{
		int bestI = -1;
		int bestDist = std::numeric_limits<int>::max();
		for (size_t i = 0; i < cache.unexplored.size(); ++i)
		{
			if (!pass[i]) continue;
			const cPosition& p = cache.unexplored[i];
			if (home && std::max (std::abs (p.x() - home->x()), std::abs (p.y() - home->y())) > radius) continue;
			const int d = (p - from).l2NormSquared();
			if (d < bestDist)
			{
				if (world.threatFor (v, p, layer) > 0) continue;
				if (std::find (unsafe.begin(), unsafe.end(), static_cast<int> (i)) != unsafe.end()) continue;
				bestDist = d;
				bestI = static_cast<int> (i);
			}
		}
		if (bestI < 0) return {};
		cPathCalculator pc (v, view, cache.unexplored[bestI], false);
		auto path = pathToVector (pc.calcPath());
		if (path.empty())
		{
			pass[bestI] = 0;
			continue;
		}
		if (!world.pathUnsafe (v, path, layer)) return path;
		auto safe = world.safePath (v, view, cache.unexplored[bestI], layer);
		if (!safe.empty() && !world.pathUnsafe (v, safe, layer)) return safe;
		unsafe.push_back (bestI);
	}
	return {};
}

// путь к dest: как у движка, а если он идёт через клетки под угрозой — в обход
// (A* со штрафом за опасные клетки), если обход есть. safe — путь без угрозы.
std::vector<cPosition> threatAwarePath (const cVehicle& v, const cMapView& view, const cPosition& dest,
	const cWorldModel& world, eThreatLayer layer = eThreatLayer::Reach)
{
	cPathCalculator pc (v, view, dest, false);
	auto path = pathToVector (pc.calcPath());
	if (!path.empty() && world.pathUnsafe (v, path, layer))
	{
		auto alt = world.safePath (v, view, dest, layer);
		if (!alt.empty()) path = std::move (alt);
	}
	return path;
}

// угроза клетки без привязки к машине (наземная)
int cellThreat (const cWorldModel& world, const cPosition& c, eThreatLayer layer)
{
	if (layer == eThreatLayer::None) return 0;
	return layer == eThreatLayer::Fire ? world.groundFire (c) : world.groundThreat (c);
}

// боевая наземная машина для операций армии
bool isGroundFighter (const cVehicle& v)
{
	const auto& sd = v.getStaticUnitData();
	return isCombatUnit (v) && sd.factorAir == 0 && sd.factorGround > 0 && !v.isDisabled() && !v.isUnitLoaded();
}

// --- оперативка/стратегия (этап 4; экономика без даров — шаг 8 этапа 5) -----

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
	const cStaticUnitData& vehicleData, const cPlayer& player,
	const std::vector<std::pair<cPosition, bool>>* avoid = nullptr)
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
				// не на площадку запланированной стройки
				if (avoid && std::any_of (avoid->begin(), avoid->end(), [&] (const auto& f) {
						const int fs = f.second ? 2 : 1;
						return p.x() >= f.first.x() && p.y() >= f.first.y() && p.x() < f.first.x() + fs && p.y() < f.first.y() + fs;
					})) continue;
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

// --- экономика: константы типов и чистые хелперы ---------------------------

const sID kIdFactory (1, 11);
const sID kIdMine (1, 22);
const sID kIdEnergy (1, 7);       // energy_big: 6 энергии за 6 нефти
const sID kIdEnergySmall (1, 8);  // energy_small: 1 энергия за 2 нефти (строит пионер)
const sID kIdConnector (1, 4);
const sID kIdStorageMetal (1, 32);
const sID kIdHabitat (1, 19);
const sID kIdResearch (1, 27);
const sID kIdTank (0, 32);
const sID kIdKonstrukt (0, 19);
// шаг 10: золото и обновление машин
const sID kIdGoldRefinery (1, 14); // переработка: 5 золота → 5 кредитов в ход, энергия 1
const sID kIdGoldVault (1, 31);    // хранилище золота (50): переработке нужен запас, когда добыча < 5
const sID kIdDepot (1, 5);         // депо: обновление, ремонт и боезапас наземных машин внутри
// шаг 13: оборона базы (ai_defense)
const sID kIdTraining (1, 34);     // учебный центр: пехота (видит коммандос) и коммандос
// шаг 13, часть 2: флот (П11–П14) и ракетная оборона берега (П25)
const sID kIdShipyard (1, 12);     // верфь: 2×2 на воде, строит корабли
const sID kIdDock (1, 6);          // док: 2×2 на воде, ремонт и боезапас кораблей внутри
const sID kIdGunMissile (1, 17);   // ракетная пушка: дальность 11 (улучшение дальности за золото — П25)
const sID kIdPlatform (1, 25);     // платформа: на воде и берегу — «суша» для стройки (П14, острова)
// шаг 13, часть 3: авиация (П15–П17, П10)
const sID kIdAirFactory (1, 9);    // авиазавод: строит самолёты
const sID kIdHangar (1, 20);       // ангар: 12 самолётов, ремонт и боезапас за металл подбазы

// --- шаг 10: исследования, золото, обновление (подобраны опытом «вариант против варианта»,
// отчёт шага 10 §4) ---
constexpr int kResearchPerIncome = 10;   // центр исследований — на 10 металла добычи в ход
constexpr int kResearchMax = 6;
constexpr int kResearchMinFactories = 2; // центры — с двух фабрик (построено и в планах)
constexpr int kResearchMinIncome = 16;   // и с добычи металла 16 в ход (с 12 — в ущерб армии, Donuts, §4)
constexpr int kGoldMinFactories = 1;     // переработка — сразу после первой фабрики, раньше новых шахт
constexpr int kGoldMin = 3;              // доступного золота шахт — от 3 в ход
constexpr int kGoldMineWeight = 150;     // вес единицы золота в выборе площадки шахты (металл — 100)
constexpr int kRefitGap = 2;             // обновлять машину, если отстала хотя бы на столько (refitGap)
constexpr int kDepotMin = 6;             // депо — когда отставших бойцов столько
constexpr int kRefitReach = 20;          // в депо — машины не дальше (Чебышёв)

// насколько машина отстала от текущей версии своего типа (урон + броня + 3·дальность +
// 4·выстрелы + hp/4 + ход): обновлять ради ничтожного (боезапас, обзор) не стоит
int refitGap (const cVehicle& v)
{
	const cPlayer* owner = v.getOwner();
	const cDynamicUnitData* last = owner ? owner->getLastUnitData (v.data.getId()) : nullptr;
	if (last == nullptr || !v.data.canBeUpgradedTo (*last)) return 0;
	return (last->getDamage() - v.data.getDamage()) + (last->getArmor() - v.data.getArmor())
		+ 3 * (last->getRange() - v.data.getRange()) + 4 * (last->getShotsMax() - v.data.getShotsMax())
		+ (last->getHitpointsMax() - v.data.getHitpointsMax()) / 4 + (last->getSpeedMax() - v.data.getSpeedMax()) / 4;
}

// золото, которое шахты главной подбазы дадут без среза металла (у шахты общий предел добычи)
int goldAvailable (const cSubBase* sb)
{
	if (sb == nullptr) return 0;
	int g = 0;
	for (const cBuilding* b : sb->getBuildings())
	{
		const int cap = b->getStaticData().canMineMaxRes;
		if (cap <= 0) continue;
		const auto& mp = b->getMaxProd();
		g += std::clamp (cap - mp.metal, 0, mp.gold);
	}
	return g;
}

constexpr int kMinMineMetal = 3;   // шахта ради металла — от 3 металла в ход
constexpr int kMinMineOil = 4;     // шахта ради нефти (когда энергию не из чего делать)
constexpr int kMaxChain = 10;      // соединителей от базы до шахты — не больше
constexpr int kMaxChainWater = 24; // шаг 13, часть 2: до верфи и дока на воде (море бывает дальше от базы)
constexpr int kMaxSiteRadius = 18; // площадки шахт — не дальше от высадки (Чебышёв)
constexpr int kBaseRadius = 14;    // здания базы — не дальше от высадки
constexpr int kMaxFactories = 6;
constexpr int kFactoryRingMin = 4; // свободных клеток вокруг фабрики — для выезда машин
constexpr int kMaxStorages = 3;
constexpr int kStuckTurns = 3;     // ходов без пути к площадке — план снимается

// шаг 7: тревога у базы и угрозы
const sID kIdFighter (0, 16);      // эталон воздушной машины для карты угроз по воздуху
constexpr int kEvacuateRadius = 15; // стройщик под угрозой ищет безопасную клетку не дальше (Чебышёв)
constexpr int kAlarmRadius = 6;    // враг ближе (евклидово) к ключевому зданию или стройке — тревога
constexpr int kAlarmFresh = 2;     // машина из памяти, виденная не раньше стольких ходов назад, — ещё угроза
constexpr int kAlarmGroup = 6;     // враги ближе друг к другу — одна группа (один ответ)
constexpr int kAlarmMaxDist = 30;  // защитники — не дальше от группы врагов
constexpr int kAlarmMarginNum = 3; // ожидаемый урон защитников ≥ 3/2 hp группы
constexpr int kAlarmMarginDen = 2;
constexpr int kSafeSnap = 8;       // отход: безопасная клетка не дальше от точки отхода
// слои угроз (подобраны турниром против base, отчёт шага 7 §4): стройщики, геолог и
// разведка танками обходят «дальность + ход» врага, отход раненых — его зону огня с места
constexpr eThreatLayer kBuilderLayer = eThreatLayer::Reach;
constexpr eThreatLayer kScoutLayer = eThreatLayer::Reach;
constexpr eThreatLayer kRetreatLayer = eThreatLayer::Fire;

using tFootprints = std::vector<std::pair<cPosition, bool>>; // левый верхний угол + big

// число конструкторов, прямо сейчас строящих здание данного типа (ещё не завершили)
int countConstructorsBuilding (const cPlayer& player, const sID& type)
{
	int n = 0;
	for (const auto& v : player.getVehicles())
		if (v->isUnitBuildingABuilding() && v->getBuildingType() == type) ++n;
	return n;
}

// Сколько нефти сожгут станции и генераторы, чтобы дать need энергии, — тот же
// расчёт, что cSubBase::checkOil (сначала станции по 6, остаток — генераторами).
int oilForEnergy (int need, int stations, int generators)
{
	int s = std::min ((need + 3) / 6, stations);
	int g = std::max (need - s * 6, 0);
	if (g > generators)
	{
		if (s < stations)
		{
			++s;
			g = 0;
		}
		else
			g = generators;
	}
	return s * 6 + g * 2;
}

// хватит ли станций/генераторов и нефти на need энергии
bool energyFeasible (int need, int stations, int generators, int oil)
{
	if (need <= 0) return true;
	if (need > stations * 6 + generators) return false;
	return oilForEnergy (need, stations, generators) <= oil;
}

// Металл стройщика на скорость 1 — как cVehicle::calcTurboBuild при металле до
// вместимости cap (каждые +4 металла — минус 1 ход); 0 — турбо недоступно.
int builderTurboCost (int cost, int needsMetal, int cap)
{
	const int nm = needsMetal == 0 ? 1 : needsMetal;
	const int turns0 = (cost + nm - 1) / nm;
	int a = cost, rounds = turns0, costs = cost;
	while (a >= 4 && cap >= costs + 4)
	{
		--rounds;
		costs += 4;
		a -= 4;
	}
	return (rounds < turns0 && rounds > 0) ? costs : 0;
}

// металл в ход фабрики на скорости speed (как cActionChangeBuildList)
int factoryMetalPerRound (const cBuilding& f, int speed)
{
	static constexpr int k[3] = {1, 4, 12};
	return k[std::clamp (speed, 0, 2)] * f.getStaticUnitData().needsMetal;
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

// Главная подбаза: где больше всего зданий (при равенстве — первая по id зданий).
const cSubBase* findMainSubBase (const cPlayer& player)
{
	std::vector<std::pair<const cSubBase*, int>> counts; // в порядке первого здания — детерминированно
	for (const auto& b : player.getBuildings())
	{
		if (b->subBase == nullptr) continue;
		auto it = std::find_if (counts.begin(), counts.end(), [&] (const auto& c) { return c.first == b->subBase; });
		if (it == counts.end())
			counts.emplace_back (b->subBase, 1);
		else
			++it->second;
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

// Экономика главной подбазы на начало хода.
struct sEcon
{
	const cSubBase* sb = nullptr;
	int metalProd = 0, metalMaxProd = 0, metalStored = 0, metalMaxStored = 0, metalNeeded = 0;
	int oil = 0;           // нефть: максимум добычи + склад
	int oilFree = 0;       // нефть без среза добычи металла (у шахты общий предел 16 в ход)
	int energyDemand = 0;  // потребность всех потребителей подбазы (работают или нет)
	int energyNeedWorking = 0;
	int stations = 0, generators = 0;
	int humanFree = 0;
	int free = 0;          // металл хода: stored + prod - needed (к концу хода не уйти в минус)
};

sEcon readEcon (const cPlayer& player)
{
	sEcon e;
	e.sb = findMainSubBase (player);
	if (e.sb == nullptr) return e;
	const cSubBase& sb = *e.sb;
	e.metalProd = sb.getProd().metal;
	e.metalMaxProd = sb.getMaxProd().metal;
	e.metalStored = sb.getResourcesStored().metal;
	e.metalMaxStored = sb.getMaxResourcesStored().metal;
	e.metalNeeded = sb.getResourcesNeeded().metal;
	e.oil = sb.getResourcesStored().oil;
	e.energyNeedWorking = sb.getEnergyNeed();
	e.humanFree = sb.getHumanProd() - sb.getHumanNeed();
	e.oilFree = sb.getResourcesStored().oil;
	for (const cBuilding* b : sb.getBuildings())
	{
		const auto& sd = b->getStaticUnitData();
		e.energyDemand += sd.needsEnergy;
		if (sd.ID == kIdEnergy) ++e.stations;
		else if (sd.ID == kIdEnergySmall) ++e.generators;
		if (sd.buildingData.canMineMaxRes > 0)
		{
			// по ресурсу под шахтой, а не по подбазе: та считает только работающие шахты
			const auto& mp = b->getMaxProd();
			e.oil += mp.oil;
			// золото у движка важнее металла при нехватке нефти — его доля тоже не свободна
			e.oilFree += std::clamp (sd.buildingData.canMineMaxRes - mp.metal - mp.gold, 0, mp.oil);
		}
	}
	e.free = std::max (0, e.metalStored + e.metalProd - e.metalNeeded);
	// станции сверх «свободной» нефти заберут мощность шахт у металла — не тратить его заранее
	const int cut = std::max (0, oilForEnergy (e.energyDemand, e.stations, e.generators) - e.oilFree);
	e.free = std::max (0, e.free - std::min (cut, e.metalProd));
	return e;
}

// разведанный ресурс клетки (0 — нет или ещё не разведан)
int exploredResource (const cMap& map, const cPlayer& player, const cPosition& p, eResourceType type)
{
	if (!map.isValidPosition (p) || !player.hasResourceExplored (p)) return 0;
	const auto& r = map.getResource (p);
	return r.typ == type ? r.value : 0;
}

// клетки под зданием (1 или 2×2)
std::vector<cPosition> footprintCells (const cPosition& q, bool big)
{
	if (!big) return {q};
	return {q, q + cPosition (1, 0), q + cPosition (0, 1), q + cPosition (1, 1)};
}

// соседи по стороне — через них здание соединяется в подбазу (как cBase::addBuilding)
std::vector<cPosition> sideNeighbours (const cPosition& q, bool big)
{
	if (big)
		return {q.relative (0, -1), q.relative (1, -1), q.relative (2, 0), q.relative (2, 1),
			q.relative (0, 2), q.relative (1, 2), q.relative (-1, 0), q.relative (-1, 1)};
	return {q.relative (0, -1), q.relative (1, 0), q.relative (0, 1), q.relative (-1, 0)};
}

// кольцо клеток вокруг здания (с углами) — куда выезжают машины
std::vector<cPosition> ringCells (const cPosition& q, bool big)
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

bool overlapsAny (const cPosition& q, bool big, const tFootprints& reserved)
{
	const int qs = big ? 2 : 1;
	for (const auto& r : reserved)
	{
		const int rs = r.second ? 2 : 1;
		if (q.x() < r.first.x() + rs && r.first.x() < q.x() + qs
			&& q.y() < r.first.y() + rs && r.first.y() < q.y() + qs) return true;
	}
	return false;
}

// на клетке стоит здание главной подбазы
bool hasMainBuilding (const cMap& map, const cPlayer& player, const cSubBase* main, const cPosition& p)
{
	if (main == nullptr || !map.isValidPosition (p)) return false;
	for (const cBuilding* b : map.getField (p).getBuildings())
		if (b->getOwner() == &player && b->subBase == main) return true;
	return false;
}

// клетка проходима для наземной машины: суша, нет «наземного» здания (соединитель/дорога — можно)
bool groundFree (const cMap& map, const cPosition& p)
{
	if (!map.isValidPosition (p) || map.isBlocked (p) || map.isWater (p)) return false;
	for (const cBuilding* b : map.getField (p).getBuildings())
	{
		if (b->isRubble()) continue; // обломки проходимы
		const auto sp = b->getStaticUnitData().surfacePosition;
		if (sp != eSurfacePosition::Above && sp != eSurfacePosition::Base && sp != eSurfacePosition::AboveBase) return false;
	}
	return true;
}

// можно ли поставить здание, не считая машин на клетках (они уедут); как проверка сервера
bool canPlaceIgnoringVehicles (const cMap& map, const cPlayer& player, const cStaticUnitData& data, const cPosition& q)
{
	for (const auto& c : footprintCells (q, data.buildingData.isBig))
	{
		if (!map.isValidPosition (c)) return false;
		if (!map.possiblePlaceBuilding (data, c, &player, map.getField (c).getVehicle())) return false;
	}
	return true;
}

// Стройщик заперт своими зданиями (высадка в угол карты): ни одной соседней клетки
// без здания и ни одной площадки большого здания с его клеткой — выйти нельзя.
// Тогда соседнее малое здание (генератор) мешает: его можно самоуничтожить (как в
// GUI), пионер поставит новое. nullptr — не заперт.
const cBuilding* trappingBuilding (const cMap& map, const cPlayer& player, const cStaticUnitData& bigData, const cVehicle& v)
{
	const cPosition pos = v.getPosition();
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
			if ((dx != 0 || dy != 0) && groundFree (map, pos + cPosition (dx, dy))) return nullptr;
	const cPosition offs[4] = {{0, 0}, {-1, 0}, {0, -1}, {-1, -1}};
	for (const auto& off : offs)
		if (canPlaceIgnoringVehicles (map, player, bigData, pos + off)) return nullptr;
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx)
		{
			const cPosition c = pos + cPosition (dx, dy);
			if (!map.isValidPosition (c)) continue;
			for (const cBuilding* b : map.getField (c).getBuildings())
				if (b->getOwner() == &player && !b->getIsBig() && b->getStaticData().canSelfDestroy && !b->isBeingAttacked())
					return b;
		}
	return nullptr;
}

// Сетка соединителей: 0 — клетка здания главной подбазы, k — сюда дойдёт k-й
// соединитель от базы (клетки без зданий, где соединитель ставится), -1 — нет.
struct sConnGrid
{
	int w = 0, h = 0;
	std::vector<int> dist;
	int at (const cPosition& p) const
	{
		if (p.x() < 0 || p.y() < 0 || p.x() >= w || p.y() >= h) return -1;
		return dist[static_cast<size_t> (p.y()) * w + p.x()];
	}
	void set (const cPosition& p, int d) { dist[static_cast<size_t> (p.y()) * w + p.x()] = d; }
};

void buildConnGrid (const cModel& model, const cPlayer& player, const cSubBase* main, sConnGrid& g, int maxChain = kMaxChain)
{
	const cMap& map = *model.getMap();
	g.w = map.getSize().x();
	g.h = map.getSize().y();
	g.dist.assign (static_cast<size_t> (g.w) * g.h, -1);
	if (main == nullptr) return;
	const auto& connData = model.getUnitsData()->getStaticUnitData (kIdConnector);
	std::vector<cPosition> queue;
	for (const auto& b : player.getBuildings())
	{
		if (b->subBase != main) continue;
		for (const auto& c : footprintCells (b->getPosition(), b->getIsBig()))
			if (g.at (c) == -1)
			{
				g.set (c, 0);
				queue.push_back (c);
			}
	}
	const cPosition dirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
	for (size_t i = 0; i < queue.size(); ++i)
	{
		const cPosition p = queue[i];
		const int d = g.at (p);
		if (d >= maxChain) continue;
		for (const auto& dir : dirs)
		{
			const cPosition n = p + dir;
			if (!map.isValidPosition (n) || g.at (n) != -1) continue;
			if (!map.getField (n).getBuildings().empty()) continue;
			// машина на клетке мешает соединителю (BuildPath прервётся); свой пионер — нет, он и строит
			const cVehicle* onCell = map.getField (n).getVehicle();
			const bool ownPionier = onCell != nullptr && onCell->getOwner() == &player
				&& onCell->getStaticUnitData().canBuild == "SmallBuilding";
			if (!map.possiblePlaceBuilding (connData, n, &player, ownPionier ? onCell : nullptr)) continue;
			g.set (n, d + 1);
			queue.push_back (n);
		}
	}
}

// сколько соединителей нужно до здания в q (0 — уже касается базы, -1 — не дотянуться)
int chainTo (const sConnGrid& g, const cPosition& q, bool big, cPosition* side = nullptr)
{
	int best = -1;
	for (const auto& s : sideNeighbours (q, big))
	{
		const int d = g.at (s);
		if (d < 0) continue;
		if (best < 0 || d < best)
		{
			best = d;
			if (side) *side = s;
		}
	}
	return best;
}

// путь соединителей от базы до клетки s: [клетка у базы, …, s]
std::vector<cPosition> connectorPath (const sConnGrid& g, cPosition s)
{
	int d = g.at (s);
	if (d < 1) return {};
	std::vector<cPosition> rev {s};
	const cPosition dirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
	while (d > 1)
	{
		bool stepped = false;
		for (const auto& dir : dirs)
		{
			const cPosition n = s + dir;
			if (g.at (n) == d - 1)
			{
				s = n;
				--d;
				rev.push_back (s);
				stepped = true;
				break;
			}
		}
		if (!stepped) return {};
	}
	std::reverse (rev.begin(), rev.end());
	return rev;
}

// клетки здания под наземной угрозой врага (шаг 7)
bool footprintThreatened (const cWorldModel& world, const cPosition& q, bool big, eThreatLayer layer)
{
	for (const auto& c : footprintCells (q, big))
		if (cellThreat (world, c, layer) > 0) return true;
	return false;
}

// площадка шахты на разведанном ресурсе
struct sSite
{
	cPosition pos {-1, -1};
	int metal = 0;
	int oil = 0;
	int chain = -1;
	long long score = std::numeric_limits<long long>::min();
	bool valid() const { return pos.x() >= 0; }
};

// Лучшая площадка шахты: металл под шахтой (и нефть, если энергию не из чего
// делать), ближе к базе и короче цепочка соединителей. Только разведанные клетки.
sSite findMineSite (const cModel& model, const cPlayer& player, const sConnGrid& g,
	const tFootprints& reserved, bool wantOil, const cWorldModel& world, eThreatLayer layer, int goldW = 0)
{
	const cMap& map = *model.getMap();
	const auto& mineData = model.getUnitsData()->getStaticUnitData (kIdMine);
	const int cap = mineData.buildingData.canMineMaxRes;
	const cPosition landing = player.getLandingPos();
	const cPosition offs[4] = {{0, 0}, {-1, 0}, {0, -1}, {-1, -1}};
	sSite best;
	for (int y = landing.y() - kMaxSiteRadius; y <= landing.y() + kMaxSiteRadius; ++y)
		for (int x = landing.x() - kMaxSiteRadius; x <= landing.x() + kMaxSiteRadius; ++x)
		{
			const cPosition c (x, y);
			if (!map.isValidPosition (c) || !player.hasResourceExplored (c)) continue;
			const auto& r = map.getResource (c);
			const bool metalSpot = r.typ == eResourceType::Metal && r.value >= kMinMineMetal;
			const bool oilSpot = wantOil && r.typ == eResourceType::Oil && r.value >= kMinMineOil;
			const bool goldSpot = goldW > 0 && r.typ == eResourceType::Gold && r.value >= 2; // шаг 10: золото — на переработку
			if (!metalSpot && !oilSpot && !goldSpot) continue;
			for (const auto& off : offs)
			{
				const cPosition q = c + off;
				if (overlapsAny (q, true, reserved)) continue;
				if (!canPlaceIgnoringVehicles (map, player, mineData, q)) continue;
				if (footprintThreatened (world, q, true, layer)) continue; // под угрозой врага — не строить
				const int chain = chainTo (g, q, true);
				if (chain < 0) continue;
				int m = 0, o = 0, gd = 0;
				for (const auto& f : footprintCells (q, true))
				{
					m += exploredResource (map, player, f, eResourceType::Metal);
					o += exploredResource (map, player, f, eResourceType::Oil);
					if (goldW > 0) gd += exploredResource (map, player, f, eResourceType::Gold);
				}
				const int metal = std::min (m, cap);
				// шаг 10: шахта добывает металл, затем золото, затем нефть (cBuilding::initMineResourceProd)
				const int gold = std::min (gd, cap - metal);
				const int oil = std::min (o, cap - metal - gold);
				if (metal < kMinMineMetal && !(wantOil && oil >= kMinMineOil) && !(goldW > 0 && gold >= kMinMineMetal)) continue;
				const int far = std::max (std::abs (q.x() - landing.x()), std::abs (q.y() - landing.y()));
				const long long score = metal * 100LL + oil * (wantOil ? 100LL : 25LL) + gold * static_cast<long long> (goldW)
					- chain * 40LL - far * 3LL;
				if (score > best.score)
				{
					best.pos = q;
					best.metal = metal;
					best.oil = oil;
					best.chain = chain;
					best.score = score;
				}
			}
		}
	return best;
}

// Сколько клеток (до limit) доступно наземной машине из start, если клетки blocked
// заняты: обход по сторонам через свободные клетки (соединители и дороги — можно).
int freeRegion (const cMap& map, const cPosition& start, const std::vector<cPosition>& blocked, int limit)
{
	auto isBlocked = [&] (const cPosition& c) { return std::find (blocked.begin(), blocked.end(), c) != blocked.end(); };
	if (isBlocked (start)) return 0;
	std::vector<cPosition> seen {start};
	const cPosition dirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
	for (size_t i = 0; i < seen.size() && static_cast<int> (seen.size()) < limit; ++i)
		for (const auto& d : dirs)
		{
			const cPosition n = seen[i] + d;
			if (!groundFree (map, n) || isBlocked (n) || std::find (seen.begin(), seen.end(), n) != seen.end()) continue;
			seen.push_back (n);
		}
	return std::min (static_cast<int> (seen.size()), limit);
}

constexpr int kMinRegion = 8; // здание не должно замуровать машину в кармане меньше этого

// Место для здания у главной базы: касается её стороной (иначе нет металла и
// энергии), не занимает разведанных металла и нефти (они под шахты), оставляет
// выезд фабрикам. Кольцами от высадки — детерминированно.
cPosition findBaseSpot (const cModel& model, const cPlayer& player, const cSubBase* main,
	const cStaticUnitData& data, const tFootprints& reserved, const cWorldModel& world, eThreatLayer layer)
{
	if (main == nullptr) return cPosition (-1, -1);
	const cMap& map = *model.getMap();
	const bool big = data.buildingData.isBig;
	const bool isFactory = !data.canBuild.empty();
	const cPosition landing = player.getLandingPos();
	std::vector<const cBuilding*> factories;
	for (const auto& b : player.getBuildings())
		if (b->subBase == main && !b->getStaticUnitData().canBuild.empty()) factories.push_back (b.get());

	// второй проход (тесная база, угол карты) — мягче: ресурсы берегутся только
	// шахтные, выезда достаточно одного, радиус больше
	for (int pass = 0; pass < 2; ++pass)
	{
	const bool strict = pass == 0;
	const int minMetal = strict ? 2 : kMinMineMetal;
	const int minOil = strict ? 3 : kMinMineOil + 3;
	const int ringSelf = isFactory ? (strict ? kFactoryRingMin + 1 : 3) : (strict ? (big ? 2 : 3) : 1);
	const int ringFactory = strict ? kFactoryRingMin : 2;
	for (int r = 1; r <= kBaseRadius + (strict ? 0 : 4); ++r)
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
				const cPosition q = landing + cPosition (dx, dy);
				if (overlapsAny (q, big, reserved)) continue;
				if (footprintThreatened (world, q, big, layer)) continue; // под угрозой врага — не строить
				const auto cells = footprintCells (q, big);
				bool ok = true;
				for (const auto& c : cells)
				{
					if (!map.isValidPosition (c)
						|| !map.possiblePlaceBuilding (data, c, &player, map.getField (c).getVehicle())
						|| exploredResource (map, player, c, eResourceType::Metal) >= minMetal
						|| exploredResource (map, player, c, eResourceType::Oil) >= minOil)
					{
						ok = false;
						break;
					}
				}
				if (!ok) continue;
				bool touches = false;
				for (const auto& s : sideNeighbours (q, big))
					if (hasMainBuilding (map, player, main, s))
					{
						touches = true;
						break;
					}
				if (!touches) continue;
				// не замуровать свои машины рядом (угол карты: конструктор в кармане у шахты)
				for (const auto& vp : player.getVehicles())
				{
					const cPosition vpos = vp->getPosition();
					if (vp->isUnitBuildingABuilding() || inFootprint (vpos, q, big)) continue;
					if (std::max (std::abs (vpos.x() - q.x()), std::abs (vpos.y() - q.y())) > 3) continue;
					if (freeRegion (map, vpos, {}, kMinRegion) >= kMinRegion && freeRegion (map, vpos, cells, kMinRegion) < kMinRegion)
					{
						ok = false;
						break;
					}
				}
				if (!ok) continue;
				// стройщик выедет из готового здания на клетку, откуда есть куда ехать
				bool exitOk = false;
				for (const auto& c : ringCells (q, big))
					if (groundFree (map, c) && freeRegion (map, c, cells, kMinRegion) >= kMinRegion)
					{
						exitOk = true;
						break;
					}
				if (!exitOk) continue;
				// свободные клетки вокруг здания fq с учётом нового здания
				auto freeRing = [&] (const cPosition& fq, bool fbig) {
					int n = 0;
					for (const auto& c : ringCells (fq, fbig))
						// шаг 9: берег — не выезд (танк на берег не встаёт, factorCoast 0): Lava 47 —
						// фабрика у берега, заставленная малой фабрикой и стройкой, стояла с готовым танком
						if (groundFree (map, c) && !map.isCoast (c)
							&& std::find (cells.begin(), cells.end(), c) == cells.end()) ++n;
					return n;
				};
				// стройщику и машинам нужен выезд: фабрике — с запасом, прочим — хотя бы два
				if (freeRing (q, big) < ringSelf) continue;
				for (const cBuilding* f : factories)
					if (freeRing (f->getPosition(), f->getIsBig()) < ringFactory)
					{
						ok = false;
						break;
					}
				if (!ok) continue;
				return q;
			}
	}
	return cPosition (-1, -1);
}

// шаг 13, часть 2 (П11, П14): место верфи или дока — 2×2 на воде водоёма sea, связь с главной подбазой
// соединителями (не длиннее kMaxChain — сетка g), выход кораблям (≥ 4 свободных клеток воды вокруг), не под
// угрозой; ближе к anchor и короче цепочка. (−1,−1) — нет
cPosition findWaterSite (const cModel& model, const cPlayer& player, const sConnGrid& g, const tFootprints& reserved,
	const cStaticUnitData& data, const cBaseDefense& defense, int sea, const cPosition& anchor, const cWorldModel& world, eThreatLayer layer)
{
	const cMap& map = *model.getMap();
	cPosition best (-1, -1);
	long long bestKey = std::numeric_limits<long long>::max();
	if (sea < 0 || anchor.x() < 0) return best;
	constexpr int R = 28; // море бывает в 20 клетках от высадки (П3: seaDistance до 20), 2×2 воды — дальше
	for (int dy = -R; dy <= R; ++dy)
		for (int dx = -R; dx <= R; ++dx)
		{
			const cPosition q = anchor + cPosition (dx, dy);
			if (overlapsAny (q, true, reserved)) continue;
			if (!canPlaceIgnoringVehicles (map, player, data, q)) continue;
			bool inSea = true;
			for (const auto& c : footprintCells (q, true))
				if (defense.seaLabelOf (c) != sea) inSea = false;
			if (!inSea || footprintThreatened (world, q, true, layer)) continue;
			const int chain = chainTo (g, q, true);
			if (chain < 0) continue;
			int water = 0;
			for (const auto& c : ringCells (q, true))
				if (map.isValidPosition (c) && map.isWater (c) && map.getField (c).getBuildings().empty()) ++water;
			if (water < 4) continue;
			const long long key = chain * 40LL + std::max (std::abs (dx), std::abs (dy)) * 3LL;
			if (key < bestKey)
			{
				bestKey = key;
				best = q;
			}
		}
	return best;
}

// шаг 13, часть 2 (П14, замечание Владыки): место на платформе у берега для малого здания, которое «не жалко»
// (генератор, склады), когда суша у базы кончилась (острова — Three Isles): клетка воды или берега касается
// стороной здания главной подбазы (здание на платформе войдёт в базу), не выезд и не площадка, вне угроз.
// На клетке своя платформа — строить здание, нет — сначала платформу (*needPlatform = true). (−1,−1) — нет
cPosition findPlatformSpot (const cModel& model, const cPlayer& player, const cSubBase* main, const cStaticUnitData& data,
	const tFootprints& reserved, const cBaseDefense& defense, const cWorldModel& world, bool* needPlatform)
{
	cPosition best (-1, -1);
	if (main == nullptr) return best;
	const cMap& map = *model.getMap();
	const cPosition landing = player.getLandingPos();
	long long bestKey = std::numeric_limits<long long>::max();
	bool bestHas = false;
	auto ownPlatform = [&] (const cPosition& q) {
		for (const cBuilding* b : map.getField (q).getBuildings())
			if (b->getOwner() == &player && b->getStaticUnitData().ID == kIdPlatform) return true;
		return false;
	};
	const int R = kBaseRadius + 4;
	for (int dy = -R; dy <= R; ++dy)
		for (int dx = -R; dx <= R; ++dx)
		{
			const cPosition q = landing + cPosition (dx, dy);
			if (!map.isValidPosition (q) || (!map.isWater (q) && !map.isCoast (q))) continue;
			if (overlapsAny (q, false, reserved) || !defense.platformSpotOk (model, player, world, q, reserved)) continue;
			bool touches = false;
			for (const auto& s : sideNeighbours (q, false))
				if (hasMainBuilding (map, player, main, s)) touches = true;
			if (!touches) continue;
			const bool has = ownPlatform (q);
			if (has && !map.possiblePlaceBuilding (data, q, &player, map.getField (q).getVehicle())) continue;
			const long long key = (has ? 0LL : 1LL) * 1000000000LL + (q - landing).l2NormSquared() * 1000 + static_cast<long long> (q.y()) * map.getSize().x() + q.x();
			if (key < bestKey)
			{
				bestKey = key;
				best = q;
				bestHas = has;
			}
		}
	if (needPlatform) *needPlatform = best.x() >= 0 && !bestHas;
	return best;
}

// приказ Move по пути к dest (цель пути — в position, для журнала)
bool pushMove (const cVehicle& v, const cMapView& view, const cPosition& dest, int playerId,
	const char* level, const std::string& reason, std::vector<cOrder>& out)
{
	cPathCalculator pc (v, view, dest, false);
	auto path = pc.calcPath();
	if (path.empty()) return false;
	cOrder o;
	o.type = eOrderType::Move;
	o.playerId = playerId;
	o.unitId = v.getId();
	o.path = pathToVector (path);
	o.position = o.path.back();
	o.level = level;
	o.reason = reason;
	out.push_back (std::move (o));
	return true;
}

// то же, но мимо клеток под угрозой, если обход есть (стройщики, шаг 7)
bool pushMoveSafe (const cVehicle& v, const cMapView& view, const cPosition& dest, const cWorldModel& world,
	eThreatLayer layer, int playerId, const char* level, const std::string& reason, std::vector<cOrder>& out)
{
	auto path = threatAwarePath (v, view, dest, world, layer);
	if (path.empty()) return false;
	cOrder o;
	o.type = eOrderType::Move;
	o.playerId = playerId;
	o.unitId = v.getId();
	o.path = std::move (path);
	o.position = o.path.back();
	o.level = level;
	o.reason = reason;
	out.push_back (std::move (o));
	return true;
}

// ближайшая свободная клетка рядом со зданием главной подбазы (там стройщик берёт металл)
cPosition findBaseDock (const cMap& map, const cPlayer& player, const cSubBase* main, const cVehicle& v,
	const cMapView& view, const tFootprints& reserved, const cWorldModel& world, eThreatLayer layer)
{
	cPosition best (-1, -1);
	int bestDist = std::numeric_limits<int>::max();
	for (const auto& b : player.getBuildings())
	{
		if (b->subBase != main) continue;
		for (const auto& c : ringCells (b->getPosition(), b->getIsBig()))
		{
			if (!map.isValidPosition (c) || !view.possiblePlace (v, c) || overlapsAny (c, false, reserved)) continue;
			if (world.threatFor (v, c, layer) > 0) continue; // под угрозой врага — не сюда
			const int d = (c - v.getPosition()).l2NormSquared();
			if (d < bestDist)
			{
				bestDist = d;
				best = c;
			}
		}
	}
	return best;
}

// здание главной подбазы рядом со стройщиком — источник металла (как Transfer в GUI)
const cBuilding* refillSource (const cPlayer& player, const cSubBase* main, const cVehicle& v)
{
	if (main == nullptr || main->getMaxResourcesStored().metal <= 0) return nullptr;
	for (const auto& b : player.getBuildings())
		if (b->subBase == main && b->isNextTo (v.getPosition())) return b.get();
	return nullptr;
}

} // namespace

//------------------------------------------------------------------------------
std::vector<cOrder> cRuleBrain::decideTurn (int playerId, const cModel& model)
{
	std::vector<cOrder> orders;
	stats_ = {};
	holdReason_.clear();   // шаг 11: «стоять» и мир правил — события этого хода
	extraEvents_.clear();
	// события операций, не взятые движком (мозг внутри советника), — не копить без конца;
	// директива, заданная до хода, остаётся и уходит в журнал с событиями этого хода
	if (ops_.pendingEvents() > 200) ops_.takeEvents();
	// память о враге и карта угроз — до всех решений хода (шаг 7)
	if (const cPlayer* player = model.getPlayer (playerId); player != nullptr && !player->isDefeated)
	{
		const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
		world_.update (model, *player, turn);
		const auto* tank = player->getLastUnitData (kIdTank);
		const auto* fighter = player->getLastUnitData (kIdFighter);
		world_.buildThreat (*model.getMap(), tank ? tank->getArmor() : 0, fighter ? fighter->getArmor() : 0);
		// роли машин и состав армии (шаг 9) — до оперативки (производство) и микро (бой)
		roles_.build (model, *player);
		comp_.update (model, *player, roles_, world_, turn);
		// польза исследований и улучшений (шаг 10) — по составу и картине врага этого хода
		tech_.update (model, *player, roles_, comp_, world_, turn);
		// шаг 13: анализ карты (П3) и оборона базы (П4–П9, П18, П22) — до оперативки (стройка) и микро (охрана)
		const sEcon econ = readEcon (*player);
		defense_.update (model, *player, world_, roles_, comp_, econ.sb, econ.metalMaxProd, turn);
		comp_.setDefenseNeeds (defense_.pioneersWanted(), defense_.minelayersWanted(), defense_.surveyorsWanted(), defense_.scannersWanted());
		// шаг 13, часть 2: флот (П11–П14) — программа по упору карты, верфь, док, охрана стройки верфи
		const bool richNow = econ.metalMaxStored > 0 && econ.metalStored * 5 >= econ.metalMaxStored * 4;
		navy_.update (model, *player, world_, roles_, defense_, richNow, econ.metalMaxProd, turn);
		// шаг 13, часть 3: коммандос (П2, П18) и авиация (П15–П17, П10)
		const int bigFactories = countBuildings (*player, kIdFactory);
		commando_.update (model, *player, world_, roles_, defense_, econ.metalMaxProd, bigFactories, turn);
		air_.update (model, *player, world_, roles_, defense_, navy_, commando_, econ.metalMaxProd, bigFactories, turn);
		commando_.setFerryAvailable (air_.counts().transports > 0);
	}
	minesToggled_.clear();
	strategicDecisions (playerId, model, orders);
	operationalDecisions (playerId, model, orders);
	microDecisions (playerId, model, orders);
	// поведенческие цифры шага 7 — в журнал движка (maxr.log партии), только когда есть что сказать
	if (stats_.alarms > 0 || stats_.retreats > 0 || stats_.evacuations > 0 || stats_.holds > 0 || stats_.opMoves > 0 || stats_.opHolds > 0
		|| stats_.moveFire > 0 || stats_.kites > 0 || stats_.repairs > 0 || stats_.rearms > 0
		|| stats_.bought > 0 || stats_.refitLoads > 0 || stats_.refits > 0
		|| stats_.defenseBuilds > 0 || stats_.guardMoves > 0 || stats_.aaHolds > 0 || stats_.minesLaid > 0
		|| navy_.mode() != cNavy::eMode::None || navy_.counts().fleetMetal > 0
		|| air_.program() != cAirForce::eProgram::None || air_.counts().planeMetal > 0 || commando_.counts().commandos > 0)
	{
		int visible = 0;
		for (const auto& kv : world_.enemies())
			if (kv.second.visible) ++visible;
		Log.info ("ai_rule p" + std::to_string (playerId) + " t" + std::to_string (world_.turn())
			+ ": alarm " + std::to_string (stats_.alarms) + " intruders " + std::to_string (stats_.intruders)
			+ " defenders " + std::to_string (stats_.defenders) + " reserve " + std::to_string (stats_.reserve)
			+ " uncovered " + std::to_string (stats_.uncovered)
			+ " holds " + std::to_string (stats_.holds)
			+ " retreats " + std::to_string (stats_.retreats) + " unsafe " + std::to_string (stats_.unsafeRetreats)
			+ " infire " + std::to_string (stats_.fireRetreats)
			+ " evac " + std::to_string (stats_.evacuations)
			+ " memory " + std::to_string (visible) + "/" + std::to_string (world_.enemies().size())
			+ " opmoves " + std::to_string (stats_.opMoves) + " opholds " + std::to_string (stats_.opHolds)
			+ " movefire " + std::to_string (stats_.moveFire) + " kites " + std::to_string (stats_.kites)
			+ " repairs " + std::to_string (stats_.repairs) + " rearms " + std::to_string (stats_.rearms)
			+ " escorts " + std::to_string (stats_.escorts)
			+ " strategy " + comp_.strategy() + " aa " + std::to_string (comp_.needs().antiAir)
			+ " air " + std::to_string (comp_.enemy().air) + "/" + std::to_string (comp_.enemy().airFactories)
			+ " bought " + std::to_string (stats_.bought) + " refitloads " + std::to_string (stats_.refitLoads)
			+ " refits " + std::to_string (stats_.refits)
			// шаг 13: оборона и упор по карте
			+ " defense " + std::to_string (defense_.level()) + " radars " + std::to_string (defense_.counts().radars) + "/"
			+ std::to_string (defense_.counts().radarsWanted) + " aaguns " + std::to_string (defense_.counts().aa) + "/"
			+ std::to_string (defense_.counts().aaWanted) + " walls " + std::to_string (defense_.counts().walls) + " open "
			+ std::to_string (defense_.counts().wallsOpen) + " guards " + std::to_string (defense_.counts().guards) + "/"
			+ std::to_string (defense_.counts().posts) + " dbuilds " + std::to_string (stats_.defenseBuilds) + " gmoves "
			+ std::to_string (stats_.guardMoves) + " aaholds " + std::to_string (stats_.aaHolds) + " mines " + std::to_string (stats_.minesLaid)
			+ " cuts " + std::to_string (defense_.counts().cutPoints)
			+ " emphasis " + defense_.map().emphasis + (defense_.map().sure ? "" : "?")
			// шаг 13, часть 2: ракетная оборона берега (П25) и флот (П11–П14)
			+ " coastguns " + std::to_string (defense_.counts().coastGuns) + "/" + std::to_string (defense_.counts().coastGunsWanted)
			+ " navy " + navy_.modeName() + " ships " + std::to_string (navy_.counts().corvettes) + "c/" + std::to_string (navy_.counts().guns)
			+ "g/" + std::to_string (navy_.counts().subs) + "s/" + std::to_string (navy_.counts().transports) + "t yards "
			+ std::to_string (navy_.counts().shipyards) + " docks " + std::to_string (navy_.counts().docks) + " landing "
			+ navy_.landingStateName() + " cargo " + std::to_string (navy_.counts().cargo) + " landed " + std::to_string (navy_.counts().landed)
			+ " waves " + std::to_string (navy_.counts().waves)
			// шаг 13, часть 3: авиация и коммандос
			+ " air " + air_.programName() + " planes " + std::to_string (air_.counts().awacs) + "a/" + std::to_string (air_.counts().bombers)
			+ "b/" + std::to_string (air_.counts().fighters) + "f/" + std::to_string (air_.counts().transports) + "t airfac "
			+ std::to_string (air_.counts().airFactories) + " hangars " + std::to_string (air_.counts().hangars) + " strikes "
			+ std::to_string (air_.counts().strikes) + " hits " + std::to_string (air_.counts().hits) + " raids " + std::to_string (air_.counts().raids)
			+ " commandos " + std::to_string (commando_.counts().commandos) + "/" + std::to_string (commando_.commandosWanted()) + " rank "
			+ std::to_string (commando_.counts().maxRank) + " captures " + std::to_string (commando_.counts().captures) + " cdisables "
			+ std::to_string (commando_.counts().disables) + " aliens " + std::to_string (commando_.counts().aliensKnown));
	}
	// шаг 11 (для шага 16): явное «стоять» с причиной и мир правил — в журнал хода
	noteWorld (playerId, model, orders);
	// шаг 11: второй заход — когда есть чем стрелять: после движения и выстрелов этого захода
	// могут найтись цели, которые оставшиеся выстрелы убьют наверняка, и обнаруженные диверсанты
	wantPass_ = !minesToggled_.empty() || navy_.wantsPass() // шаг 13: минёр включил закладку — выключить в следующем заходе; флот — выгрузка
		|| air_.wantsPass() || commando_.wantsPass();          // шаг 13, ч. 3: второй выстрел, отход, налёт; коммандос подошёл — действовать
	if (const cPlayer* player = model.getPlayer (playerId); player != nullptr && !player->isDefeated)
		for (const auto& v : player->getVehicles())
			if (v->getStaticUnitData().canAttack > 0 && v->data.getShotsMax() > 0 && !v->isDisabled() && !v->isUnitLoaded())
			{
				wantPass_ = true;
				break;
			}
	return orders;
}

//------------------------------------------------------------------------------
// Шаг 11 (для шага 16): события журнала хода.
// «hold» — своя вооружённая машина в контакте (враг ближе 12 клеток, видимый или запомненный не
// старше 2 ходов) без приказа: причина — какое правило велело стоять, иначе «no task» (не было
// дела). «world» — мир правил: у своих машин роль, операция (лидер), угроза в клетке (зона огня
// и «удар с подъезда»), память о враге.
std::vector<nlohmann::json> cRuleBrain::takeJournalEvents()
{
	auto events = ops_.takeEvents();
	for (auto& e : defense_.takeEvents()) events.push_back (std::move (e)); // шаг 13: карта (П3) и оборона
	for (auto& e : navy_.takeEvents()) events.push_back (std::move (e)); // шаг 13, часть 2: флот
	for (auto& e : commando_.takeEvents()) events.push_back (std::move (e)); // шаг 13, часть 3: коммандос
	for (auto& e : air_.takeEvents()) events.push_back (std::move (e)); // шаг 13, часть 3: авиация
	for (auto& e : extraEvents_) events.push_back (std::move (e));
	extraEvents_.clear();
	return events;
}

void cRuleBrain::noteWorld (int playerId, const cModel& model, const std::vector<cOrder>& orders)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;
	std::vector<unsigned int> ordered;
	for (const auto& o : orders) ordered.push_back (o.unitId);
	std::sort (ordered.begin(), ordered.end());
	std::map<unsigned int, std::pair<int, bool>> opOf; // машина → операция, лидер ли
	for (const auto& op : ops_.operations())
		for (unsigned int id : op.members) opOf[id] = {op.id, id == op.leader};
	nlohmann::json units = nlohmann::json::array();
	for (const auto& v : player->getVehicles())
	{
		if (v->isUnitLoaded()) continue;
		const cPosition p = v->getPosition();
		const auto op = opOf.find (v->getId());
		units.push_back ({v->getId(), roleName (roles_.roleOf (*v)), op != opOf.end() ? op->second.first : 0,
			op != opOf.end() && op->second.second ? 1 : 0, world_.groundFire (p), world_.groundStrike (p)});
		// «стоять»: вооружённая свободная машина в контакте без приказа
		if (v->getStaticUnitData().canAttack == 0 || v->data.getShotsMax() <= 0 || !isIdle (*v)) continue;
		if (std::binary_search (ordered.begin(), ordered.end(), v->getId())) continue;
		bool contact = false;
		for (const auto& kv : world_.enemies())
			if (world_.age (kv.second) <= 2 && (kv.second.pos - p).l2NormSquared() <= 144)
			{
				contact = true;
				break;
			}
		if (!contact) continue;
		const auto r = holdReason_.find (v->getId());
		extraEvents_.push_back ({{"event", "hold"}, {"unitId", v->getId()}, {"pos", {{"x", p.x()}, {"y", p.y()}}},
			{"reason", r != holdReason_.end() ? r->second : std::string ("no task")}, {"level", "micro"}});
	}
	nlohmann::json memory = nlohmann::json::array();
	for (const auto& [id, m] : world_.enemies())
		memory.push_back ({id, m.owner, std::to_string (m.typeFirst) + "." + std::to_string (m.typeSecond), m.pos.x(), m.pos.y(),
			m.seenTurn, m.visible ? 1 : 0, m.hp});
	extraEvents_.push_back ({{"event", "world"}, {"level", "rule"},
		{"units", std::move (units)}, // [id, роль, операция (0 — нет), лидер, зона огня, удар с подъезда]
		{"memory", std::move (memory)}}); // [id, владелец, тип, x, y, ход наблюдения, виден, hp]
}

//------------------------------------------------------------------------------
// Шаг 11: заходы 2+ хода — приказы прошлого захода доиграны, поле новое. Только простое и
// безопасное (см. ai_rule_brain.h): обнаруженный диверсант — огонь всеми, кто достаёт; враг,
// которого оставшиеся выстрелы убьют наверняка, — ровно столько выстрелов, сколько нужно.
// Один приказ Attack — один выстрел; у кого выстрелы остались, выстрелит в следующем заходе.
std::vector<cOrder> cRuleBrain::decidePass (int playerId, const cModel& model, int pass)
{
	std::vector<cOrder> out;
	wantPass_ = false;
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return out;
	// шаг 13 (П8): минёр включил закладку в прошлом заходе (мина легла) — выключить: иначе мина ляжет
	// на каждой клетке его пути
	for (unsigned int id : minesToggled_)
		if (const cVehicle* v = model.getVehicleFromID (id); v != nullptr && v->getOwner() == player && v->isUnitLayingMines())
		{
			cOrder o;
			o.type = eOrderType::ToggleLayMines;
			o.playerId = playerId;
			o.unitId = id;
			o.level = "micro";
			o.reason = std::string ("pass ") + std::to_string (pass) + ": minelayer: stop laying";
			out.push_back (std::move (o));
		}
	minesToggled_.clear();
	// шаг 13, часть 2: флот — выгрузка у берега (транспорт доплыл в прошлом заходе), второй выстрел подлодки
	for (auto& o : navy_.planPass (model, *player, world_, roles_, pass)) out.push_back (std::move (o));
	const auto& map = model.getMap();
	cMapView view (map, sharedPlayerOf (model, playerId));
	// шаг 13, часть 3: коммандос подошёл — выключить или захватить; авиация — налёт, второй выстрел, отход, высадка
	for (auto& o : commando_.planPass (model, *player, view, world_, pass)) out.push_back (std::move (o));
	for (auto& o : air_.planPass (model, *player, view, world_, roles_, commando_, pass)) out.push_back (std::move (o));
	const bool modulesWant = air_.wantsPass() || commando_.wantsPass();
	std::vector<sEnemy> enemies;
	collectVisibleEnemies (*player, model, view, enemies);
	if (enemies.empty())
	{
		wantPass_ = modulesWant;
		return out;
	}

	// стрелки: свободные боевые машины с выстрелом и боезапасом (разведчики не стреляют — разведка)
	std::vector<const cVehicle*> shooters;
	for (const auto& v : player->getVehicles())
		if (isIdle (*v) && isCombatUnit (*v) && v->data.getAmmo() > 0 && !v->isAttacking() && !v->isBeingAttacked()
			&& roles_.roleOf (*v) != eRole::Scout && roles_.roleOf (*v) != eRole::Submarine // шаг 13, ч. 2: подлодка стреляет только по кораблям (флот)
			&& !air_.claimed (v->getId()) && !commando_.claimed (v->getId()) // шаг 13, ч. 3: самолёты и коммандос — свои модули
			&& !isAirRole (roles_.roleOf (*v)) && roles_.roleOf (*v) != eRole::Commando)
			shooters.push_back (v.get());
	if (shooters.empty())
	{
		wantPass_ = modulesWant; // как прежде: стрелков нет — заход только ради модулей части 3
		return out;
	}
	std::vector<bool> used (shooters.size(), false);

	// кто бьёт цель e и сколько за выстрел (кассетная — не по цели, у которой рядом свои);
	// порядок: больше урона, ближе, меньший id — детерминированно
	const auto candidates = [&] (const sEnemy& e) {
		std::vector<std::tuple<int, long long, unsigned int, size_t>> c; // −урон, дистанция², id, индекс
		for (size_t a = 0; a < shooters.size(); ++a)
		{
			if (used[a]) continue;
			const cVehicle& s = *shooters[a];
			if (!s.canAttackObjectAt (e.pos, view, false)) continue;
			// выстрел попадёт именно в эту цель (на клетке может стоять и другая машина)
			if (cAttackJob::selectTarget (e.pos, s.getStaticUnitData().canAttack, view, player) != e.unit) continue;
			if (s.getStaticUnitData().muzzleType == eMuzzleType::RocketCluster && ownWithinSplash (*map, player, e.pos)) continue;
			c.emplace_back (-effectiveDamagePerShot (s.data.getDamage(), e.armor), (e.pos - s.getPosition()).l2NormSquared(), s.getId(), a);
		}
		std::sort (c.begin(), c.end());
		return c;
	};
	const auto fire = [&] (size_t a, const sEnemy& e, const char* reason) {
		used[a] = true;
		cOrder o;
		o.type = eOrderType::Attack;
		o.playerId = playerId;
		o.unitId = shooters[a]->getId();
		o.position = e.pos;
		o.level = "micro";
		o.reason = std::string ("pass ") + std::to_string (pass) + ": " + reason;
		out.push_back (std::move (o));
	};

	// 1) обнаруженные диверсанты (скрытые машины врага, видимые сейчас): огонь всеми, кто достаёт,
	//    пока урона хватает на hp — со следующего своего хода диверсант снова невидим
	int infiltratorShots = 0;
	for (const auto& e : enemies)
	{
		if (!e.unit->isAVehicle() || e.unit->getStaticUnitData().isStealthOn == 0) continue;
		int dealt = 0;
		for (const auto& c : candidates (e))
		{
			if (dealt >= e.hitpoints) break;
			dealt -= std::get<0> (c);
			fire (std::get<3> (c), e, "detected infiltrator");
			++infiltratorShots;
		}
	}

	// 2) добить: цели, которые свободные стрелки убьют этим залпом наверняка (урон без случайности:
	//    max(1, урон − броня)); раньше — опасные машины, потом остальное; при равенстве — меньше hp, id
	std::vector<size_t> order (enemies.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	const auto danger = [&] (const sEnemy& e) { return e.unit->isAVehicle() ? static_cast<long long> (e.damage) * std::max (1, e.shots) : 0LL; };
	std::sort (order.begin(), order.end(), [&] (size_t x, size_t y) {
		const auto& a = enemies[x];
		const auto& b = enemies[y];
		if (danger (a) != danger (b)) return danger (a) > danger (b);
		if (a.hitpoints != b.hitpoints) return a.hitpoints < b.hitpoints;
		return a.unit->getId() < b.unit->getId();
	});
	int kills = 0;
	for (size_t i : order)
	{
		const sEnemy& e = enemies[i];
		if (e.unit->isAVehicle() && e.unit->getStaticUnitData().isStealthOn != 0) continue; // уже выше
		if (e.unit->isABuilding() && e.unit->data.getBuildCost() <= 2) continue; // дороги, соединители — не цель
		const auto c = candidates (e);
		int dealt = 0;
		size_t need = 0;
		while (need < c.size() && dealt < e.hitpoints) dealt -= std::get<0> (c[need++]);
		if (dealt < e.hitpoints) continue; // наверняка не убить — выстрел бережём на ответный огонь
		for (size_t k = 0; k < need; ++k)
			fire (std::get<3> (c[k]), e, "finish off (sure kill)");
		++kills;
	}

	// ещё заход — если стреляли: у стрелков могли остаться выстрелы, а поле изменилось
	wantPass_ = !out.empty() || modulesWant;
	if (!out.empty())
		Log.info ("ai_rule p" + std::to_string (playerId) + " t" + std::to_string (world_.turn()) + " pass " + std::to_string (pass)
			+ ": kills " + std::to_string (kills) + " infiltrator shots " + std::to_string (infiltratorShots)
			+ " orders " + std::to_string (out.size()));
	return out;
}

//------------------------------------------------------------------------------
// Стратегия: исследования и улучшения за золото.
// Шаг 10: тема исследований — по пользе против видимого врага (ai_tech), все центры на
// одной теме (klei1984); кредиты — на улучшения с наибольшим приростом силы на кредит.
void cRuleBrain::strategicDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;

	// покупки за кредиты: улучшения типа действуют на новые машины (стоящие — обновление в депо)
	if (player->getCredits() > 0)
	{
		int income = 0; // кредитов в ход: золото, что берут работающие переработки
		for (const auto& sb : player->base.SubBases)
			income += sb->getResourcesNeeded().gold;
		int credits = player->getCredits();
		// шаг 13, часть 2 (П25): дальность ракетной пушки +1 за золото — у моря их ≥ 2, видны корабли или верфь врага
		// (или упор «море»): подойти и подплыть станет совсем трудно, а скаут (обзор 9) не видит, откуда бьют
		if (defense_.counts().coastGuns >= 2 && (defense_.enemyShipMetal() > 0 || defense_.enemyShipyardSeen() || navy_.mode() == cNavy::eMode::Sea))
		{
			const cDynamicUnitData* cur = player->getLastUnitData (kIdGunMissile);
			const cDynamicUnitData& orig = model.getUnitsData()->getDynamicUnitData (kIdGunMissile, player->getClan());
			std::array<int, 8> steps {};
			steps[2] = 1; // дальность (порядок kUpgradeKinds)
			cUnitUpgrade upg;
			int cost = 0;
			if (cur != nullptr && cur->getRange() <= orig.getRange() && makeUnitUpgrade (model, *player, kIdGunMissile, steps, upg, cost)
				&& cost <= credits)
			{
				cOrder o;
				o.type = eOrderType::BuyUpgrades;
				o.playerId = playerId;
				o.unitType = {kIdGunMissile.firstPart, kIdGunMissile.secondPart};
				o.upgradeSteps = steps;
				o.upgradeCost = cost;
				o.level = "strategy";
				o.reason = "buy upgrades: coastal missile gun range+1 (enemy fleet: outrange gunboats and missile ships)";
				out.push_back (std::move (o));
				stats_.bought += cost;
				credits -= cost;
			}
		}
		// шаг 13, часть 3 (П10): дальность истребителя +1 за золото — вражеские истребители первыми получат урон, им дальше
		// облетать зону поражения, а свои бьют и уходят в невидимость; (П2): обзор коммандос +1 — увидеть пехоту и коммандос
		// врага раньше, чем они его, и обойти
		auto buyOne = [&] (const sID& id, int kind, int have, int need, const char* why) {
			if (have < need) return;
			const cDynamicUnitData* cur = player->getLastUnitData (id);
			if (cur == nullptr) return;
			const cDynamicUnitData& orig = model.getUnitsData()->getDynamicUnitData (id, player->getClan());
			const int curV = kind == 2 ? cur->getRange() : cur->getScan();
			const int origV = kind == 2 ? orig.getRange() : orig.getScan();
			if (curV > origV) return; // уже куплено (или исследовано)
			std::array<int, 8> steps {};
			steps[kind] = 1;
			cUnitUpgrade upg;
			int cost = 0;
			if (!makeUnitUpgrade (model, *player, id, steps, upg, cost) || cost > credits) return;
			cOrder o;
			o.type = eOrderType::BuyUpgrades;
			o.playerId = playerId;
			o.unitType = {id.firstPart, id.secondPart};
			o.upgradeSteps = steps;
			o.upgradeCost = cost;
			o.level = "strategy";
			o.reason = why;
			out.push_back (std::move (o));
			stats_.bought += cost;
			credits -= cost;
		};
		buyOne (roles_.typeFor (eRole::Fighter, "Plane"), 2, air_.counts().fighters, 2,
			"buy upgrades: fighter range+1 (П10: enemy fighters take the first hit, ours strike and leave)");
		buyOne (roles_.typeFor (eRole::Commando, "Human"), 6, commando_.counts().commandos, 2,
			"buy upgrades: commando scan+1 (П2: see enemy infantry and commandos before they see us)");
		for (const auto& p : tech_.purchases (model, *player, credits, income))
		{
			cOrder o;
			o.type = eOrderType::BuyUpgrades;
			o.playerId = playerId;
			o.unitType = {p.id.firstPart, p.id.secondPart};
			o.upgradeSteps = p.steps;
			o.upgradeCost = p.cost;
			o.level = "strategy";
			std::string what;
			for (size_t kk = 0; kk < p.steps.size(); ++kk)
				if (p.steps[kk] > 0) what += std::string (" ") + upgradeKindName (static_cast<int> (kk)) + "+" + std::to_string (p.steps[kk]);
			o.reason = std::string ("buy upgrades: ") + roleName (roles_.roleOfType (p.id)) + what;
			out.push_back (std::move (o));
			stats_.bought += p.cost;
		}
	}

	// есть работающие исследовательские центры — направляем их
	const int total = player->getResearchCentersWorkingTotal();
	if (total <= 0) return;

	using RA = cResearch::eResearchArea;
	cOrder o;
	o.type = eOrderType::ChangeResearch;
	o.playerId = playerId;
	o.level = "strategy";
	const auto plan = tech_.researchPlan (model, *player, total);
	bool same = true;
	for (int a = 0; a < 8; ++a)
	{
		o.researchAreas[a] = plan[a];
		if (player->getResearchCentersWorkingOnArea (static_cast<RA> (a)) != plan[a]) same = false;
	}
	if (same) return; // центры уже так — приказ не нужен
	o.reason = tech_.researchWhy();
	out.push_back (std::move (o));
}

//------------------------------------------------------------------------------
// Оперативка: стройка, производство, снабжение стройщиков — без даровой подпитки.
//
// Металл хода (бюджет) — stored + prod - needed главной подбазы: столько можно
// забрать, не заглушив к концу хода работающие фабрики (иначе движок
// отказывает в сдаче хода). Сначала пополняются стройщики (шахты, энергия,
// фабрики — вложения), остаток — фабрикам. Порядок приказов: пополнение →
// производство → стройка → запуск шахт; сервер исполняет их по порядку.
void cRuleBrain::operationalDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;

	const auto& unitsData = model.getUnitsData();
	const cMap& map = *model.getMap();
	const cMapView view (model.getMap(), sharedPlayerOf (model, playerId));

	const sEcon econ = readEcon (*player);
	const cSubBase* main = econ.sb;
	opMoved_.clear();
	const eThreatLayer bl = kBuilderLayer; // слой угроз для стройщиков (шаг 7)
	// малая фабрика (шаг 9) — здание, что строит малые наземные машины (по данным)
	sID smallFactoryId;
	for (const auto& sd : unitsData->getStaticUnitsData())
		if (sd.ID.isABuilding() && sd.canBuild == "SmallGroundVehicle" && !sd.isAlien)
		{
			smallFactoryId = sd.ID;
			break;
		}

	sConnGrid grid;
	buildConnGrid (model, *player, main, grid);
	// шаг 13, часть 2: верфь и док — на воде, а море бывает дальше от базы: для них цепочка соединителей до
	// kMaxChainWater (длинная сетка — только когда флоту нужно место или связь)
	auto isNavalYard = [] (const sID& id) { return id == kIdShipyard || id == kIdDock; };
	bool needLong = navy_.wantShipyard() || navy_.wantDock();
	for (const auto& b : player->getBuildings())
		if (isNavalYard (b->getStaticUnitData().ID) && b->subBase != main) needLong = true;
	for (const auto& ov : player->getVehicles())
		if (ov->isUnitBuildingABuilding() && isNavalYard (ov->getBuildingType())) needLong = true;
	sConnGrid gridLong;
	if (needLong) buildConnGrid (model, *player, main, gridLong, kMaxChainWater);
	const sConnGrid& gridWater = needLong ? gridLong : grid;

	// планы машин, которых больше нет, которые уже строят или застряли без пути, — забыть
	const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	for (auto it = stuck_.begin(); it != stuck_.end();)
	{
		const cVehicle* v = model.getVehicleFromID (it->first);
		if (v == nullptr || v->getOwner() != player || (it->second >= kStuckTurns && turn % 10 == 0))
			it = stuck_.erase (it); // раз в 10 ходов — попробовать снова (карман мог открыться)
		else
			++it;
	}
	auto isStuck = [&] (const cVehicle& v) {
		const auto it = stuck_.find (v.getId());
		return it != stuck_.end() && it->second >= kStuckTurns;
	};
	for (auto it = plans_.begin(); it != plans_.end();)
	{
		const cVehicle* v = model.getVehicleFromID (it->first);
		if (v == nullptr || v->getOwner() != player || v->isUnitBuildingABuilding() || isStuck (*v))
			it = plans_.erase (it);
		// шаг 13, часть 2: план верфи или дока, который флоту больше не нужен (программа сменилась), — снять
		else if ((sID (it->second.typeFirst, it->second.typeSecond) == kIdShipyard && !navy_.wantShipyard()) || (sID (it->second.typeFirst, it->second.typeSecond) == kIdDock && !navy_.wantDock()))
			it = plans_.erase (it);
		else
			++it;
	}

	auto planType = [] (const sBuildPlan& p) { return sID (p.typeFirst, p.typeSecond); };
	// построено + строится + в планах
	auto totalOf = [&] (const sID& id) {
		int n = countBuildings (*player, id) + countConstructorsBuilding (*player, id);
		for (const auto& kv : plans_)
			if (planType (kv.second) == id) ++n;
		return n;
	};
	// энергия главной базы с учётом строящегося и запланированного: {спрос, станции, генераторы}
	auto energyState = [&] () {
		int demand = econ.energyDemand, stations = econ.stations, generators = econ.generators;
		auto add = [&] (const sID& id) {
			demand += unitsData->getStaticUnitData (id).needsEnergy;
			if (id == kIdEnergy) ++stations;
			else if (id == kIdEnergySmall) ++generators;
		};
		for (const auto& v : player->getVehicles())
			if (v->isUnitBuildingABuilding()) add (v->getBuildingType());
		for (const auto& kv : plans_) add (planType (kv.second));
		return std::array<int, 3> {demand, stations, generators};
	};
	// строящееся сейчас (стройщик стоит на площадке) — его площадки тоже заняты
	tFootprints inProgress;
	for (const auto& v : player->getVehicles())
		if (v->isUnitBuildingABuilding()) inProgress.emplace_back (v->getPosition(), v->getIsBig());
	auto reservedFootprints = [&] () {
		tFootprints r = inProgress;
		for (const auto& kv : plans_)
			r.emplace_back (kv.second.pos, unitsData->getStaticUnitData (planType (kv.second)).buildingData.isBig);
		return r;
	};
	// план ещё выполним: площадка свободна от зданий и чужой стройки и не под угрозой врага
	auto planValid = [&] (const sBuildPlan& p) {
		const auto& sd = unitsData->getStaticUnitData (planType (p));
		return canPlaceIgnoringVehicles (map, *player, sd, p.pos) && !overlapsAny (p.pos, sd.buildingData.isBig, inProgress)
			&& !footprintThreatened (world_, p.pos, sd.buildingData.isBig, bl);
	};

	const bool rich = econ.metalMaxStored > 0 && econ.metalStored * 5 >= econ.metalMaxStored * 4; // склад ≥ 80%
	// фабрик — по добыче металла; склад полон — ещё одна (металл есть, мощностей нет)
	const int factoryTarget = std::clamp ((econ.metalMaxProd + 5) / 8 + (rich ? 1 : 0), 1, kMaxFactories);

	struct sRefill
	{
		const cVehicle* v;
		const cBuilding* src;
		int want;
	};
	std::vector<sRefill> refills;
	std::vector<cOrder> builderOrders; // стройка и движение стройщиков — до производства (выезд не займёт площадку)

	auto pushFinishBuild = [&] (const cVehicle& v) {
		const cPosition esc = findVehicleEscape (model, v);
		if (esc.x() < 0) return;
		cOrder o;
		o.type = eOrderType::FinishBuild;
		o.playerId = playerId;
		o.unitId = v.getId();
		o.position = esc;
		o.level = "op";
		o.reason = "finish building";
		builderOrders.push_back (std::move (o));
	};
	// стройщику не хватает металла: у базы — пополнить, иначе — к базе
	auto needMetal = [&] (const cVehicle& v, int want) {
		if (const cBuilding* src = refillSource (*player, main, v))
			refills.push_back ({&v, src, want});
		else
		{
			const cPosition dock = findBaseDock (map, *player, main, v, view, reservedFootprints(), world_, bl);
			if (dock.x() >= 0) pushMoveSafe (v, view, dock, world_, bl, playerId, "op", "to base for metal", builderOrders);
		}
	};
	// план стройщика: стоит на площадке — строит, иначе идёт к ней
	auto executePlan = [&] (const cVehicle& v, sBuildPlan& plan) {
		const sID what = planType (plan);
		const auto& bdata = unitsData->getStaticUnitData (what);
		const bool big = bdata.buildingData.isBig;
		const int buildCost = player->getLastUnitData (what)->getBuildCost();
		if (v.getStoredResources() < plan.cost)
		{
			// вдали от базы на турбо не хватает, но на обычную скорость есть — строить сейчас
			const int gap = plan.cost - v.getStoredResources();
			const bool refillSoon = refillSource (*player, main, v) != nullptr
				&& econ.metalStored + 3 * std::max (0, econ.metalProd - econ.metalNeeded) >= gap;
			if (plan.speed > 0 && v.getStoredResources() >= buildCost && !refillSoon)
			{
				plan.speed = 0;
				plan.cost = buildCost;
			}
			else
			{
				needMetal (v, std::min (plan.cost, v.getStaticUnitData().storageResMax) - v.getStoredResources());
				return;
			}
		}
		if (!inFootprint (v.getPosition(), plan.pos, big))
		{
			// ближайшая к стройщику клетка площадки, куда можно встать
			cPosition best (-1, -1);
			int bestDist = std::numeric_limits<int>::max();
			for (const auto& c : footprintCells (plan.pos, big))
			{
				if (!view.possiblePlace (v, c)) continue;
				const int d = (c - v.getPosition()).l2NormSquared();
				if (d < bestDist)
				{
					bestDist = d;
					best = c;
				}
			}
			if (best.x() >= 0 && pushMoveSafe (v, view, best, world_, bl, playerId, "op", "to build site: " + bdata.getDefaultName(), builderOrders))
				stuck_.erase (v.getId());
			else
				++stuck_[v.getId()]; // пути нет — после трёх ходов план снимается
			return;
		}
		for (const auto& c : footprintCells (plan.pos, big))
			if (!map.possiblePlaceBuilding (bdata, c, player, &v)) return; // площадку заняли — ждём
		const int speed = std::min (plan.speed, maxFeasibleBuildSpeed (v, buildCost));
		cOrder o;
		o.type = eOrderType::Build;
		o.playerId = playerId;
		o.unitId = v.getId();
		o.buildingType = {what.firstPart, what.secondPart};
		o.position = plan.pos;
		o.buildSpeed = speed;
		o.level = "op";
		o.reason = "build " + bdata.getDefaultName() + (speed > 0 ? " (turbo)" : "");
		builderOrders.push_back (std::move (o));
	};
	// turbo: скорость 1 за ×2 металла — когда металл всё равно лишний (или это первая фабрика)
	auto makePlan = [&] (const cVehicle& v, const sID& what, const cPosition& pos, bool turbo) -> sBuildPlan& {
		const int cost0 = player->getLastUnitData (what)->getBuildCost();
		const int cost1 = builderTurboCost (cost0, v.getStaticUnitData().needsMetal, v.getStaticUnitData().storageResMax);
		const bool fast = cost1 > 0 && (turbo || rich || econ.free >= cost1 + 12);
		sBuildPlan p;
		p.typeFirst = what.firstPart;
		p.typeSecond = what.secondPart;
		p.pos = pos;
		p.speed = fast ? 1 : 0;
		p.cost = fast ? cost1 : cost0;
		return plans_.insert_or_assign (v.getId(), p).first->second;
	};
	// своя машина без дела мешает (площадка, выезд фабрики) — отогнать на 3–6 клеток от center
	std::vector<unsigned int> shooed;
	auto shooAway = [&] (const cVehicle& u, const cPosition& center, const char* reason) {
		if (u.getOwner() != player || !isIdle (u)) return;
		if (std::find (shooed.begin(), shooed.end(), u.getId()) != shooed.end()) return;
		for (const auto& o : builderOrders)
			if (o.unitId == u.getId()) return;
		const tFootprints reserved = reservedFootprints();
		cPosition dest (-1, -1);
		int bestDist = std::numeric_limits<int>::max();
		for (int dy = -6; dy <= 6; ++dy)
			for (int dx = -6; dx <= 6; ++dx)
			{
				const cPosition n = center + cPosition (dx, dy);
				if (std::max (std::abs (dx), std::abs (dy)) < 3) continue;
				if (!map.isValidPosition (n) || !view.possiblePlace (u, n) || overlapsAny (n, false, reserved)) continue;
				const int d = (n - u.getPosition()).l2NormSquared();
				if (d < bestDist)
				{
					bestDist = d;
					dest = n;
				}
			}
		if (dest.x() >= 0 && pushMove (u, view, dest, playerId, "op", reason, builderOrders))
		{
			shooed.push_back (u.getId());
			opMoved_.push_back (u.getId());
		}
	};
	// стройщик под угрозой врага — уйти в ближайшую безопасную клетку (ближе к высадке)
	auto evacuate = [&] (const cVehicle& v) {
		if (world_.threatFor (v, v.getPosition(), bl) <= 0) return false;
		cPosition dest;
		if (!world_.nearestSafeCell (v, view, v.getPosition(), kEvacuateRadius, player->getLandingPos(), dest, bl)) return false;
		if (!pushMoveSafe (v, view, dest, world_, bl, playerId, "op", "builder evacuates (threat)", builderOrders)) return false;
		++stats_.evacuations;
		return true;
	};
	// без дела вдали от базы — вернуться (не стоять на пути соединителей и выезде фабрик)
	auto idleToBase = [&] (const cVehicle& v) {
		if (refillSource (*player, main, v) != nullptr) return;
		const cPosition dock = findBaseDock (map, *player, main, v, view, reservedFootprints(), world_, bl);
		if (dock.x() >= 0) pushMoveSafe (v, view, dock, world_, bl, playerId, "op", "idle builder to base", builderOrders);
	};

	// 1) конструкторы: каждый строит своё здание, по нужде экономики
	const auto& factoryData = unitsData->getStaticUnitData (kIdFactory);
	int smallSpot = -1; // шаг 9: есть ли место малой фабрике (−1 — не проверяли)
	for (const auto& vp : player->getVehicles())
	{
		const cVehicle& v = *vp;
		if (v.getStaticUnitData().canBuild != "BigBuilding" || v.isDisabled()) continue;
		if (v.isUnitBuildingABuilding())
		{
			if (v.getBuildTurns() == 0) pushFinishBuild (v); // иначе здание не появится
			continue;
		}
		if (v.isUnitMoving() || v.getMoveJob() != nullptr) continue;

		// заперт своими зданиями (высадка в угол) — освободить проход, как в GUI
		if (const cBuilding* blocker = trappingBuilding (map, *player, factoryData, v))
		{
			cOrder o;
			o.type = eOrderType::SelfDestroy;
			o.playerId = playerId;
			o.unitId = blocker->getId();
			o.level = "op";
			o.reason = "free trapped constructor";
			builderOrders.push_back (std::move (o));
			continue;
		}

		if (evacuate (v)) continue; // под угрозой — не строить, уйти

		if (isStuck (v)) continue; // заперт без пути — его стройку не ждать

		auto it = plans_.find (v.getId());
		if (it != plans_.end() && !planValid (it->second))
		{
			plans_.erase (it); // площадку заняли
			it = plans_.end();
		}
		// шаг 9: видна авиация врага, а малой фабрики нет ни в стройке, ни в планах — прежний план
		// подождёт: ПВО больше негде взять (опыт: план шахты держал конструктора 25 ходов под бомбами)
		if (it != plans_.end() && comp_.needs().antiAir > 0 && smallFactoryId != sID() && totalOf (smallFactoryId) == 0
			&& planType (it->second) != smallFactoryId && countBuildings (*player, kIdFactory) >= 1)
		{
			// только если малой фабрике есть место (иначе конструкторы стояли бы без дела)
			if (smallSpot < 0)
				smallSpot = findBaseSpot (model, *player, main, unitsData->getStaticUnitData (smallFactoryId), reservedFootprints(), world_, bl).x() >= 0 ? 1 : 0;
			if (smallSpot == 1)
			{
				plans_.erase (it);
				it = plans_.end();
			}
		}
		if (it == plans_.end())
		{
			// энергия — по нефти, которую шахты дают без среза металла
			const auto e = energyState();
			const bool energyShort = !energyFeasible (e[0] + 1, e[1], e[2], econ.oilFree);
			const bool stationHelps = energyFeasible (e[0] + 1, e[1] + 1, e[2], econ.oilFree);
			const bool generatorHelps = energyFeasible (e[0] + 1, e[1], e[2] + 1, econ.oilFree);
			const bool needOil = energyShort && !stationHelps && !generatorHelps;
			const tFootprints reserved = reservedFootprints();
			// шаг 10: переработка золота — столько, сколько золота шахты дадут без среза металла
			const int nRefinery = totalOf (kIdGoldRefinery);
			const int goldAvail = goldAvailable (main);
			const int refineryTarget = goldAvail >= kGoldMin ? std::clamp (goldAvail / 5, 1, 3) : 0;
			// площадки шахт с золотом ищем, как только переработка по плану (иначе золоту неоткуда взяться)
			const int goldW = totalOf (kIdFactory) >= kGoldMinFactories ? kGoldMineWeight : 0;
			const sSite site = findMineSite (model, *player, grid, reserved, needOil, world_, bl, goldW);
			// нефти рядом нет — станция ценой среза металла у шахт (лучше, чем стоять без энергии)
			const bool cutFeasible = energyFeasible (e[0] + 1, e[1], e[2], econ.oil); // хватает и так, ценой среза
			const bool stationByCut = needOil && !(site.valid() && site.oil >= kMinMineOil) && !cutFeasible
				&& energyFeasible (e[0] + 1, e[1] + 1, e[2], econ.oil);
			// новые потребители без энергии только стоят
			const bool consumersOk = !energyShort || generatorHelps || cutFeasible;
			const int nFactory = totalOf (kIdFactory);
			std::vector<sID> wants; // по приоритету; нет места — берём следующее
			if (nFactory < 1) wants.push_back (kIdFactory);
			if (energyShort && (stationHelps || stationByCut)) wants.push_back (kIdEnergy);
			// шаг 13 (П7): учебный центр — пехота видит коммандос; ему нужен человек (жилой блок даёт 3)
			const bool trainingWanted = (defense_.wantTrainingHall() || commando_.wantTrainingHall()) && consumersOk && totalOf (kIdTraining) < 1
				&& (defense_.commandoAlarm() || rich || commando_.wantTrainingHall()); // без тревоги — из лишнего металла (склад ≥ 80%); шаг 13, ч. 3 — коммандос (чужие, диверсия)
			const bool habitatForTraining = trainingWanted
				&& econ.humanFree + 3 * (totalOf (kIdHabitat) - countBuildings (*player, kIdHabitat)) < 1;
			if (trainingWanted && defense_.commandoAlarm())
			{
				if (habitatForTraining) wants.push_back (kIdHabitat);
				wants.push_back (kIdTraining);
			}
			// шаг 9: малая фабрика (разведчики, ремонтники, ПВО) — когда доход тянет; видна авиация
			// врага или её фабрика — сразу после первой большой и раньше шахты (ПВО больше негде взять)
			const bool airAlarm = comp_.needs().antiAir > 0;
			const bool smallOk = smallFactoryId != sID() && consumersOk && nFactory >= 1
				&& (airAlarm || econ.metalMaxProd >= kSmallFactoryIncome || defense_.level() >= 2) && totalOf (smallFactoryId) < kMaxSmallFactories;
			if (smallOk && airAlarm)
				wants.insert (wants.begin() + (nFactory < 1 ? 1 : 0), smallFactoryId);
			// шаг 13, часть 2 (П11): верфь — упор «море» (враг за водой): сразу после первой фабрики, раньше шахт;
			// «берег» — после фабрик (из лишнего металла или в ответ на флот врага)
			const bool yardOk = navy_.wantShipyard() && consumersOk && nFactory >= 1 && totalOf (kIdShipyard) < 1;
			if (yardOk && navy_.shipyardEarly()) wants.push_back (kIdShipyard);
			// шаг 13, часть 3 (п. 100): подлодки врага у нашей верфи и кораблей, или враг только за водой без моря у базы — авиазавод
			// и ангар раньше шахт (бомбардировщик бьёт подлодку; ждать «экономики» — потерять верфь, как в части 2)
			const bool airUrgent = air_.factoryUrgent() && consumersOk && nFactory >= 1;
			if (airUrgent && air_.wantAirFactory() && totalOf (kIdAirFactory) < 1) wants.push_back (kIdAirFactory);
			if (air_.factoryUrgent() && air_.wantHangar() && totalOf (kIdHangar) < 1) wants.push_back (kIdHangar);
			// переработка — раньше шахт: после шахт строилась к ходу ~80 и стоила 0.035 D (отчёт §4)
			if (consumersOk && nFactory >= kGoldMinFactories && nRefinery < refineryTarget) wants.push_back (kIdGoldRefinery);
			if (site.valid()) wants.push_back (kIdMine);
			// шаг 13, часть 2 (П14): док — ремонт кораблей (флот от четырёх кораблей)
			if (navy_.wantDock() && consumersOk && totalOf (kIdDock) < 1) wants.push_back (kIdDock);
			if (trainingWanted && !defense_.commandoAlarm())
			{
				if (habitatForTraining) wants.push_back (kIdHabitat);
				wants.push_back (kIdTraining);
			}
			if (consumersOk && nFactory >= 1 && nFactory < factoryTarget) wants.push_back (kIdFactory);
			// шаг 13, часть 3 (П15–П17, П10): авиазавод и ангар — после фабрик (экономика сначала: программа авиации
			// включается по доходу и ходу, урок флота части 2)
			if (air_.wantAirFactory() && consumersOk && nFactory >= 1 && totalOf (kIdAirFactory) < 1) wants.push_back (kIdAirFactory);
			if (air_.wantHangar() && totalOf (kIdHangar) < 1) wants.push_back (kIdHangar);
			if (yardOk && !navy_.shipyardEarly()) wants.push_back (kIdShipyard);
			// шаг 13 (П4): депо — укрытие мобильного радара (и обновление машин)
			if (defense_.wantDepot() && rich && consumersOk && nFactory >= 1 && totalOf (kIdDepot) < 1) wants.push_back (kIdDepot);
			if (smallOk && !airAlarm) wants.push_back (smallFactoryId); // после больших фабрик (раньше — в шуме, отчёт §4)
			// шаг 10: депо — обновлять отставшие машины (улучшения и исследования — только новым)
			if (consumersOk && nFactory >= 1 && totalOf (kIdDepot) < 1)
			{
				int behind = 0;
				for (const auto& ov : player->getVehicles())
					if (isArmyRole (roles_.roleOf (*ov)) && refitGap (*ov) >= kRefitGap) ++behind;
				if (behind >= kDepotMin) wants.push_back (kIdDepot);
			}
			// шаг 10: центров исследований — по доходу металла (центр на kResearchPerIncome), с двух
			// фабрик и добычи 16; жилой блок — на 3 центра (идея klei1984: центров столько же, сколько
			// теплиц, теплиц — от числа фабрик). С добычи 12 — в ущерб армии (отчёт §4)
			{
				int rcTarget = 0;
				if (nFactory >= kResearchMinFactories && econ.metalMaxProd >= kResearchMinIncome)
					rcTarget = std::clamp (econ.metalMaxProd / kResearchPerIncome, 1, kResearchMax);
				const int nResearch = totalOf (kIdResearch);
				const int nHabitat = totalOf (kIdHabitat);
				if (consumersOk && nHabitat < (rcTarget + 2) / 3) wants.push_back (kIdHabitat);
				if (consumersOk && nResearch < rcTarget && nHabitat * 3 > nResearch) wants.push_back (kIdResearch);
			}
			sID what;
			cPosition pos (-1, -1);
			for (const sID& w : wants)
			{
				if (w == kIdShipyard || w == kIdDock)
				{
					// на воде у базы: верфь — у центра базы, док — у верфи
					cPosition anchor = defense_.baseCenter();
					if (w == kIdDock)
						for (const auto& b : player->getBuildings())
							if (b->getStaticUnitData().ID == kIdShipyard)
							{
								anchor = b->getPosition();
								break;
							}
					pos = findWaterSite (model, *player, gridWater, reserved, unitsData->getStaticUnitData (w), defense_, navy_.homeSea(), anchor, world_, bl);
				}
				else
					pos = w == kIdMine ? site.pos : findBaseSpot (model, *player, main, unitsData->getStaticUnitData (w), reserved, world_, bl);
				if (pos.x() >= 0)
				{
					what = w;
					break;
				}
			}
			if (pos.x() < 0)
			{
				idleToBase (v); // строить нечего — металл остаётся фабрикам
				continue;
			}
			// турбо: первая фабрика; шаг 13 — учебный центр и жилой блок по тревоге «диверсант»
			const bool turboPlan = (what == kIdFactory && countBuildings (*player, kIdFactory) == 0)
				|| (defense_.commandoAlarm() && (what == kIdTraining || (what == kIdHabitat && totalOf (kIdTraining) <= 1)));
			executePlan (v, makePlan (v, what, pos, turboPlan));
			if (what == kIdShipyard) navy_.noteShipyardSite (pos); // П13: стройку верфи — охранять
			continue;
		}
		executePlan (v, it->second);
		if (planType (it->second) == kIdShipyard) navy_.noteShipyardSite (it->second.pos);
	}

	// 2) пионеры: генератор при нехватке энергии, соединители до отрезанных зданий, склад
	for (const auto& vp : player->getVehicles())
	{
		const cVehicle& v = *vp;
		if (v.getStaticUnitData().canBuild != "SmallBuilding" || v.isDisabled()) continue;
		if (v.isUnitBuildingABuilding())
		{
			if (v.getBuildTurns() == 0 && !v.bandPosition) pushFinishBuild (v);
			continue;
		}
		if (v.isUnitMoving() || v.getMoveJob() != nullptr || v.bandPosition) continue;
		if (evacuate (v)) continue; // под угрозой — не строить, уйти
		if (isStuck (v)) continue;

		auto it = plans_.find (v.getId());
		if (it != plans_.end() && !planValid (it->second))
		{
			plans_.erase (it);
			it = plans_.end();
		}
		if (it != plans_.end())
		{
			executePlan (v, it->second);
			continue;
		}

		// шаг 13, часть 2 (П14): суши у базы нет — малое здание, которое «не жалко», на платформе у берега
		auto onPlatform = [&] (const sID& what) {
			// только на острове (наша суша меньше 600 клеток): у озёр и рек материка суши хватит дальше от базы
			if (defense_.map().landRegionCells >= 600) return false;
			bool needPlatform = false;
			const cPosition q = findPlatformSpot (model, *player, main, unitsData->getStaticUnitData (what), reservedFootprints(), defense_, world_, &needPlatform);
			if (q.x() < 0) return false;
			executePlan (v, makePlan (v, needPlatform ? kIdPlatform : what, q, false));
			extraEvents_.push_back ({{"event", "navy"}, {"level", "operational"}, {"what", "platform"}, {"unitId", v.getId()},
				{"type", unitsData->getStaticUnitData (what).getDefaultName()}, {"pos", {{"x", q.x()}, {"y", q.y()}}},
				{"why", needPlatform ? "no land left at the base: a platform first (only what is not valuable stands on water)" : "build on our platform"}});
			return true;
		};
		// а) энергии не хватает уже на построенное и строящееся — генератор (2 нефти)
		const auto e = energyState();
		const int capNow = e[1] * 6 + e[2];
		if (!energyFeasible (e[0], e[1], e[2], econ.oilFree)
			&& energyFeasible (std::min (e[0], capNow + 1), e[1], e[2] + 1, econ.oilFree))
		{
			const cPosition pos = findBaseSpot (model, *player, main, unitsData->getStaticUnitData (kIdEnergySmall), reservedFootprints(), world_, bl);
			if (pos.x() >= 0)
			{
				executePlan (v, makePlan (v, kIdEnergySmall, pos, false));
				continue;
			}
			if (onPlatform (kIdEnergySmall)) continue;
		}

		// шаг 13 (П4, П5, П9, П18, П22): стройка обороны — радары, кольцо ПВО-пушек, стены вокруг ключевых
		// зданий (ai_defense); по тревоге (видна авиация, выключено своё здание) — раньше соединителей и складов
		auto defenseBuild = [&] () {
			// без тревоги — только из лишнего металла: склад базы заполнен хотя бы на 40% (армия и шахты
			// важнее; опыт: радары на ходу 21 стоили партии на Mushroom)
			if (!defense_.commandoAlarm() && !defense_.airAlarm() && defense_.enemyShipMetal() <= 0 // шаг 13, ч. 2: корабли врага — ракетные пушки сразу
				&& !(econ.metalMaxStored > 0 && econ.metalStored * 2 >= econ.metalMaxStored))
				return false;
			std::vector<std::pair<sID, cPosition>> planned;
			for (const auto& kv : plans_) planned.emplace_back (planType (kv.second), kv.second.pos);
			for (const auto& ov : player->getVehicles())
				if (ov->isUnitBuildingABuilding()) planned.emplace_back (ov->getBuildingType(), ov->getPosition());
			const auto b = defense_.nextSmall (model, *player, world_, planned, reservedFootprints());
			if (!b.valid()) return false;
			executePlan (v, makePlan (v, b.type, b.pos, false));
			++stats_.defenseBuilds;
			extraEvents_.push_back ({{"event", "defense"}, {"level", "operational"}, {"what", "plan"}, {"unitId", v.getId()},
				{"type", unitsData->getStaticUnitData (b.type).getDefaultName()}, {"pos", {{"x", b.pos.x()}, {"y", b.pos.y()}}},
				{"why", b.why}});
			return true;
		};
		const bool defenseUrgent = defense_.commandoAlarm() || defense_.airAlarm()
			|| (defense_.enemyShipMetal() > 0 && defense_.counts().coastGuns < defense_.counts().coastGunsWanted); // шаг 13, ч. 2 (П25)
		if (defenseUrgent && defenseBuild()) continue;

		// б) соединители до своего здания вне главной подбазы (шахта на ресурсе)
		std::vector<cPosition> path;
		for (const auto& b : player->getBuildings())
		{
			if (!b->getStaticData().connectsToBase || b->subBase == main) continue;
			cPosition side;
			const sConnGrid& gg = isNavalYard (b->getStaticUnitData().ID) ? gridWater : grid; // шаг 13, ч. 2: верфь — длинная цепочка
			if (chainTo (gg, b->getPosition(), b->getIsBig(), &side) < 1) continue;
			auto p = connectorPath (gg, side);
			if (!p.empty() && (path.empty() || p.size() < path.size())) path = std::move (p);
		}
		for (const auto& ov : player->getVehicles()) // большое здание строится вдали от базы
		{
			if (!ov->isUnitBuildingABuilding() || !ov->getIsBig()) continue;
			cPosition side;
			const sConnGrid& gg = isNavalYard (ov->getBuildingType()) ? gridWater : grid;
			if (chainTo (gg, ov->getPosition(), true, &side) < 1) continue;
			auto p = connectorPath (gg, side);
			if (!p.empty() && (path.empty() || p.size() < path.size())) path = std::move (p);
		}
		// цепочка под угрозой врага — не тянуть (пионер погибнет на ней)
		if (std::any_of (path.begin(), path.end(), [&] (const cPosition& c) { return cellThreat (world_, c, bl) > 0; }))
			path.clear();
		if (!path.empty())
		{
			// прямой отрезок от первой клетки: BuildPath кладёт соединитель за ходом по прямой
			size_t run = 0;
			if (path.size() > 1)
			{
				const cPosition dir = path[1] - path[0];
				while (run + 1 < path.size() && path[run + 1] - path[run] == dir) ++run;
			}
			const int perConnector = player->getLastUnitData (kIdConnector)->getBuildCost();
			const int need = std::min (v.getStaticUnitData().storageResMax, perConnector * static_cast<int> (run + 1));
			if (v.getStoredResources() < need)
			{
				needMetal (v, std::min (v.getStaticUnitData().storageResMax, perConnector * static_cast<int> (path.size())) - v.getStoredResources());
				continue;
			}
			if (v.getPosition() != path[0])
			{
				pushMoveSafe (v, view, path[0], world_, bl, playerId, "op", "to connector path", builderOrders);
				continue;
			}
			cOrder o;
			o.type = run > 0 ? eOrderType::BuildPath : eOrderType::Build;
			o.playerId = playerId;
			o.unitId = v.getId();
			o.buildingType = {kIdConnector.firstPart, kIdConnector.secondPart};
			o.position = path[0];
			o.position2 = path[run];
			o.buildSpeed = 0;
			o.level = "op";
			o.reason = "connect remote building (" + std::to_string (path.size()) + " connectors)";
			builderOrders.push_back (std::move (o));
			continue;
		}

		// в) склад металла: добыча переливается через край
		if (main != nullptr && econ.metalStored + econ.metalProd - econ.metalNeeded > econ.metalMaxStored
			&& totalOf (kIdStorageMetal) < kMaxStorages)
		{
			const cPosition pos = findBaseSpot (model, *player, main, unitsData->getStaticUnitData (kIdStorageMetal), reservedFootprints(), world_, bl);
			if (pos.x() >= 0)
			{
				executePlan (v, makePlan (v, kIdStorageMetal, pos, false));
				continue;
			}
			if (onPlatform (kIdStorageMetal)) continue;
		}
		// г) шаг 10: хранилище золота — переработке нужен запас, когда шахты дают меньше 5 в ход
		if (main != nullptr && countBuildings (*player, kIdGoldRefinery) >= 1 && totalOf (kIdGoldVault) < 1)
		{
			const cPosition pos = findBaseSpot (model, *player, main, unitsData->getStaticUnitData (kIdGoldVault), reservedFootprints(), world_, bl);
			if (pos.x() >= 0)
			{
				executePlan (v, makePlan (v, kIdGoldVault, pos, false));
				continue;
			}
			if (onPlatform (kIdGoldVault)) continue;
		}
		// д) шаг 13: оборона — когда экономике пионер не нужен
		if (!defenseUrgent && defenseBuild()) continue;
		idleToBase (v);
	}

	// 2½) шаг 9: ремонтник чинит, снабженец пополняет боезапас металлом из своего запаса — у базы
	//     долить до полного, запас меньше трети — ехать к базе (микро его в этот ход не трогает)
	for (const auto& vp : player->getVehicles())
	{
		const cVehicle& v = *vp;
		const eRole vr = roles_.roleOf (v);
		// шаг 13 (П8): минёр тоже берёт металл у базы (мина — единица металла), пока поле не заложено
		if ((vr != eRole::Repair && vr != eRole::Supply && vr != eRole::MineLayer) || !isIdle (v) || main == nullptr) continue;
		if (vr == eRole::MineLayer && defense_.mineField().empty()) continue;
		const int cap = v.getStaticUnitData().storageResMax;
		const int stored = v.getStoredResources();
		if (stored >= cap) continue;
		if (const cBuilding* src = refillSource (*player, main, v))
			refills.push_back ({&v, src, cap - stored});
		else if (stored * 3 < cap)
		{
			const cPosition dock = findBaseDock (map, *player, main, v, view, reservedFootprints(), world_, bl);
			if (dock.x() >= 0 && pushMoveSafe (v, view, dock, world_, bl, playerId, "op", "repairer to base for metal", builderOrders))
				opMoved_.push_back (v.getId());
		}
	}

	// 3) металл хода: сначала стройщикам (под их стройку), остаток — фабрикам
	int budget = econ.free;
	int storedLeft = econ.metalStored;
	int buildersShort = 0; // шаг 9: металла, что стройщикам не досталось в этот ход
	for (const auto& r : refills)
	{
		const int give = std::min ({r.want, budget, storedLeft});
		if (!r.v->getStaticUnitData().canBuild.empty()) buildersShort += r.want - std::max (0, give);
		if (give <= 0) continue;
		cOrder o;
		o.type = eOrderType::Transfer;
		o.playerId = playerId;
		o.unitId = r.src->getId();
		o.unitId2 = r.v->getId();
		o.transferValue = give;
		o.resType = eResourceType::Metal;
		o.level = "op";
		o.reason = "refill constructor";
		out.push_back (std::move (o));
		budget -= give;
		storedLeft -= give;
	}

	// 4) фабрики главной базы: очередь, выезд, запуск, скорость по металлу.
	//    Шаг 9: обе наземные фабрики — большая (строй и поддержка по весам ролей, ai_roles;
	//    конструкторы — до нужного числа) и малая (разведчики, ремонтники, ПВО — по нужде).
	//    В очереди — текущая машина и следующая: выехала одна — фабрика сразу начинает
	//    другую (повтор очереди не нужен); следующую дописываем, когда в очереди одна.
	auto groundFactory = [] (const cBuilding& b) {
		const auto& cb = b.getStaticUnitData().canBuild;
		return cb == "BigGroundVehicle" || cb == "SmallGroundVehicle" || cb == "Human" // шаг 13: учебный центр — пехота
			|| cb == "Ship"   // шаг 13, часть 2: верфь — корабли (ai_navy)
			|| cb == "Plane"; // шаг 13, часть 3: авиазавод — самолёты (ai_air)
	};
	std::vector<cOrder> prodOrders; // после стройки: выехавший юнит не займёт площадку
	// добыча металла упала (движок отдал мощность шахт нефти или золоту) и работающим
	// фабрикам не хватит — остановить лишние сразу, иначе движок откажет в сдаче хода
	std::vector<unsigned int> stopped;
	{
		int deficit = econ.metalNeeded - econ.metalProd - econ.metalStored;
		for (const auto& bp : player->getBuildings())
		{
			if (deficit <= 0) break;
			const cBuilding& b = *bp;
			if (!groundFactory (b) || b.subBase != main || !b.isUnitWorking()) continue;
			cOrder o;
			o.type = eOrderType::StopWork;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.level = "op";
			o.reason = "stop factory (metal income dropped)";
			prodOrders.push_back (std::move (o));
			deficit -= b.getMetalPerRound();
			stopped.push_back (b.getId());
		}
	}
	//    Скорость 0 — танк за 12 металла, 1 — вдвое быстрее, но за 24: быстрая —
	//    только когда металл всё равно копится на складе.
	const bool richNow = econ.metalMaxStored > 0 && storedLeft * 5 >= econ.metalMaxStored * 4;
	int energyStarted = 0; // энергия фабрик, запущенных в этом ходу
	// ускорять (×2 металла за юнит) — только когда ни одна фабрика не стоит без металла
	bool allWorking = true;
	for (const auto& bp : player->getBuildings())
		if (groundFactory (*bp) && bp->subBase == main && !bp->isBuildListEmpty()
			&& bp->getBuildListItem (0).getRemainingMetal() > 0 && !bp->isUnitWorking())
			allWorking = false;
	// шаг 9: стройщикам металла не хватило — фабрики не ускорять, быстрые — сбавить до обычной
	// скорости (Delta, восток: фабрика на скорости 1 съедала весь доход 12, стройщики без
	// металла не ставили шахт — экономика стояла всю партию, «88 машин» шага 8)
	const bool buildersStarved = buildersShort > 0;
	const bool speedUp = richNow && allWorking && !buildersStarved;
	const int nKonstrukt = countVehicles (*player, kIdKonstrukt);
	const int konstruktTarget = econ.metalMaxProd >= 20 ? 3 : 2;
	int konstruktQueued = 0;
	for (const auto& b : player->getBuildings())
		if (groundFactory (*b))
			for (size_t i = 0; i < b->getBuildListSize(); ++i)
				if (b->getBuildListItem (i).getType() == kIdKonstrukt) ++konstruktQueued;
	// следующая машина фабрики: конструктор, пока их меньше нужного (большая фабрика),
	// иначе — по ролям (ai_roles); sID() — малой фабрике сейчас ничего не нужно
	auto nextFor = [&] (const cBuilding& b) -> sID {
		const std::string& kind = b.getStaticUnitData().canBuild;
		// шаг 13, часть 2 (П11–П14): верфь — корабль по программе флота (ai_navy пишет в журнал, из чего выбирал)
		if (kind == "Ship") return navy_.nextShip (model, *player, roles_, comp_, kind);
		// шаг 13, часть 3 (П15–П17, П10): авиазавод — самолёт по программе авиации (ai_air пишет в журнал, из чего выбирал)
		if (kind == "Plane") return air_.nextPlane (model, *player, roles_, comp_, kind);
		// шаг 13 (П7): учебный центр — пехота, пока постов охраны больше, чем пехоты (с очередью); шаг 13, ч. 3 (П2) —
		// коммандос по программе ai_commando (тревога «диверсант» — сначала пехота)
		if (kind == "Human")
		{
			const sID inf = roles_.typeFor (eRole::Guard, kind);
			const sID cmd = roles_.typeFor (eRole::Commando, kind);
			const bool guardsShort = inf != sID() && comp_.count (eRole::Guard) < defense_.guardsWanted();
			if (guardsShort && defense_.commandoAlarm())
			{
				comp_.noteQueued (roles_, inf);
				return inf;
			}
			if (cmd != sID() && comp_.count (eRole::Commando) < commando_.commandosWanted())
			{
				comp_.noteQueued (roles_, cmd);
				extraEvents_.push_back ({{"event", "commando"}, {"level", "operational"}, {"what", "build commando"},
					{"have", comp_.count (eRole::Commando)}, {"want", commando_.commandosWanted()}, {"why", commando_.programWhy()}});
				return cmd;
			}
			if (!guardsShort) return sID();
			comp_.noteQueued (roles_, inf);
			return inf;
		}
		if (unitsData->getStaticUnitData (kIdKonstrukt).buildAs == kind && nKonstrukt + konstruktQueued < konstruktTarget)
		{
			++konstruktQueued;
			return kIdKonstrukt;
		}
		// шаг 13, часть 3 (П2): БМП — коммандос за воду, когда авиатранспорта нет (под водой её видит только корвет)
		if (kind == "SmallGroundVehicle" && commando_.apcWanted() > comp_.count (eRole::Apc))
			if (const sID apc = roles_.typeFor (eRole::Apc, kind); apc != sID())
			{
				comp_.noteQueued (roles_, apc);
				extraEvents_.push_back ({{"event", "commando"}, {"level", "operational"}, {"what", "build apc"},
					{"why", "commando target beyond water, no air transport: APC under water"}});
				return apc;
			}
		const sID t = kind == "SmallGroundVehicle" ? comp_.nextSmall (roles_, kind) : comp_.nextBig (roles_, kind);
		if (t != sID()) comp_.noteQueued (roles_, t);
		return t;
	};
	auto listOf = [] (const cBuilding& b) {
		std::vector<sTypeId> l;
		for (size_t i = 0; i < b.getBuildListSize(); ++i)
			l.push_back ({b.getBuildListItem (i).getType().firstPart, b.getBuildListItem (i).getType().secondPart});
		return l;
	};
	auto produceReason = [&] (const std::vector<sTypeId>& l) {
		// «produce constructor» — по нему советник узнаёт заказ конструктора (ai_llm_directives)
		for (const auto& t : l)
			if (sID (t.firstPart, t.secondPart) == kIdKonstrukt) return std::string ("produce constructor");
		std::string r = "produce";
		for (const auto& t : l)
			r += std::string (" ") + roleName (roles_.roleOfType (sID (t.firstPart, t.secondPart)));
		return r;
	};

	for (const auto& bp : player->getBuildings())
	{
		const cBuilding& b = *bp;
		if (!groundFactory (b) || b.isDisabled()) continue;
		if (main == nullptr || b.subBase != main) continue; // вне главной базы металла нет
		if (std::find (stopped.begin(), stopped.end(), b.getId()) != stopped.end()) continue;

		if (b.isBuildListEmpty())
		{
			const sID first = nextFor (b);
			if (first == sID()) continue; // малой фабрике ничего не нужно — стоит без металла
			const sID second = nextFor (b);
			// ПВО по угрозе с воздуха — срочно: быстрая скорость, если металл хода есть
			const bool urgent = roles_.roleOfType (first) == eRole::AntiAir
				|| (roles_.roleOfType (first) == eRole::Guard && defense_.commandoAlarm()) // шаг 13: диверсант — охрана срочно
				|| (roles_.roleOfType (first) == eRole::Corvette && navy_.counts().corvettes == 0) // шаг 13, ч. 2: первый корвет — охрана верфи
				|| ((roles_.roleOfType (first) == eRole::Fighter || roles_.roleOfType (first) == eRole::Bomber) && air_.urgent()); // шаг 13, ч. 3
			const int speed = ((speedUp || urgent) && budget >= factoryMetalPerRound (b, 1)) ? 1 : 0;
			cOrder o;
			o.type = eOrderType::ChangeBuildList;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.buildSpeed = speed;
			o.buildList = {{first.firstPart, first.secondPart}};
			if (second != sID()) o.buildList.push_back ({second.firstPart, second.secondPart});
			o.repeat = false;
			o.reason = produceReason (o.buildList);
			o.level = "op";
			prodOrders.push_back (std::move (o));
			const int mpr = factoryMetalPerRound (b, speed);
			if (budget >= mpr)
			{
				budget -= mpr; // ChangeBuildList сам запускает фабрику
				energyStarted += b.getStaticUnitData().needsEnergy;
			}
			continue;
		}

		if (b.getBuildListItem (0).getRemainingMetal() <= 0)
		{
			// готовый юнит — выезд; есть следующая в очереди (или повтор) — движок сразу начнёт её
			const auto& vehicleData = unitsData->getStaticUnitData (b.getBuildListItem (0).getType());
			const tFootprints reservedNow = reservedFootprints();
			cPosition esc = findFactoryEscape (model, b, vehicleData, *player, &reservedNow);
			if (esc.x() < 0) esc = findFactoryEscape (model, b, vehicleData, *player);
			if (esc.x() < 0)
			{
				// выезд заставлен своими машинами без дела — отогнать их
				for (const auto& c : ringCells (b.getPosition(), b.getIsBig()))
					if (map.isValidPosition (c))
						if (const cVehicle* u = map.getField (c).getVehicle())
							shooAway (*u, b.getPosition(), "clear factory exit");
				continue;
			}
			cOrder o;
			o.type = eOrderType::FinishBuild;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.position = esc;
			o.level = "op";
			o.reason = "roll out vehicle";
			prodOrders.push_back (std::move (o));
			const int mpr = factoryMetalPerRound (b, b.getBuildSpeed());
			if ((b.getRepeatBuild() || b.getBuildListSize() >= 2) && budget >= mpr)
			{
				budget -= mpr;
				energyStarted += b.getStaticUnitData().needsEnergy;
			}
			continue;
		}

		if (b.isUnitWorking())
		{
			// в очереди одна машина (и не повтор советника) — дописать следующую, чтобы фабрика не встала
			std::vector<sTypeId> list = listOf (b);
			bool changed = false;
			if (list.size() == 1 && !b.getRepeatBuild())
				if (const sID next = nextFor (b); next != sID())
				{
					list.push_back ({next.firstPart, next.secondPart});
					changed = true;
				}
			// металл копится — ускорить работающую фабрику
			int speed = b.getBuildSpeed();
			if (speedUp && b.getBuildSpeed() == 0)
			{
				const int extra = factoryMetalPerRound (b, 1) - b.getMetalPerRound();
				if (budget >= extra)
				{
					speed = 1;
					budget -= extra;
				}
			}
			// шаг 9: стройщики без металла — быструю фабрику сбавить (задел сохраняется)
			bool slowForBuilders = false;
			if (buildersStarved && b.getBuildSpeed() > 0)
			{
				speed = 0;
				slowForBuilders = true;
			}
			if (changed || speed != b.getBuildSpeed())
			{
				cOrder o;
				o.type = eOrderType::ChangeBuildList;
				o.playerId = playerId;
				o.unitId = b.getId();
				o.buildList = std::move (list);
				o.buildSpeed = speed;
				o.repeat = b.getRepeatBuild();
				o.level = "op";
				o.reason = slowForBuilders ? "slow down factory (builders need metal)"
					: (speed != b.getBuildSpeed() ? "speed up factory (metal piles up)" : produceReason (o.buildList));
				prodOrders.push_back (std::move (o));
			}
			continue;
		}

		// стоит с недостроенным заданием (не хватило металла или энергии)
		const int mpr = b.getMetalPerRound();
		const bool energyOk = energyFeasible (econ.energyNeedWorking + energyStarted + b.getStaticUnitData().needsEnergy,
			econ.stations, econ.generators, econ.oil);
		if (b.buildingCanBeStarted() && budget >= mpr && energyOk)
		{
			cOrder o;
			o.type = eOrderType::StartWork;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.level = "op";
			o.reason = "start factory";
			prodOrders.push_back (std::move (o));
			budget -= mpr;
			energyStarted += b.getStaticUnitData().needsEnergy;
		}
		else if (b.getBuildSpeed() > 0)
		{
			// не тянем быструю скорость — перейти на дешёвую, задел сохраняется
			cOrder o;
			o.type = eOrderType::ChangeBuildList;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.buildList = listOf (b);
			o.buildSpeed = 0;
			o.repeat = b.getRepeatBuild();
			o.level = "op";
			o.reason = "slow down factory (metal short)";
			prodOrders.push_back (std::move (o));
			const int mpr0 = factoryMetalPerRound (b, 0);
			if (budget >= mpr0) budget -= mpr0;
		}
	}

	// 5) площадки планов заняты своими машинами без дела — отогнать (иначе стройка ждёт вечно)
	{
		const tFootprints reserved = reservedFootprints();
		std::vector<unsigned int> busy; // уже получили приказ стройщика
		for (const auto& o : builderOrders) busy.push_back (o.unitId);
		for (const auto& kv : plans_)
		{
			const bool big = unitsData->getStaticUnitData (planType (kv.second)).buildingData.isBig;
			for (const auto& c : footprintCells (kv.second.pos, big))
			{
				if (!map.isValidPosition (c)) continue;
				const cVehicle* u = map.getField (c).getVehicle();
				if (u == nullptr || u->getOwner() != player || u->getId() == kv.first || !isIdle (*u)) continue;
				if (std::find (busy.begin(), busy.end(), u->getId()) != busy.end()) continue;
				cPosition dest (-1, -1); // ближайшая свободная клетка вне площадок
				for (int r = 1; r <= 4 && dest.x() < 0; ++r)
					for (int dy = -r; dy <= r && dest.x() < 0; ++dy)
						for (int dx = -r; dx <= r && dest.x() < 0; ++dx)
						{
							if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
							const cPosition n = c + cPosition (dx, dy);
							if (map.isValidPosition (n) && view.possiblePlace (*u, n) && !overlapsAny (n, false, reserved)) dest = n;
						}
				if (dest.x() >= 0 && pushMove (*u, view, dest, playerId, "op", "clear build site", builderOrders))
				{
					busy.push_back (u->getId());
					opMoved_.push_back (u->getId());
				}
			}
		}
	}

	// 6) стройка и движение стройщиков, затем производство
	for (auto& o : builderOrders)
		out.push_back (std::move (o));
	for (auto& o : prodOrders)
		out.push_back (std::move (o));

	// 7) шахты и исследования главной базы, что стоят, — запустить, если хватит энергии
	//    (генераторы не трогаем: движок сам включает их под потребителей)
	if (main != nullptr)
	{
		int energyNeed = econ.energyNeedWorking + energyStarted;
		int humanFree = econ.humanFree;
		// шаг 10: золото для переработки — добыча и запас сверх того, что уже перерабатывается
		int goldFree = main->getProd().gold + main->getResourcesStored().gold - main->getResourcesNeeded().gold;
		// золота на работающие переработки к концу хода не хватит — остановить их сразу, иначе движок
		// остановит сам и откажет в сдаче хода (как с металлом у фабрик)
		for (const auto& bp : player->getBuildings())
		{
			if (goldFree >= 0) break;
			const cBuilding& b = *bp;
			if (b.subBase != main || !b.isUnitWorking() || b.getStaticData().convertsGold <= 0) continue;
			cOrder o;
			o.type = eOrderType::StopWork;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.level = "op";
			o.reason = "stop gold refinery (gold short)";
			out.push_back (std::move (o));
			goldFree += b.getStaticData().convertsGold;
			energyNeed -= b.getStaticUnitData().needsEnergy;
		}
		for (const auto& bp : player->getBuildings())
		{
			const cBuilding& b = *bp;
			if (b.subBase != main || b.isDisabled() || b.isUnitWorking() || !b.buildingCanBeStarted()) continue;
			const auto& sd = b.getStaticUnitData();
			const bool isMine = sd.buildingData.canMineMaxRes > 0;
			const bool isResearch = sd.buildingData.canResearch;
			const bool isRefinery = sd.buildingData.convertsGold > 0;
			if (!isMine && !isResearch && !isRefinery) continue;
			if (isRefinery && goldFree < sd.buildingData.convertsGold) continue;
			if (sd.needsHumans > humanFree) continue;
			if (!energyFeasible (energyNeed + sd.needsEnergy, econ.stations, econ.generators, econ.oil)) continue;
			cOrder o;
			o.type = eOrderType::StartWork;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.level = "op";
			o.reason = isMine ? "start mine" : (isRefinery ? "start gold refinery" : "start research");
			out.push_back (std::move (o));
			energyNeed += sd.needsEnergy;
			humanFree -= sd.needsHumans;
			if (isRefinery) goldFree -= sd.buildingData.convertsGold;
		}
	}

	// 8) шаг 10: обновление отставших машин в депо — улучшения и исследования действуют только на
	//    новые машины; стоящие обновляются в хранилище за четверть цены металлом подбазы.
	//    Ход 1: машина подъезжает к депо; ход 2: депо грузит её (въезд — после сдачи хода);
	//    ход 3: обновить и выпустить. В депо — только бойцы без дела, не в походе операции.
	for (auto it = refit_.begin(); it != refit_.end();)
	{
		const cVehicle* v = model.getVehicleFromID (it->first);
		if (v == nullptr || v->getOwner() != player || turn - it->second > 6) it = refit_.erase (it);
		else ++it;
	}
	// 8¾) шаг 10: здания тоже отстают от текущей версии (исследования брони и прочности действуют только
	//     на новые) — обновить на месте, когда металл всё равно копится (склад ≥ 80%): сначала
	//     вооружённые, потом шахты, фабрики и энергия; за ход — один тип, все такие здания подбазы
	if (main != nullptr && econ.metalMaxStored > 0 && storedLeft * 5 >= econ.metalMaxStored * 4)
	{
		const cBuilding* pick = nullptr;
		int pickRank = 0;
		for (const auto& bp : player->getBuildings())
		{
			const cBuilding& b = *bp;
			if (b.subBase != main || b.isDisabled() || !outdated (b)) continue;
			const cDynamicUnitData* last = player->getLastUnitData (b.data.getId());
			if (last == nullptr) continue;
			// ради брони, прочности, урона и дальности (цена от исследования «стоимость» — не повод)
			const int gap = (last->getArmor() - b.data.getArmor()) + (last->getHitpointsMax() - b.data.getHitpointsMax()) / 8
				+ (last->getDamage() - b.data.getDamage()) + (last->getRange() - b.data.getRange());
			if (gap <= 0) continue;
			const auto& bsd = b.getStaticUnitData();
			const int rank = bsd.canAttack != 0 ? 3 : ((bsd.buildingData.canMineMaxRes > 0 || !bsd.canBuild.empty() || bsd.produceEnergy > 0) ? 2 : 0);
			if (rank == 0) continue; // соединители, склады, жилые — не стоят металла
			if (rank > pickRank)
			{
				pickRank = rank;
				pick = &b;
			}
		}
		if (pick != nullptr)
		{
			int n = 0;
			for (const cBuilding* b : main->getBuildings())
				if (b->data.getId() == pick->data.getId() && outdated (*b)) ++n;
			const int cost = refitMetalCost (*player, pick->data.getId());
			const int afford = std::min (budget, storedLeft) / std::max (1, cost);
			if (afford >= 1)
			{
				cOrder o;
				o.type = eOrderType::UpgradeBuilding;
				o.playerId = playerId;
				o.unitId = pick->getId();
				o.allUnits = afford >= n;
				o.level = "op";
				o.reason = std::string ("upgrade buildings: ") + pick->getStaticUnitData().getDefaultName() + (o.allUnits ? " (all)" : "");
				out.push_back (std::move (o));
				const int spent = cost * (o.allUnits ? n : 1);
				budget -= spent;
				storedLeft -= spent;
				++stats_.refits;
			}
		}
	}

	const cBuilding* depot = nullptr;
	if (main != nullptr)
		for (const auto& b : player->getBuildings())
			if (b->getStaticUnitData().ID == kIdDepot && b->subBase == main && !b->isDisabled())
			{
				depot = b.get();
				break;
			}
	// шаг 13 (П4): мобильный радар прячется в депо от разведки врага; база ослепла (радаров нет или все
	// выключены, а нужны) — выходит и встаёт у центра базы; радары снова видят — обратно в депо
	if (depot != nullptr)
	{
		const bool blind = defense_.counts().radarsWanted > 0 && defense_.counts().radarsWorking == 0;
		for (const auto& vp : player->getVehicles())
		{
			const cVehicle& v = *vp;
			if (roles_.roleOf (v) != eRole::Scanner || v.isDisabled()) continue;
			if (v.isUnitLoaded())
			{
				if (!blind || std::find (depot->storedUnits.begin(), depot->storedUnits.end(), &v) == depot->storedUnits.end()) continue;
				for (const auto& c : ringCells (depot->getPosition(), depot->getIsBig()))
				{
					if (!map.isValidPosition (c) || !depot->canExitTo (c, map, v.getStaticUnitData()) || map.isCoast (c)) continue;
					cOrder o;
					o.type = eOrderType::ActivateUnit;
					o.playerId = playerId;
					o.unitId = depot->getId();
					o.unitId2 = v.getId();
					o.position = c;
					o.level = "op";
					o.reason = "scanner: base is blind, leave depot";
					out.push_back (std::move (o));
					break;
				}
				continue;
			}
			if (!isIdle (v)) continue;
			if (blind)
			{
				// у центра базы — радар на колёсах
				const cPosition c = defense_.baseCenter();
				if (std::max (std::abs (v.getPosition().x() - c.x()), std::abs (v.getPosition().y() - c.y())) > 3)
				{
					cPosition dest;
					if (world_.nearestSafeCell (v, view, c, 4, c, dest, eThreatLayer::Fire)
						&& pushMoveSafe (v, view, dest, world_, eThreatLayer::Fire, playerId, "op", "scanner: base is blind, to the centre", out))
						opMoved_.push_back (v.getId());
				}
				continue;
			}
			if (depot->isNextTo (v.getPosition()))
			{
				if (!depot->canLoad (&v)) continue;
				cOrder o;
				o.type = eOrderType::Load;
				o.playerId = playerId;
				o.unitId = depot->getId();
				o.unitId2 = v.getId();
				o.level = "op";
				o.reason = "scanner: hide in depot";
				out.push_back (std::move (o));
				opMoved_.push_back (v.getId());
				continue;
			}
			const tFootprints reservedNow = reservedFootprints();
			cPosition best (-1, -1);
			long long bestD = std::numeric_limits<long long>::max();
			for (const auto& rc : ringCells (depot->getPosition(), depot->getIsBig()))
			{
				if (!map.isValidPosition (rc) || !view.possiblePlace (v, rc) || overlapsAny (rc, false, reservedNow)) continue;
				const long long dd = (rc - v.getPosition()).l2NormSquared();
				if (dd < bestD)
				{
					bestD = dd;
					best = rc;
				}
			}
			if (best.x() >= 0 && pushMove (v, view, best, playerId, "op", "scanner: to depot", out)) opMoved_.push_back (v.getId());
		}
	}
	if (depot != nullptr)
	{
		// а) внутри: обновить, пока хватает металла хода (и на складе), и выпустить в свободные клетки рядом
		int refitMetal = std::min (budget, storedLeft);
		std::vector<cPosition> exits;
		for (const cVehicle* v : depot->storedUnits)
		{
			if (roles_.roleOf (*v) == eRole::Scanner) continue; // шаг 13: мобильный радар сидит в депо — выпускает блок выше
			const auto rit = refit_.find (v->getId());
			const bool overdue = rit == refit_.end() || turn - rit->second > 4; // ждать металла — не дольше
			if (outdated (*v))
			{
				const int c = refitMetalCost (*player, v->data.getId());
				if (c > refitMetal && !overdue) continue; // металла хода нет — подождать внутри
				cOrder o;
				o.type = eOrderType::UpgradeVehicle;
				o.playerId = playerId;
				o.unitId = depot->getId();
				o.unitId2 = v->getId();
				o.level = "op";
				o.reason = "refit vehicle in depot";
				if (c <= refitMetal)
				{
					out.push_back (std::move (o));
					refitMetal -= c;
					budget -= c;
					++stats_.refits;
				}
			}
			for (const auto& c : ringCells (depot->getPosition(), depot->getIsBig()))
			{
				if (!map.isValidPosition (c) || std::find (exits.begin(), exits.end(), c) != exits.end()) continue;
				if (!depot->canExitTo (c, map, v->getStaticUnitData()) || map.isCoast (c)) continue;
				exits.push_back (c);
				cOrder o;
				o.type = eOrderType::ActivateUnit;
				o.playerId = playerId;
				o.unitId = depot->getId();
				o.unitId2 = v->getId();
				o.position = c;
				o.level = "op";
				o.reason = "leave depot";
				out.push_back (std::move (o));
				refit_.erase (v->getId());
				break;
			}
		}
		// б) кандидаты: отставшие бойцы без дела вне похода, ближе refitReach к депо, депо не под угрозой
		int room = depot->getStaticUnitData().storageUnitsMax - static_cast<int> (depot->storedUnits.size());
		for (const auto& kv : refit_)
			if (const cVehicle* v = model.getVehicleFromID (kv.first); v != nullptr && !v->isUnitLoaded()) --room;
		const bool depotSafe = cellThreat (world_, depot->getPosition(), eThreatLayer::Strike) == 0;
		std::vector<std::tuple<int, unsigned int, const cVehicle*>> cands;
		for (const auto& vp : player->getVehicles())
		{
			const cVehicle& v = *vp;
			if (!isIdle (v) || !isArmyRole (roles_.roleOf (v)) || ops_.committed (v.getId())) continue;
			if (std::find (opMoved_.begin(), opMoved_.end(), v.getId()) != opMoved_.end()) continue;
			const bool chosen = refit_.count (v.getId()) != 0;
			if (!chosen && (refitGap (v) < kRefitGap || !depotSafe)) continue;
			const int d = std::max (std::abs (v.getPosition().x() - depot->getPosition().x()), std::abs (v.getPosition().y() - depot->getPosition().y()));
			if (!chosen && d > kRefitReach) continue;
			if (world_.groundFire (v.getPosition()) > 0 || v.isBeingAttacked()) continue; // в бою — не до депо
			cands.emplace_back (chosen ? -1 : d, v.getId(), &v);
		}
		std::sort (cands.begin(), cands.end(), [] (const auto& a, const auto& b) {
			return std::get<0> (a) != std::get<0> (b) ? std::get<0> (a) < std::get<0> (b) : std::get<1> (a) < std::get<1> (b);
		});
		const tFootprints reserved = reservedFootprints();
		for (const auto& c : cands)
		{
			const cVehicle& v = *std::get<2> (c);
			const bool chosen = refit_.count (v.getId()) != 0;
			if (!chosen && room <= 0) continue;
			if (depot->isNextTo (v.getPosition()))
			{
				if (!depot->canLoad (&v)) continue;
				cOrder o;
				o.type = eOrderType::Load;
				o.playerId = playerId;
				o.unitId = depot->getId();
				o.unitId2 = v.getId();
				o.level = "op";
				o.reason = "into depot for refit";
				out.push_back (std::move (o));
				++stats_.refitLoads;
			}
			else
			{
				// ближайшая к машине свободная клетка у депо
				cPosition best (-1, -1);
				long long bestD = std::numeric_limits<long long>::max();
				for (const auto& rc : ringCells (depot->getPosition(), depot->getIsBig()))
				{
					if (!map.isValidPosition (rc) || !view.possiblePlace (v, rc) || overlapsAny (rc, false, reserved)) continue;
					const long long dd = (rc - v.getPosition()).l2NormSquared();
					if (dd < bestD)
					{
						bestD = dd;
						best = rc;
					}
				}
				if (best.x() < 0 || !pushMove (v, view, best, playerId, "op", "to depot for refit", out)) continue;
			}
			if (!chosen)
			{
				refit_[v.getId()] = turn;
				--room;
			}
			opMoved_.push_back (v.getId()); // микро и операции эту машину в этот ход не трогают
		}
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

	// шаг 13 (П8): закладка мин осталась включённой (второго захода не было) — выключить
	for (const auto& v : player->getVehicles())
		if (v->isUnitLayingMines() && !v->isUnitMoving() && v->getMoveJob() == nullptr)
		{
			cOrder o;
			o.type = eOrderType::ToggleLayMines;
			o.playerId = playerId;
			o.unitId = v->getId();
			o.level = "micro";
			o.reason = "minelayer: stop laying";
			out.push_back (std::move (o));
		}

	auto opMovedId = [&] (unsigned int id) { return std::find (opMoved_.begin(), opMoved_.end(), id) != opMoved_.end(); };

	// --- шаг 9: поддержка в зоне огня врага — уйти туда, где враг не достанет, и бить оттуда ---
	// (стоя в его дальности, она отдаёт ему два выстрела за один свой; уйти некуда — стреляет)
	std::map<unsigned int, sStandoff> kite;
	{
		std::vector<cPosition> kiteReserved;
		for (const auto& vehicle : player->getVehicles())
		{
			const cVehicle& v = *vehicle;
			if (!isIdle (v) || !isCombatUnit (v) || !isSupportRole (roles_.roleOf (v)) || opMovedId (v.getId())) continue;
			if (v.isBeingAttacked() || v.isAttacking()) continue; // связана выстрелом — не двинется
			if (world_.groundFire (v.getPosition()) == 0) continue;
			sStandoff s = findStandoff (model, view, world_, v, attackableTargets (model, world_, v), v.data.getRange(),
				true, supportMaxDanger (roles_.roleOf (v)), kiteReserved);
			if (s.path.empty() || s.danger >= 2) continue; // уйти некуда — стрелять с места
			kiteReserved.push_back (s.path.back());
			kite[v.getId()] = std::move (s);
		}
	}

	// --- собрать боевые юниты и скоординировать фокус-огонь на уровне игрока ---
	// (шаг 9: разведчики не стреляют — разведка; уходящая из-под огня поддержка — тоже)
	std::vector<const cVehicle*> attackers;
	for (const auto& vehicle : player->getVehicles())
		if (isIdle (*vehicle) && isCombatUnit (*vehicle) && roles_.roleOf (*vehicle) != eRole::Scout && kite.count (vehicle->getId()) == 0
			&& roles_.roleOf (*vehicle) != eRole::Submarine // шаг 13, ч. 2: выстрел раскрывает подлодку — бьёт только корабли (флот)
			&& !isAirRole (roles_.roleOf (*vehicle)) && roles_.roleOf (*vehicle) != eRole::Commando) // шаг 13, ч. 3: самолёты и коммандос — свои модули
			attackers.push_back (vehicle.get());
	const std::vector<int> focus = coordinateFocusFire (attackers, enemies, view, map.get());

	// --- операции армии (шаг 8): состав, роли, цели, силы, переходы — до тревоги ---
	// шаг 9: в операциях — строй и поддержка; разведчики — отдельно (разведка, корректировка)
	cArmyOps::sContext opCtx;
	opCtx.model = &model;
	opCtx.player = player;
	opCtx.view = &view;
	opCtx.world = &world_;
	opCtx.turn = world_.turn();
	opCtx.roles = &roles_;
	// шаг 13, часть 3 (П25): враг бьёт дальше, чем видит скаут (ракетная пушка 11 против обзора 9), а AWAC есть — разведку у
	// зданий врага ведёт AWAC (и радары), скауты туда не ходят (их бьют из тумана)
	std::vector<cPosition> scoutAvoid;
	if (air_.counts().awacs > 0)
	{
		int scoutScan = 9;
		if (const sID sc = roles_.typeFor (eRole::Scout, "SmallGroundVehicle"); sc != sID())
			if (const auto* d = player->getLastUnitData (sc)) scoutScan = d->getScan();
		int longest = 0;
		for (const auto& kv : world_.enemies())
			if (kv.second.building && kv.second.hitsGround()) longest = std::max (longest, kv.second.range);
		if (longest > scoutScan)
		{
			for (const auto& kv : world_.enemies())
				if (kv.second.building && kv.second.owner >= 0) scoutAvoid.push_back (kv.second.pos);
			opCtx.scoutAvoid = &scoutAvoid;
			opCtx.scoutAvoidR = longest + 2;
		}
	}
	// шаг 13, часть 2: груз флота (к транспорту и в нём) и высаженные на чужую сушу — не армия своей суши
	const int homeRegion = defense_.map().landRegion;
	// на чужой суше: область танка клетки есть и не наша (амфибия на берегу или воде — области нет, она дома: Donuts —
	// разведчик на берегу выпадал из разведки операций, и партии расходились с base3)
	auto offHome = [&] (const cVehicle& v) {
		const int r = world_.tankRegionOf (v.getPosition());
		return homeRegion >= 0 && r >= 0 && r != homeRegion;
	};
	for (const auto& v : player->getVehicles())
	{
		if (!isGroundFighter (*v)) continue;
		if (navy_.claimed (v->getId()) || offHome (*v)) continue;
		const eRole r = roles_.roleOf (*v);
		if (isArmyRole (r)) opCtx.fighters.push_back (v.get());
		else if (r == eRole::Scout) opCtx.scoutUnits.push_back (v.get());
	}
	ops_.refresh (opCtx);

	// --- тревога у базы (шаг 7): вооружённый враг у ключевого здания или стройки ---
	// Ответ — ближайшими свободными бойцами, пока их ожидаемый урон за ход не
	// покроет hp группы врагов с запасом; остальная армия занята своим. Защитники
	// и резерв подходят, как и все, только до края зоны огня врага (§4 отчёта
	// шага 7: полное сближение отдавало первый выстрел врагу и стоило 0.07 D).
	std::map<unsigned int, cPosition> defend; // защитник → клетка врага, к которому идти
	std::vector<unsigned int> covered;        // враги, на которых сил хватило: прочим к ним не идти
	std::vector<cPosition> alarmTargets;      // клетки всех врагов тревоги — для резерва без дела
	{
		// ключевые клетки: шахты, фабрики, электростанции, центры исследований, идущая стройка
		std::vector<cPosition> keys;
		for (const auto& b : player->getBuildings())
		{
			const auto& bsd = b->getStaticUnitData();
			if (bsd.buildingData.canMineMaxRes > 0 || !bsd.canBuild.empty() || bsd.produceEnergy > 0 || bsd.buildingData.canResearch)
				for (const auto& c : footprintCells (b->getPosition(), b->getIsBig()))
					keys.push_back (c);
		}
		for (const auto& v : player->getVehicles())
			if (v->isUnitBuildingABuilding())
				for (const auto& c : footprintCells (v->getPosition(), v->getIsBig()))
					keys.push_back (c);

		// враги у базы: вооружены; машины — видны сейчас или недавно (память)
		std::vector<std::vector<const sEnemyMemory*>> groups;
		for (const auto& kv : world_.enemies())
		{
			const sEnemyMemory& m = kv.second;
			if (!m.armed()) continue;
			if (!m.building && world_.age (m) > kAlarmFresh) continue;
			long long dOur = std::numeric_limits<long long>::max();
			for (const auto& k : keys)
				dOur = std::min<long long> (dOur, (k - m.pos).l2NormSquared());
			if (dOur > static_cast<long long> (kAlarmRadius) * kAlarmRadius) continue;
			// враг ближе к своим зданиям, чем к нашим (базы рядом — Donuts), — не налёт на нас
			long long dTheir = std::numeric_limits<long long>::max();
			for (const auto& kv2 : world_.enemies())
				if (kv2.second.building && kv2.second.owner == m.owner && kv2.first != m.id)
					dTheir = std::min<long long> (dTheir, (kv2.second.pos - m.pos).l2NormSquared());
			if (dTheir <= dOur) continue;
			// группа — враги ближе kAlarmGroup друг к другу (порядок по id — детерминированно)
			bool joined = false;
			for (auto& g : groups)
				if (std::any_of (g.begin(), g.end(), [&] (const sEnemyMemory* o) {
						return (o->pos - m.pos).l2NormSquared() <= kAlarmGroup * kAlarmGroup;
					}))
				{
					g.push_back (&m);
					joined = true;
					break;
				}
			if (!joined) groups.push_back ({&m});
		}

		// свободные бойцы: без цели огня в этом ходу, не отходят, не отогнаны оперативкой
		std::vector<const cVehicle*> idleFighters;
		for (size_t a = 0; a < attackers.size(); ++a)
		{
			const cVehicle* v = attackers[a];
			if (focus[a] >= 0) continue;
			if (std::find (opMoved_.begin(), opMoved_.end(), v->getId()) != opMoved_.end()) continue;
			if (shouldRetreat (*v, enemies)) continue;
			if (ops_.committed (v->getId())) continue; // в походе операции — не отзывать
			if (navy_.claimed (v->getId()) || offHome (*v)) continue; // шаг 13, ч. 2: груз флота, высаженные
			if (!isArmyRole (roles_.roleOf (*v))) continue; // шаг 9: ПВО держится у армии и базы, не защитник
			idleFighters.push_back (v);
		}
		std::vector<bool> taken (idleFighters.size(), false);
		for (const auto& g : groups)
		{
			long long hp = 0;
			int armor = 0;
			int flag = 0; // по каким поверхностям надо бить: воздух 1, море 2, суша 4
			for (const sEnemyMemory* m : g)
			{
				hp += m->hp;
				armor = std::max (armor, m->armor);
				flag |= m->plane ? 1 : (m->ship ? 2 : 4);
			}
			const long long need = hp * kAlarmMarginNum / kAlarmMarginDen;
			// кандидаты — по близости к группе, при равенстве — по id
			std::vector<std::tuple<long long, unsigned int, size_t>> cands;
			for (size_t i = 0; i < idleFighters.size(); ++i)
			{
				if (taken[i] || (idleFighters[i]->getStaticUnitData().canAttack & flag) == 0) continue;
				long long d = std::numeric_limits<long long>::max();
				for (const sEnemyMemory* m : g)
					d = std::min<long long> (d, (idleFighters[i]->getPosition() - m->pos).l2NormSquared());
				if (d > kAlarmMaxDist * kAlarmMaxDist) continue;
				cands.emplace_back (d, idleFighters[i]->getId(), i);
			}
			std::sort (cands.begin(), cands.end());
			long long have = 0;
			for (const auto& c : cands)
			{
				if (have >= need) break;
				const size_t i = std::get<2> (c);
				const cVehicle* v = idleFighters[i];
				taken[i] = true;
				have += static_cast<long long> (effectiveDamagePerShot (v->data.getDamage(), armor)) * v->data.getShotsMax();
				// идти к ближайшему врагу группы
				const sEnemyMemory* nearest = g.front();
				for (const sEnemyMemory* m : g)
					if ((v->getPosition() - m->pos).l2NormSquared() < (v->getPosition() - nearest->pos).l2NormSquared()) nearest = m;
				defend[v->getId()] = nearest->pos;
				++stats_.defenders;
			}
			++stats_.alarms;
			stats_.intruders += static_cast<int> (g.size());
			for (const sEnemyMemory* m : g)
				alarmTargets.push_back (m->pos);
			if (have >= need)
				for (const sEnemyMemory* m : g)
					covered.push_back (m->id);
			else
				++stats_.uncovered;
		}
	}
	// шаг 13 (П7): тревога «диверсант» — до 2 свободных бойцов обходят клетки вокруг выключенного здания.
	// Въезд в клетку скрытого коммандос сдвигает его на соседнюю, а сдвинуться некуда или нечем (ход
	// истрачен) — он обнаружен (cModel::sideStepStealthUnit); обнаруженного убивают в этом же ходу (заходы 2+)
	std::map<unsigned int, cPosition> sweep;
	if (const auto site = defense_.alarmSite (model, *player); site.first.x() >= 0)
	{
		std::vector<cPosition> around = ringCells (site.first, site.second);
		std::vector<std::tuple<long long, unsigned int, const cVehicle*>> cands;
		for (size_t a = 0; a < attackers.size(); ++a)
		{
			const cVehicle* v = attackers[a];
			if (focus[a] >= 0 || !isArmyRole (roles_.roleOf (*v)) || ops_.committed (v->getId()) || defend.count (v->getId())) continue;
			if (opMovedId (v->getId()) || shouldRetreat (*v, enemies)) continue;
			if (navy_.claimed (v->getId()) || offHome (*v)) continue; // шаг 13, ч. 2
			const long long d2 = (v->getPosition() - site.first).l2NormSquared();
			if (d2 > 25LL * 25) continue;
			cands.emplace_back (d2, v->getId(), v);
		}
		std::sort (cands.begin(), cands.end(), [] (const auto& a, const auto& b) {
			return std::get<0> (a) != std::get<0> (b) ? std::get<0> (a) < std::get<0> (b) : std::get<1> (a) < std::get<1> (b);
		});
		const int turnNow = world_.turn();
		for (size_t k = 0; k < cands.size() && k < 2 && !around.empty(); ++k)
		{
			const cVehicle* v = std::get<2> (cands[k]);
			// клетка обхода — по кругу с ходом (каждый ход — другая), не та, где стоит, проходимая на вид
			for (size_t t = 0; t < around.size(); ++t)
			{
				const cPosition c = around[(static_cast<size_t> (turnNow) * 3 + k * around.size() / 2 + t) % around.size()];
				if (c == v->getPosition() || !view.isValidPosition (c) || !view.possiblePlace (*v, c)) continue;
				sweep[v->getId()] = c;
				break;
			}
		}
	}
	// шаг 13, часть 2 (П11–П14): флот — корабли, док, высадка (груз — свободные бойцы своей суши не в походе);
	// охрана стройки верфи (П13) — 2 свободных бойца у конструктора
	std::vector<unsigned int> shipGuards;
	{
		std::vector<unsigned int> navyBusy;
		for (size_t a = 0; a < attackers.size(); ++a)
			if (focus[a] >= 0) navyBusy.push_back (attackers[a]->getId());
		for (const auto& kv : kite) navyBusy.push_back (kv.first);
		for (const auto& kv : sweep) navyBusy.push_back (kv.first);
		for (const auto& kv : defend) navyBusy.push_back (kv.first);
		for (const unsigned int id : opMoved_) navyBusy.push_back (id);
		std::vector<const cVehicle*> armyFree;
		for (const cVehicle* v : opCtx.fighters)
		{
			if (!isArmyRole (roles_.roleOf (*v)) || !isIdle (*v) || ops_.committed (v->getId())) continue;
			if (std::find (navyBusy.begin(), navyBusy.end(), v->getId()) != navyBusy.end() || shouldRetreat (*v, enemies)) continue;
			armyFree.push_back (v);
		}
		navy_.setRally (ops_.rallyPoint()); // причал транспорта — у точки сбора армии, не у фабрик
		navy_.plan (model, *player, view, world_, roles_, defense_, navyBusy, armyFree, out);
		if (navy_.guardsWanted() > 0 && navy_.guardPoint().x() >= 0)
		{
			std::vector<std::tuple<long long, unsigned int>> cands;
			for (const cVehicle* v : armyFree)
			{
				if (navy_.claimed (v->getId())) continue;
				const long long d2 = (v->getPosition() - navy_.guardPoint()).l2NormSquared();
				if (d2 <= 30LL * 30) cands.emplace_back (d2, v->getId());
			}
			std::sort (cands.begin(), cands.end());
			for (size_t k = 0; k < cands.size() && static_cast<int> (k) < navy_.guardsWanted(); ++k) shipGuards.push_back (std::get<1> (cands[k]));
		}
	}
	// шаг 13, часть 3: коммандос (П2, П18) — чужие, диверсия, налёт; авиация (П15–П17, П10) — AWAC, бомбардировщики,
	// истребители, авиатранспорт, налёт «AWAC + коммандос + бомбардировщики»
	commando_.plan (model, *player, view, world_, roles_, defense_, out);
	air_.plan (model, *player, view, world_, roles_, defense_, commando_, out);
	// задания операций армии (шаг 8) — машинам без приказа: огонь, отход, тревога и оперативка — раньше
	std::map<unsigned int, sOpTask> opTasks;
	{
		std::vector<unsigned int> busy;
		for (const cVehicle* v : opCtx.fighters)
			if (navy_.claimed (v->getId())) busy.push_back (v->getId()); // шаг 13, ч. 2: груз флота
		for (const unsigned int id : shipGuards) busy.push_back (id);
		for (size_t a = 0; a < attackers.size(); ++a)
			if (focus[a] >= 0) busy.push_back (attackers[a]->getId());
		for (const auto& kv : kite) busy.push_back (kv.first);
		for (const auto& kv : sweep) busy.push_back (kv.first); // шаг 13: обход у выключенного здания
		for (const cVehicle* v : opCtx.fighters)
		{
			const unsigned int id = v->getId();
			if (!isIdle (*v) || std::find (opMoved_.begin(), opMoved_.end(), id) != opMoved_.end() || defend.count (id) != 0
				|| shouldRetreat (*v, enemies))
				busy.push_back (id);
		}
		for (const cVehicle* v : opCtx.scoutUnits)
			if (!isIdle (*v) || opMovedId (v->getId()) || shouldRetreat (*v, enemies))
				busy.push_back (v->getId());
		ops_.plan (opCtx, busy, opTasks);
	}

	// видимые враги, к которым сближаются бойцы без тревоги: без тех, на кого защитников хватило
	// (шаг 9: и без самолётов — наземным их не достать, ПВО держится у армии и базы)
	std::vector<sEnemy> approachable;
	for (const auto& e : enemies)
		if (std::find (covered.begin(), covered.end(), e.unit->getId()) == covered.end()
			&& e.unit->getStaticUnitData().factorAir <= 0)
			approachable.push_back (e);

	// --- шаг 9: сопровождение — ПВО (половина при армии в походе, остальные у базы) и ремонтники ---
	bool armyOut = false;
	const cPosition armyLead = ops_.escortAnchor (&armyOut);
	const cPosition rally = ops_.rallyPoint();
	// точка в хвосте армии: от лидера на 3 клетки к точке сбора
	const cPosition armyRear = armyOut ? cPosition (armyLead + signDirOf (rally - armyLead) * 3) : rally;
	std::vector<unsigned int> aaIds;
	for (const auto& v : player->getVehicles())
		if (roles_.roleOf (*v) == eRole::AntiAir) aaIds.push_back (v->getId());
	std::sort (aaIds.begin(), aaIds.end());
	std::vector<cPosition> escortReserved;
	// у своих зданий не стоять: кольцо выезда фабрик, площадки
	auto nextToOwnBuilding = [&] (const cPosition& c) {
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition n = c + cPosition (dx, dy);
				if (!map->isValidPosition (n)) continue;
				for (const cBuilding* b : map->getField (n).getBuildings())
					if (b->getOwner() == player && b->getStaticUnitData().surfacePosition == eSurfacePosition::Ground) return true;
			}
		return false;
	};
	auto chebDist = [] (const cPosition& a, const cPosition& b) { return std::max (std::abs (a.x() - b.x()), std::abs (a.y() - b.y())); };
	// шаг 13 (П4): база ослепла (радары нужны, а работающих нет; мобильного радара нет) — первый по id
	// разведчик (обзор 9) встаёт у центра базы, пока радары не вернутся
	unsigned int radarBackupScout = kNoUnit;
	if (defense_.counts().radarsWanted > 0 && defense_.counts().radarsWorking == 0)
	{
		bool scanner = false;
		for (const auto& v : player->getVehicles())
			if (roles_.roleOf (*v) == eRole::Scanner && !v->isDisabled()) scanner = true;
		if (!scanner)
			for (const auto& v : player->getVehicles())
				if (roles_.roleOf (*v) == eRole::Scout && !v->isUnitLoaded() && !v->isDisabled() && v->getId() < radarBackupScout)
					radarBackupScout = v->getId();
	}
	// шаг 13 (П8): геолог при армии — второй по id (если геологов больше одного)
	unsigned int escortSurveyor = kNoUnit;
	{
		std::vector<unsigned int> surveyors;
		for (const auto& v : player->getVehicles())
			if (v->getStaticUnitData().vehicleData.canSurvey && !v->isUnitLoaded()) surveyors.push_back (v->getId());
		std::sort (surveyors.begin(), surveyors.end());
		if (surveyors.size() >= 2) escortSurveyor = surveyors[1];
	}
	// машина сопровождения: рядом с anchor (до holdR), вне «удара с подъезда» врага и не у своих зданий
	auto escortTo = [&] (const cVehicle& v, const cPosition& anchor, int holdR, const char* reason) {
		if (anchor.x() < 0) return;
		const cPosition up = v.getPosition();
		if (chebDist (up, anchor) <= holdR && world_.groundStrike (up) == 0 && !nextToOwnBuilding (up)
			&& std::find (escortReserved.begin(), escortReserved.end(), up) == escortReserved.end())
		{
			escortReserved.push_back (up);
			holdReason_[v.getId()] = reason; // шаг 11: на месте — стоять (сохранить выстрел)
			return; // уже на месте — стоять (сохранить выстрел)
		}
		cPosition best (-1, -1);
		long long bestKey = std::numeric_limits<long long>::max();
		for (int r = 0; r <= holdR + 3 && best.x() < 0; ++r)
			for (int dy = -r; dy <= r; ++dy)
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
					const cPosition c = anchor + cPosition (dx, dy);
					if (!view.isValidPosition (c) || (c != up && !view.possiblePlace (v, c))) continue;
					if (world_.groundStrike (c) > 0 || nextToOwnBuilding (c)) continue;
					if (std::find (escortReserved.begin(), escortReserved.end(), c) != escortReserved.end()) continue;
					const long long key = (c - up).l2NormSquared();
					if (key < bestKey)
					{
						bestKey = key;
						best = c;
					}
				}
		if (best.x() < 0 || best == up) return;
		auto path = threatAwarePath (v, view, best, world_, eThreatLayer::Strike);
		cutToTurn (v, view, path);
		if (path.empty()) return;
		escortReserved.push_back (path.back());
		cOrder o;
		o.type = eOrderType::Move;
		o.playerId = playerId;
		o.unitId = v.getId();
		o.path = std::move (path);
		o.position = o.path.back(); // цель пути — для журнала и дневника
		o.level = "micro";
		o.reason = reason;
		out.push_back (std::move (o));
		++stats_.escorts;
	};
	// выстрел в конце пути («подъехал — выстрелил»): цель в дальности последней клетки, выстрел остался
	auto moveFireTarget = [&] (const cVehicle& v, const std::vector<cPosition>& path) -> unsigned int {
		return endMoveTarget (model, v, path, attackableTargets (model, world_, v));
	};
	// поддержка: клетка вне «удара с подъезда» врага, бить с предельной дальности (цели — у near)
	auto supportStandoff = [&] (const cVehicle& v, const cPosition& near, int radius, const char* reason) {
		std::vector<const sEnemyMemory*> targets;
		for (const sEnemyMemory* m : attackableTargets (model, world_, v))
			if ((m->pos - near).l2NormSquared() <= static_cast<long long> (radius) * radius) targets.push_back (m);
		if (targets.empty()) return false;
		sStandoff s = findStandoff (model, view, world_, v, targets, v.data.getRange(), true, supportMaxDanger (roles_.roleOf (v)), escortReserved);
		if (s.path.empty())
		{
			holdReason_[v.getId()] = std::string (reason) + ": best cell"; // шаг 11
			return true; // стоять: здесь лучшее место
		}
		escortReserved.push_back (s.path.back());
		cOrder o;
		o.type = eOrderType::Move;
		o.playerId = playerId;
		o.unitId = v.getId();
		o.path = std::move (s.path);
		o.position = o.path.back();
		o.unitId2 = s.attackId;
		o.level = "micro";
		o.reason = std::string (reason) + (s.attackId != kNoUnit ? " +fire" : "");
		out.push_back (std::move (o));
		if (s.attackId != kNoUnit) ++stats_.moveFire;
		return true;
	};

	for (const auto& vehicle : player->getVehicles())
	{
		if (!isIdle (*vehicle)) continue;
		if (std::find (opMoved_.begin(), opMoved_.end(), vehicle->getId()) != opMoved_.end()) continue; // отогнан с площадки

		const auto& sd = vehicle->getStaticUnitData();
		const eRole role = roles_.roleOf (*vehicle);
		// шаг 13, часть 3: самолёты и коммандос — приказы дали свои модули (ai_air, ai_commando)
		if (air_.claimed (vehicle->getId()) || commando_.claimed (vehicle->getId()) || isAirRole (role)) continue;

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

			// 1½) шаг 9: поддержка в зоне огня врага — уйти, где он не достанет (и выстрелить оттуда)
			if (const auto k = kite.find (vehicle->getId()); k != kite.end())
			{
				cOrder o;
				o.type = eOrderType::Move;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.path = k->second.path;
				o.position = o.path.back();
				o.unitId2 = k->second.attackId;
				o.level = "micro";
				o.reason = std::string ("support: out of fire") + (k->second.attackId != kNoUnit ? " +fire" : "");
				out.push_back (std::move (o));
				++stats_.kites;
				if (k->second.attackId != kNoUnit) ++stats_.moveFire;
				continue;
			}

			// шаг 13, часть 2: корабли и груз флота — приказы дал флот (ai_navy) выше
			if (navy_.claimed (vehicle->getId())) continue;
			// П13: охрана стройки верфи — у конструктора, вне «удара с подъезда» врага
			if (std::find (shipGuards.begin(), shipGuards.end(), vehicle->getId()) != shipGuards.end())
			{
				escortTo (*vehicle, navy_.guardPoint(), 3, "guard the shipyard builder");
				continue;
			}

			// шаг 13 (П7): обход клеток у выключенного здания — въезд сдвигает или обнаруживает коммандос
			if (const auto sw = sweep.find (vehicle->getId()); sw != sweep.end())
			{
				cPathCalculator pc (*vehicle, view, sw->second, false);
				auto path = pathToVector (pc.calcPath());
				cutToTurn (*vehicle, view, path);
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = std::move (path);
					o.position = o.path.back();
					o.level = "micro";
					o.reason = "commando alarm: sweep around the disabled building";
					out.push_back (std::move (o));
					++stats_.guardMoves;
					continue;
				}
			}

			// шаг 13 (П7): пехота — на посту у ключевых зданий (ai_defense): коммандос врага видят только
			// пехота и коммандос, и только при шаге — раз в 4 хода (у выключенного здания — каждый ход) шаг на
			// соседнюю клетку и обратно; стоящий пехотинец бережёт выстрел на ответный огонь. Обнаруженного
			// диверсанта убивают огнём в этом же ходу (фокус-огонь выше, заходы 2+ — decidePass)
			if (role == eRole::Guard || role == eRole::Commando)
			{
				const auto post = defense_.postOf (vehicle->getId());
				const cPosition up = vehicle->getPosition();
				if (post.post.x() < 0)
				{
					holdReason_[vehicle->getId()] = "guard: no post";
					continue;
				}
				cPosition dest (-1, -1);
				const char* why = nullptr;
				if (up != post.post && up != post.patrol)
				{
					dest = post.post;
					why = "guard: to post";
				}
				else if (post.patrol.x() >= 0 && defense_.patrolTurn (vehicle->getId()))
				{
					dest = up == post.post ? post.patrol : post.post;
					why = post.search ? "guard: search step (building disabled)" : "guard: patrol step";
				}
				if (dest.x() < 0)
				{
					holdReason_[vehicle->getId()] = "guard: at post";
					continue;
				}
				auto path = threatAwarePath (*vehicle, view, dest, world_, eThreatLayer::Fire);
				cutToTurn (*vehicle, view, path);
				if (path.empty())
				{
					holdReason_[vehicle->getId()] = "guard: post blocked";
					continue;
				}
				cOrder o;
				o.type = eOrderType::Move;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.path = std::move (path);
				o.position = o.path.back();
				o.level = "micro";
				o.reason = why;
				out.push_back (std::move (o));
				++stats_.guardMoves;
				continue;
			}

			// шаг 9: ПВО бьёт только самолёты (фокус-огонь выше) — держится у армии или у базы
			if (role == eRole::AntiAir)
			{
				// шаг 13 (П6): налёт рядом — не ехать: в «Turn based» выстрелы вернутся только в начале своего
				// хода, и переехавшая зенитка весь ход врага молчит (у мобильной ПВО выстрел один)
				if (vehicle->data.getShots() > 0 && world_.groundFire (vehicle->getPosition()) == 0
					&& defense_.airRaidNear (world_, vehicle->getPosition(), 24))
				{
					holdReason_[vehicle->getId()] = "aa: air raid near, keep the shot";
					++stats_.aaHolds;
					continue;
				}
				const size_t idx = static_cast<size_t> (std::find (aaIds.begin(), aaIds.end(), vehicle->getId()) - aaIds.begin());
				const bool withArmy = armyOut && idx % 2 == 0;
				escortTo (*vehicle, withArmy ? armyRear : player->getLandingPos(), withArmy ? 2 : 5,
					withArmy ? "aa escorts army" : "aa guards base");
				continue;
			}

			// шаг 13 (П4): разведчик на подстраховке радаров — у центра базы
			if (vehicle->getId() == radarBackupScout && !shouldRetreat (*vehicle, enemies))
			{
				escortTo (*vehicle, defense_.baseCenter(), 3, "scout: radar backup (base is blind)");
				continue;
			}

			// шаг 9: разведчик — задание операций (разведка или корректировка); ранен — отход ниже
			// шаг 13, часть 2: разведчик, высаженный флотом на чужую сушу (разведка высадкой), — к невиданному у фокуса
			// врага, мимо его зоны огня; найдёт здания врага — флот повезёт бойцов
			if (role == eRole::Scout && offHome (*vehicle) && !shouldRetreat (*vehicle, enemies))
			{
				cPosition goal;
				if (navy_.landedGoal (view, world_, *vehicle, goal, true))
				{
					auto path = threatAwarePath (*vehicle, view, goal, world_, eThreatLayer::Fire);
					cutToTurn (*vehicle, view, path);
					if (!path.empty())
					{
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.path = std::move (path);
						o.position = o.path.back();
						o.level = "micro";
						o.reason = "recon: landed scout looks for the enemy base";
						out.push_back (std::move (o));
						continue;
					}
				}
				holdReason_[vehicle->getId()] = "recon: hold";
				continue;
			}
			if (role == eRole::Scout && !shouldRetreat (*vehicle, enemies))
			{
				if (const auto t = opTasks.find (vehicle->getId()); t != opTasks.end() && !t->second.path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = t->second.path;
					o.position = o.path.back();
					o.level = "operational";
					o.reason = t->second.reason;
					out.push_back (std::move (o));
					++stats_.opMoves;
				}
				else
					holdReason_[vehicle->getId()] = "scout: hold"; // шаг 11
				continue;
			}

			// 1¾) шаг 9: боезапаса не хватит на залп — к снабженцу с металлом, если он рядом и вне огня
			if (isArmyRole (role) && vehicle->data.getAmmo() < vehicle->data.getShotsMax())
			{
				cPosition dest (-1, -1);
				long long bestD = static_cast<long long> (kRepairReach) * kRepairReach + 1;
				for (const auto& sp : player->getVehicles())
				{
					if (roles_.roleOf (*sp) != eRole::Supply || sp->getStoredResources() <= 0) continue;
					if (world_.groundFire (sp->getPosition()) > 0) continue;
					for (const auto& c : ringCells (sp->getPosition(), false))
					{
						if (!view.isValidPosition (c) || world_.groundFire (c) > 0) continue;
						if (c != vehicle->getPosition() && !view.possiblePlace (*vehicle, c)) continue;
						if (std::find (escortReserved.begin(), escortReserved.end(), c) != escortReserved.end()) continue;
						const long long d2 = (c - vehicle->getPosition()).l2NormSquared();
						if (d2 < bestD)
						{
							bestD = d2;
							dest = c;
						}
					}
				}
				if (dest == vehicle->getPosition())
				{
					holdReason_[vehicle->getId()] = "at supply: wait for ammo"; // шаг 11
					continue; // уже у снабженца — ждать
				}
				if (dest.x() >= 0)
				{
					auto path = threatAwarePath (*vehicle, view, dest, world_, eThreatLayer::Fire);
					cutToTurn (*vehicle, view, path);
					if (!path.empty())
					{
						escortReserved.push_back (path.back());
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.path = std::move (path);
						o.position = o.path.back();
						o.level = "micro";
						o.reason = "to supply for ammo";
						out.push_back (std::move (o));
						continue;
					}
				}
			}

			// 2) ранен/под обстрелом — КОРОТКИЙ отход за спины здоровых союзников
			//    (не до базы): юнит выходит из-под огня, но остаётся у фронта,
			//    где его может отремонтировать ремонтник. Стоим, если уже у цели.
			if (shouldRetreat (*vehicle, enemies))
			{
				bool found = false;
				bool toRepairer = false;
				cPosition dest (-1, -1);
				// шаг 9: ремонтник с металлом рядом и вне зоны огня — отойти к нему (он починит)
				{
					long long bestD = static_cast<long long> (kRetreatToRepairer) * kRetreatToRepairer + 1;
					for (const auto& rp : player->getVehicles())
					{
						if (roles_.roleOf (*rp) != eRole::Repair || rp->getStoredResources() <= 0) continue;
						if (world_.groundFire (rp->getPosition()) > 0) continue;
						for (const auto& c : ringCells (rp->getPosition(), false))
						{
							if (!view.isValidPosition (c) || world_.groundFire (c) > 0) continue;
							if (c != vehicle->getPosition() && !view.possiblePlace (*vehicle, c)) continue;
							const long long d2 = (c - vehicle->getPosition()).l2NormSquared();
							if (d2 < bestD)
							{
								bestD = d2;
								dest = c;
								toRepairer = true;
							}
						}
					}
					if (toRepairer && dest == vehicle->getPosition())
					{
						holdReason_[vehicle->getId()] = "at repairer: wait for repair"; // шаг 11
						continue; // уже у ремонтника — ждать ремонта
					}
				}
				if (!toRepairer)
					dest = findRetreatTarget (*vehicle, *player, enemies, view, found);
				const bool toBase = !found && !toRepairer;
				if (toBase)
					dest = player->getLandingPos(); // нет строя/направления — старый запасной путь
				// шаг 7: отход — в клетку без угрозы (у точки отхода, иначе — рядом с собой)
				const eThreatLayer rl = kRetreatLayer;
				if (world_.threatFor (*vehicle, dest, rl) > 0)
				{
					cPosition safe;
					if (world_.nearestSafeCell (*vehicle, view, dest, kSafeSnap, vehicle->getPosition(), safe, rl)
						|| world_.nearestSafeCell (*vehicle, view, vehicle->getPosition(), kRetreatMax, dest, safe, rl))
						dest = safe;
				}
				auto path = threatAwarePath (*vehicle, view, dest, world_, rl);
				if (path.empty()) holdReason_[vehicle->getId()] = "retreat: at the retreat cell"; // шаг 11
				if (!path.empty())
				{
					// опасная клетка в отчёте — всегда по «дальность + ход»; отдельно — в зоне огня
					const bool unsafe = world_.threatFor (*vehicle, path.back()) > 0;
					if (world_.threatFor (*vehicle, path.back(), eThreatLayer::Fire) > 0) ++stats_.fireRetreats;
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = std::move (path);
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = std::string (toRepairer ? "retreat to repairer" : (toBase ? "retreat to base" : "retreat behind line")) + (unsafe ? " (unsafe)" : "");
					out.push_back (std::move (o));
					++stats_.retreats;
					if (unsafe) ++stats_.unsafeRetreats;
				}
				continue; // уже у цели отхода — стоим, не лезем обратно в бой
			}

			// 3) тревога у базы: защитник идёт к врагу у ключевого здания
			if (const auto d = defend.find (vehicle->getId()); d != defend.end())
			{
				// шаг 9: поддержка — бить врагов тревоги с предельной дальности, вне их «удара с подъезда»
				if (isSupportRole (role) && supportStandoff (*vehicle, d->second, kAlarmGroup + 4, "base alarm: support"))
					continue;
				cPathCalculator pc (*vehicle, view, d->second, true);
				auto path = pathToVector (pc.calcPath());
				// как и сближение — только до края зоны огня (если сами не под ним)
				if (world_.groundFire (vehicle->getPosition()) == 0)
				{
					size_t cut = 0;
					while (cut < path.size() && world_.groundFire (path[cut]) == 0) ++cut;
					if (cut < path.size())
					{
						path.resize (cut);
						if (path.empty())
						{
							holdReason_[vehicle->getId()] = "base alarm: at the fire edge"; // шаг 11
							++stats_.holds;
							continue;
						}
					}
				}
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.unitId2 = moveFireTarget (*vehicle, path); // шаг 9: доехал до врага в дальности — выстрел
					o.path = std::move (path);
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = std::string ("base alarm: defend") + (o.unitId2 != kNoUnit ? " +fire" : "");
					if (o.unitId2 != kNoUnit) ++stats_.moveFire;
					out.push_back (std::move (o));
					continue;
				}
				// пути нет (уже в радиусе, а враг не виден) — по обычным правилам
			}

			// 3½) операция армии (шаг 8): сбор, поход строем, бой группой, отход, разведка, резерв
			if (const auto t = opTasks.find (vehicle->getId()); t != opTasks.end())
			{
				if (!t->second.path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.unitId2 = t->second.attackId; // шаг 9: выстрел в конце пути
					o.path = t->second.path;
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "operational";
					o.reason = t->second.reason;
					out.push_back (std::move (o));
					++stats_.opMoves;
					if (o.unitId2 != kNoUnit) ++stats_.moveFire;
				}
				else
				{
					holdReason_[vehicle->getId()] = t->second.reason.empty() ? std::string ("operation: hold") : t->second.reason; // шаг 11
					++stats_.opHolds;
				}
				continue;
			}

			// 4) враги видны, но не в радиусе — сближаемся (кроме тех, на кого хватило защитников);
			//    шаг 7 — только до края зоны огня врага: стоящий бьёт первым и ответным
			//    огнём (cVehicle::provokeReactionFire), входящий под выстрелы — проигрывает
			if (const sEnemy* n = nearestEnemy (approachable, vehicle->getPosition()))
			{
				// шаг 9: поддержка — встать вне «удара с подъезда» и бить с предельной дальности
				if (isSupportRole (role) && supportStandoff (*vehicle, n->pos, vehicle->data.getRange() + 10, "support: standoff"))
					continue;
				if (world_.groundFire (vehicle->getPosition()) == 0)
				{
					cPathCalculator pc (*vehicle, view, n->pos, true);
					auto path = pathToVector (pc.calcPath());
					size_t cut = 0;
					while (cut < path.size() && world_.groundFire (path[cut]) == 0) ++cut;
					if (cut < path.size())
					{
						path.resize (cut);
						if (path.empty())
						{
							holdReason_[vehicle->getId()] = "approach: at the fire edge"; // шаг 11
							++stats_.holds; // уже у края — ждать, пока враг войдёт в нашу дальность
							continue;
						}
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.path = std::move (path);
						o.position = o.path.back(); // цель пути — для журнала и дневника
						o.level = "micro";
						o.reason = "approach to fire edge";
						out.push_back (std::move (o));
						continue;
					}
					// путь до цели вне зоны огня (враг без оружия, мы дальнобойнее) — обычное сближение
				}
				cPathCalculator pc (*vehicle, view, n->pos, true);
				auto path = pc.calcPath();
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					// шаг 9: путь до цели вне её огня (без оружия или мы дальнобойнее) — выстрел по приезде
					o.unitId2 = n->unit != nullptr ? n->unit->getId() : kNoUnit;
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = std::string ("approach enemy") + (o.unitId2 != kNoUnit ? " +fire" : "");
					if (o.unitId2 != kNoUnit) ++stats_.moveFire;
					out.push_back (std::move (o));
					continue;
				}
			}

			// 4½) шаг 13, часть 2: высаженные флотом на чужую сушу — к ближайшему виденному зданию врага там (иначе — к
			//     невиданному у фокуса врага), мимо его зоны огня
			if (offHome (*vehicle))
			{
				cPosition goal;
				if (navy_.landedGoal (view, world_, *vehicle, goal, false))
				{
					if (goal == vehicle->getPosition())
					{
						holdReason_[vehicle->getId()] = "landing force: hold the beachhead (no soft target)";
						continue;
					}
					auto path = threatAwarePath (*vehicle, view, goal, world_, eThreatLayer::Fire);
					cutToTurn (*vehicle, view, path);
					if (!path.empty())
					{
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.unitId2 = moveFireTarget (*vehicle, path);
						o.path = std::move (path);
						o.position = o.path.back();
						o.level = "micro";
						o.reason = std::string ("landing force: toward the enemy") + (o.unitId2 != kNoUnit ? " +fire" : "");
						out.push_back (std::move (o));
						continue;
					}
				}
			}
			// 5) врагов не видно — выдвигаемся к ближайшей неразведанной клетке
			//    (разведка боем; на симметричной карте армии встретятся и вступят в бой);
			//    шаг 7 — только к безопасным клеткам и в обход опасных (замена — шаг 8)
			{
				auto path = pathToUnexplored (*vehicle, view, scoutCache, world_, kScoutLayer);
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = std::move (path);
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = "advance (scout)";
					out.push_back (std::move (o));
					continue;
				}
			}

			// 6) делать нечего (разведка заперта угрозами) — резерв идёт к ближайшему врагу тревоги
			{
				const cPosition* best = nullptr;
				for (const auto& p : alarmTargets)
					if (best == nullptr || (p - vehicle->getPosition()).l2NormSquared() < (*best - vehicle->getPosition()).l2NormSquared())
						best = &p;
				if (best != nullptr)
				{
					cPathCalculator pc (*vehicle, view, *best, true);
					auto path = pathToVector (pc.calcPath());
					if (world_.groundFire (vehicle->getPosition()) == 0)
					{
						size_t cut = 0;
						while (cut < path.size() && world_.groundFire (path[cut]) == 0) ++cut;
						if (cut < path.size()) path.resize (cut);
					}
					if (!path.empty())
					{
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.path = std::move (path);
						o.position = o.path.back(); // цель пути — для журнала и дневника
						o.level = "micro";
						o.reason = "base alarm: reserve";
						out.push_back (std::move (o));
						++stats_.reserve;
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

			// шаг 9: чинить всех соседей, пока есть металл (ремонт — приказ, не ход и не выстрел)
			int metal = vehicle->getStoredResources();
			bool repaired = false;
			for (cVehicle* d : damaged)
			{
				if (metal <= 0) break;
				if (!vehicle->canSupply (d, eSupplyType::REPAIR)) continue;
				cOrder o;
				o.type = eOrderType::Repair;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.unitId2 = d->getId();
				o.level = "micro";
				o.reason = "repair damaged unit";
				out.push_back (std::move (o));
				++stats_.repairs;
				repaired = true;
				// металла на ремонт: hp за единицу — 4 × hpMax / цена (cActionRepairReload)
				const int hpPerMetal = std::max (1, 4 * d->data.getHitpointsMax() / std::max (1, d->data.getBuildCost()));
				metal -= (d->data.getHitpointsMax() - d->data.getHitpoints() + hpPerMetal - 1) / hpPerMetal;
			}
			if (repaired || metal <= 0) continue; // без металла — оперативка ведёт к базе

			// к самому нужному раненому, к которому безопасно подойти: он и клетка рядом с ним —
			// вне зоны огня врага; путь — в обход неё, не дальше хода
			bool moved = false;
			for (cVehicle* d : damaged)
			{
				if (d->getStaticUnitData().factorAir > 0 || d->isBeingAttacked()) continue;
				if (chebDist (d->getPosition(), vehicle->getPosition()) > kRepairReach) continue;
				if (world_.groundFire (d->getPosition()) > 0) continue;
				cPosition best (-1, -1);
				long long bestD = std::numeric_limits<long long>::max();
				for (const auto& c : ringCells (d->getPosition(), d->getIsBig()))
				{
					if (!view.isValidPosition (c) || world_.groundFire (c) > 0) continue;
					if (c != vehicle->getPosition() && !view.possiblePlace (*vehicle, c)) continue;
					if (std::find (escortReserved.begin(), escortReserved.end(), c) != escortReserved.end()) continue;
					const long long d2 = (c - vehicle->getPosition()).l2NormSquared();
					if (d2 < bestD)
					{
						bestD = d2;
						best = c;
					}
				}
				if (best.x() < 0) continue;
				if (best == vehicle->getPosition())
				{
					moved = true; // уже рядом (раненый ещё стреляет или едет) — ждать
					break;
				}
				auto path = threatAwarePath (*vehicle, view, best, world_, eThreatLayer::Fire);
				cutToTurn (*vehicle, view, path);
				if (path.empty()) continue;
				escortReserved.push_back (path.back());
				cOrder o;
				o.type = eOrderType::Move;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.path = std::move (path);
				o.position = o.path.back(); // цель пути — для журнала и дневника
				o.level = "micro";
				o.reason = "approach damaged unit";
				out.push_back (std::move (o));
				moved = true;
				break;
			}
			if (moved) continue;
			// раненых нет — за армией в хвосте (вне «удара с подъезда» врага), армии в походе нет — у точки сбора
			escortTo (*vehicle, armyRear, 3, armyOut ? "repairer follows army" : "repairer at rally");
			continue;
		}

		// --- снабженец (шаг 9): пополняет боезапас соседям — как ремонтник, но патроны, а не hp ---
		if (role == eRole::Supply)
		{
			// кому нужен боезапас: меньше всего осталось (доля) — раньше, при равенстве — по id
			std::vector<cVehicle*> needy;
			for (const auto& v : player->getVehicles())
				if (v.get() != vehicle.get() && v->data.getAmmoMax() > 0 && v->data.getAmmo() < v->data.getAmmoMax()) needy.push_back (v.get());
			std::sort (needy.begin(), needy.end(), [] (const cVehicle* a, const cVehicle* b) {
				const long long ka = static_cast<long long> (a->data.getAmmo()) * b->data.getAmmoMax();
				const long long kb = static_cast<long long> (b->data.getAmmo()) * a->data.getAmmoMax();
				if (ka != kb) return ka < kb;
				return a->getId() < b->getId();
			});
			int metal = vehicle->getStoredResources();
			bool rearmed = false;
			for (cVehicle* d : needy)
			{
				if (metal <= 0) break;
				if (!vehicle->canSupply (d, eSupplyType::REARM)) continue;
				cOrder o;
				o.type = eOrderType::Rearm;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.unitId2 = d->getId();
				o.level = "micro";
				o.reason = "rearm unit";
				out.push_back (std::move (o));
				++stats_.rearms;
				rearmed = true;
				--metal; // боезапас целиком — за единицу металла (cActionRepairReload)
			}
			if (rearmed || metal <= 0) continue;
			// к машине с боезапасом на исходе (меньше половины), к которой безопасно подойти
			bool moved = false;
			for (cVehicle* d : needy)
			{
				if (d->data.getAmmo() * 2 >= d->data.getAmmoMax()) break; // дальше — у всех больше половины
				if (d->getStaticUnitData().factorAir > 0 || d->isBeingAttacked()) continue;
				if (chebDist (d->getPosition(), vehicle->getPosition()) > kRepairReach) continue;
				if (world_.groundFire (d->getPosition()) > 0) continue;
				cPosition best (-1, -1);
				long long bestD = std::numeric_limits<long long>::max();
				for (const auto& c : ringCells (d->getPosition(), d->getIsBig()))
				{
					if (!view.isValidPosition (c) || world_.groundFire (c) > 0) continue;
					if (c != vehicle->getPosition() && !view.possiblePlace (*vehicle, c)) continue;
					if (std::find (escortReserved.begin(), escortReserved.end(), c) != escortReserved.end()) continue;
					const long long d2 = (c - vehicle->getPosition()).l2NormSquared();
					if (d2 < bestD)
					{
						bestD = d2;
						best = c;
					}
				}
				if (best.x() < 0) continue;
				if (best == vehicle->getPosition())
				{
					moved = true; // рядом, но пополнить нельзя (стреляет или едет) — ждать
					break;
				}
				auto path = threatAwarePath (*vehicle, view, best, world_, eThreatLayer::Fire);
				cutToTurn (*vehicle, view, path);
				if (path.empty()) continue;
				escortReserved.push_back (path.back());
				cOrder o;
				o.type = eOrderType::Move;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.path = std::move (path);
				o.position = o.path.back();
				o.level = "micro";
				o.reason = "approach unit to rearm";
				out.push_back (std::move (o));
				moved = true;
				break;
			}
			if (moved) continue;
			escortTo (*vehicle, armyRear, 3, armyOut ? "supply follows army" : "supply at rally");
			continue;
		}

		// --- разведчик ---
		// Он один и заменить его нечем (малой фабрики правила не строят): сначала
		// разведать ресурсы у своей базы (там ставятся шахты), от врагов — назад.
		if (sd.vehicleData.canSurvey)
		{
			// шаг 13 (П8): второй геолог — при армии в походе: видит мины врага (путь их обходит, по ним можно
			// стрелять); армии в походе нет — разведывает, как первый
			if (armyOut && vehicle->getId() == escortSurveyor && world_.threatFor (*vehicle, vehicle->getPosition(), kBuilderLayer) == 0)
			{
				escortTo (*vehicle, armyRear, 2, "surveyor with army (sees mines)");
				continue;
			}
			const cPosition landing = player->getLandingPos();
			// шаг 7: под угрозой врага (карта угроз по памяти) — в ближайшую безопасную клетку,
			// при равной дальности — ближе к базе
			const eThreatLayer bl = kBuilderLayer;
			if (world_.threatFor (*vehicle, vehicle->getPosition(), bl) > 0)
			{
				cPosition dest;
				if (world_.nearestSafeCell (*vehicle, view, vehicle->getPosition(), kEvacuateRadius, landing, dest, bl))
				{
					auto path = threatAwarePath (*vehicle, view, dest, world_, bl);
					if (!path.empty())
					{
						cOrder o;
						o.type = eOrderType::Move;
						o.playerId = playerId;
						o.unitId = vehicle->getId();
						o.path = std::move (path);
						o.position = o.path.back(); // цель пути — для журнала и дневника
						o.level = "micro";
						o.reason = "surveyor away from enemy";
						out.push_back (std::move (o));
						++stats_.evacuations;
					}
					continue;
				}
			}
			auto path = pathToUnexplored (*vehicle, view, scoutCache, world_, bl, &landing, kSurveyorHome);
			if (path.empty()) path = pathToUnexplored (*vehicle, view, scoutCache, world_, bl);
			if (!path.empty())
			{
				cOrder o;
				o.type = eOrderType::Move;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.path = std::move (path);
				o.position = o.path.back(); // цель пути — для журнала и дневника
				o.level = "micro";
				o.reason = "survey unexplored";
				out.push_back (std::move (o));
			}
			continue;
		}

		// --- шаг 13 (П8): минёр — мины на подходах к базе (ai_defense), по одной: доехал до клетки поля —
		// включил закладку (мина ложится сразу), в следующем заходе выключил (иначе мина ляжет на каждой
		// клетке пути); металл — у базы (оперативка)
		if (role == eRole::MineLayer)
		{
			const auto& field = defense_.mineField();
			if (field.empty() || vehicle->getStoredResources() <= 0) continue;
			const cPosition up = vehicle->getPosition();
			if (std::find (field.begin(), field.end(), up) != field.end() && world_.groundFire (up) == 0)
			{
				cOrder o;
				o.type = eOrderType::ToggleLayMines;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.level = "micro";
				o.reason = "minelayer: lay a mine on the approach";
				out.push_back (std::move (o));
				minesToggled_.push_back (vehicle->getId());
				++stats_.minesLaid;
				continue;
			}
			// ближайшая клетка поля без угрозы (по порядку поля при равенстве)
			cPosition best (-1, -1);
			long long bestD = std::numeric_limits<long long>::max();
			for (const auto& c : field)
			{
				if (world_.threatFor (*vehicle, c, kBuilderLayer) > 0 || std::find (escortReserved.begin(), escortReserved.end(), c) != escortReserved.end()) continue;
				if (c != up && !view.possiblePlace (*vehicle, c)) continue;
				const long long d2 = (c - up).l2NormSquared();
				if (d2 < bestD)
				{
					bestD = d2;
					best = c;
				}
			}
			if (best.x() < 0) continue;
			auto path = threatAwarePath (*vehicle, view, best, world_, kBuilderLayer);
			cutToTurn (*vehicle, view, path);
			if (path.empty()) continue;
			escortReserved.push_back (path.back());
			cOrder o;
			o.type = eOrderType::Move;
			o.playerId = playerId;
			o.unitId = vehicle->getId();
			o.path = std::move (path);
			o.position = o.path.back();
			o.level = "micro";
			o.reason = "minelayer: to the mine field";
			out.push_back (std::move (o));
			continue;
		}

		// конструкторы и прочие — оперативка (этап 4)
	}
}

} // namespace ai
