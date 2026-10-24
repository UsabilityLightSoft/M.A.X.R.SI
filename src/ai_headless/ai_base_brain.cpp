/***************************************************************************
 * ai_base_brain.cpp — замороженная копия ai_rule_brain.cpp (мозг «base»).
 *
 * ЗАМОРОЖЕН 24.10.2026, НЕ ПРАВИТЬ — эталон для турниров шагов 7–12 (PLAN.md §6):
 * копия ai_rule_brain.{h,cpp} на 24.10.2026 (эталоны rule v12/v13), мозг «base».
 * Правила усиливаются в ai_rule_brain.cpp; эта копия не меняется, чтобы турнир
 * «rule против base» мерил именно изменения шага. Отличия от копии — только
 * имена: класс cBaseRuleBrain, страж заголовка, name() = "base".
 *
 * Описание копии: алгоритмический мозг: микро (этап 3), оперативка и
 * стратегия (этап 4), экономика без даровой подпитки движка (шаг 8 этапа 5).
 ***************************************************************************/

#include "ai_base_brain.h"

#include "ai_types.h"

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
#include "game/logic/pathcalculator.h"
#include "utility/log.h"
#include "utility/mathtools.h"

#include "game/logic/turncounter.h"

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
constexpr int kSurveyorDanger = 9;  // враг ближе — разведчик уходит к базе

// Путь к ближайшей неразведанной проходимой клетке (для surveyor и боевого «скаута»);
// home — только клетки не дальше radius (Чебышёв) от home. Ближайшая может быть
// недостижима (вода, отрезанный берег): такая помечается в кэше хода и берётся
// следующая — до tries попыток (раньше юнит в этом случае стоял на месте).
std::forward_list<cPosition> pathToUnexplored (const cVehicle& v, const cMapView& view,
	sScoutCache& cache, const cPosition* home = nullptr, int radius = 0, int tries = 3)
{
	auto& pass = scoutPassable (cache, v, view);
	const cPosition from = v.getPosition();
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
				bestDist = d;
				bestI = static_cast<int> (i);
			}
		}
		if (bestI < 0) return {};
		cPathCalculator pc (v, view, cache.unexplored[bestI], false);
		auto path = pc.calcPath();
		if (!path.empty()) return path;
		pass[bestI] = 0;
	}
	return {};
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

constexpr int kMinMineMetal = 3;   // шахта ради металла — от 3 металла в ход
constexpr int kMinMineOil = 4;     // шахта ради нефти (когда энергию не из чего делать)
constexpr int kMaxChain = 10;      // соединителей от базы до шахты — не больше
constexpr int kMaxSiteRadius = 18; // площадки шахт — не дальше от высадки (Чебышёв)
constexpr int kBaseRadius = 14;    // здания базы — не дальше от высадки
constexpr int kMaxFactories = 6;
constexpr int kFactoryRingMin = 4; // свободных клеток вокруг фабрики — для выезда машин
constexpr int kMaxStorages = 3;
constexpr int kStuckTurns = 3;     // ходов без пути к площадке — план снимается

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

void buildConnGrid (const cModel& model, const cPlayer& player, const cSubBase* main, sConnGrid& g)
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
		if (d >= kMaxChain) continue;
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
	const tFootprints& reserved, bool wantOil)
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
			if (!metalSpot && !oilSpot) continue;
			for (const auto& off : offs)
			{
				const cPosition q = c + off;
				if (overlapsAny (q, true, reserved)) continue;
				if (!canPlaceIgnoringVehicles (map, player, mineData, q)) continue;
				const int chain = chainTo (g, q, true);
				if (chain < 0) continue;
				int m = 0, o = 0;
				for (const auto& f : footprintCells (q, true))
				{
					m += exploredResource (map, player, f, eResourceType::Metal);
					o += exploredResource (map, player, f, eResourceType::Oil);
				}
				const int metal = std::min (m, cap);
				const int oil = std::min (o, cap - metal);
				if (metal < kMinMineMetal && !(wantOil && oil >= kMinMineOil)) continue;
				const int far = std::max (std::abs (q.x() - landing.x()), std::abs (q.y() - landing.y()));
				const long long score = metal * 100LL + oil * (wantOil ? 100LL : 25LL) - chain * 40LL - far * 3LL;
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
	const cStaticUnitData& data, const tFootprints& reserved)
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
						if (groundFree (map, c) && std::find (cells.begin(), cells.end(), c) == cells.end()) ++n;
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

// ближайшая свободная клетка рядом со зданием главной подбазы (там стройщик берёт металл)
cPosition findBaseDock (const cMap& map, const cPlayer& player, const cSubBase* main, const cVehicle& v,
	const cMapView& view, const tFootprints& reserved)
{
	cPosition best (-1, -1);
	int bestDist = std::numeric_limits<int>::max();
	for (const auto& b : player.getBuildings())
	{
		if (b->subBase != main) continue;
		for (const auto& c : ringCells (b->getPosition(), b->getIsBig()))
		{
			if (!map.isValidPosition (c) || !view.possiblePlace (v, c) || overlapsAny (c, false, reserved)) continue;
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
std::vector<cOrder> cBaseRuleBrain::decideTurn (int playerId, const cModel& model)
{
	std::vector<cOrder> orders;
	strategicDecisions (playerId, model, orders);
	operationalDecisions (playerId, model, orders);
	microDecisions (playerId, model, orders);
	return orders;
}

//------------------------------------------------------------------------------
// Стратегия (этап 4): фокус исследований + техпуть. Пока — статичные правила.
void cBaseRuleBrain::strategicDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
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
// Оперативка: стройка, производство, снабжение стройщиков — без даровой подпитки.
//
// Металл хода (бюджет) — stored + prod - needed главной подбазы: столько можно
// забрать, не заглушив к концу хода работающие фабрики (иначе движок
// отказывает в сдаче хода). Сначала пополняются стройщики (шахты, энергия,
// фабрики — вложения), остаток — фабрикам. Порядок приказов: пополнение →
// производство → стройка → запуск шахт; сервер исполняет их по порядку.
void cBaseRuleBrain::operationalDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
{
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return;

	const auto& unitsData = model.getUnitsData();
	const cMap& map = *model.getMap();
	const cMapView view (model.getMap(), sharedPlayerOf (model, playerId));

	const sEcon econ = readEcon (*player);
	const cSubBase* main = econ.sb;
	opMoved_.clear();

	sConnGrid grid;
	buildConnGrid (model, *player, main, grid);

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
	// план ещё выполним: площадка свободна от зданий и чужой стройки
	auto planValid = [&] (const sBuildPlan& p) {
		const auto& sd = unitsData->getStaticUnitData (planType (p));
		return canPlaceIgnoringVehicles (map, *player, sd, p.pos) && !overlapsAny (p.pos, sd.buildingData.isBig, inProgress);
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
			const cPosition dock = findBaseDock (map, *player, main, v, view, reservedFootprints());
			if (dock.x() >= 0) pushMove (v, view, dock, playerId, "op", "to base for metal", builderOrders);
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
			if (best.x() >= 0 && pushMove (v, view, best, playerId, "op", "to build site: " + bdata.getDefaultName(), builderOrders))
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
	// без дела вдали от базы — вернуться (не стоять на пути соединителей и выезде фабрик)
	auto idleToBase = [&] (const cVehicle& v) {
		if (refillSource (*player, main, v) != nullptr) return;
		const cPosition dock = findBaseDock (map, *player, main, v, view, reservedFootprints());
		if (dock.x() >= 0) pushMove (v, view, dock, playerId, "op", "idle builder to base", builderOrders);
	};

	// 1) конструкторы: каждый строит своё здание, по нужде экономики
	const auto& factoryData = unitsData->getStaticUnitData (kIdFactory);
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

		if (isStuck (v)) continue; // заперт без пути — его стройку не ждать

		auto it = plans_.find (v.getId());
		if (it != plans_.end() && !planValid (it->second))
		{
			plans_.erase (it); // площадку заняли
			it = plans_.end();
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
			const sSite site = findMineSite (model, *player, grid, reserved, needOil);
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
			if (site.valid()) wants.push_back (kIdMine);
			if (consumersOk && nFactory >= 1 && nFactory < factoryTarget) wants.push_back (kIdFactory);
			if (consumersOk && nFactory >= 3 && econ.metalMaxProd >= 20 && totalOf (kIdHabitat) < 1) wants.push_back (kIdHabitat);
			if (consumersOk && totalOf (kIdHabitat) >= 1 && totalOf (kIdResearch) < 1) wants.push_back (kIdResearch);
			sID what;
			cPosition pos (-1, -1);
			for (const sID& w : wants)
			{
				pos = w == kIdMine ? site.pos : findBaseSpot (model, *player, main, unitsData->getStaticUnitData (w), reserved);
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
			executePlan (v, makePlan (v, what, pos, what == kIdFactory && countBuildings (*player, kIdFactory) == 0));
			continue;
		}
		executePlan (v, it->second);
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

		// а) энергии не хватает уже на построенное и строящееся — генератор (2 нефти)
		const auto e = energyState();
		const int capNow = e[1] * 6 + e[2];
		if (!energyFeasible (e[0], e[1], e[2], econ.oilFree)
			&& energyFeasible (std::min (e[0], capNow + 1), e[1], e[2] + 1, econ.oilFree))
		{
			const cPosition pos = findBaseSpot (model, *player, main, unitsData->getStaticUnitData (kIdEnergySmall), reservedFootprints());
			if (pos.x() >= 0)
			{
				executePlan (v, makePlan (v, kIdEnergySmall, pos, false));
				continue;
			}
		}

		// б) соединители до своего здания вне главной подбазы (шахта на ресурсе)
		std::vector<cPosition> path;
		for (const auto& b : player->getBuildings())
		{
			if (!b->getStaticData().connectsToBase || b->subBase == main) continue;
			cPosition side;
			if (chainTo (grid, b->getPosition(), b->getIsBig(), &side) < 1) continue;
			auto p = connectorPath (grid, side);
			if (!p.empty() && (path.empty() || p.size() < path.size())) path = std::move (p);
		}
		for (const auto& ov : player->getVehicles()) // большое здание строится вдали от базы
		{
			if (!ov->isUnitBuildingABuilding() || !ov->getIsBig()) continue;
			cPosition side;
			if (chainTo (grid, ov->getPosition(), true, &side) < 1) continue;
			auto p = connectorPath (grid, side);
			if (!p.empty() && (path.empty() || p.size() < path.size())) path = std::move (p);
		}
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
				pushMove (v, view, path[0], playerId, "op", "to connector path", builderOrders);
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
			const cPosition pos = findBaseSpot (model, *player, main, unitsData->getStaticUnitData (kIdStorageMetal), reservedFootprints());
			if (pos.x() >= 0)
			{
				executePlan (v, makePlan (v, kIdStorageMetal, pos, false));
				continue;
			}
		}
		idleToBase (v);
	}

	// 3) металл хода: сначала стройщикам (под их стройку), остаток — фабрикам
	int budget = econ.free;
	int storedLeft = econ.metalStored;
	for (const auto& r : refills)
	{
		const int give = std::min ({r.want, budget, storedLeft});
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
			if (b.getStaticUnitData().ID != kIdFactory || b.subBase != main || !b.isUnitWorking()) continue;
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
		if (bp->getStaticUnitData().ID == kIdFactory && bp->subBase == main && !bp->isBuildListEmpty()
			&& bp->getBuildListItem (0).getRemainingMetal() > 0 && !bp->isUnitWorking())
			allWorking = false;
	const bool speedUp = richNow && allWorking;
	const int nKonstrukt = countVehicles (*player, kIdKonstrukt);
	const int konstruktTarget = econ.metalMaxProd >= 20 ? 3 : 2;
	int konstruktQueued = 0;
	for (const auto& b : player->getBuildings())
		if (b->getStaticUnitData().ID == kIdFactory && !b->isBuildListEmpty()
			&& b->getBuildListItem (0).getType() == kIdKonstrukt)
			++konstruktQueued;

	for (const auto& bp : player->getBuildings())
	{
		const cBuilding& b = *bp;
		if (b.getStaticUnitData().ID != kIdFactory || b.isDisabled()) continue;
		if (main == nullptr || b.subBase != main) continue; // вне главной базы металла нет
		if (std::find (stopped.begin(), stopped.end(), b.getId()) != stopped.end()) continue;

		if (b.isBuildListEmpty())
		{
			const int speed = (speedUp && budget >= factoryMetalPerRound (b, 1)) ? 1 : 0;
			cOrder o;
			o.type = eOrderType::ChangeBuildList;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.buildSpeed = speed;
			if (nKonstrukt + konstruktQueued < konstruktTarget)
			{
				o.buildList = {{kIdKonstrukt.firstPart, kIdKonstrukt.secondPart}};
				o.repeat = false;
				o.reason = "produce constructor";
				++konstruktQueued;
			}
			else
			{
				o.buildList = {{kIdTank.firstPart, kIdTank.secondPart}};
				o.repeat = true;
				o.reason = "produce tanks";
			}
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
			// готовый юнит — выезд; при повторе движок сам запускает следующее задание
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
			if (b.getRepeatBuild() && budget >= mpr)
			{
				budget -= mpr;
				energyStarted += b.getStaticUnitData().needsEnergy;
			}
			continue;
		}

		if (b.isUnitWorking())
		{
			// металл копится — ускорить работающую фабрику
			if (speedUp && b.getBuildSpeed() == 0)
			{
				const int extra = factoryMetalPerRound (b, 1) - b.getMetalPerRound();
				if (budget >= extra)
				{
					cOrder o;
					o.type = eOrderType::ChangeBuildList;
					o.playerId = playerId;
					o.unitId = b.getId();
					for (size_t i = 0; i < b.getBuildListSize(); ++i)
						o.buildList.push_back ({b.getBuildListItem (i).getType().firstPart, b.getBuildListItem (i).getType().secondPart});
					o.buildSpeed = 1;
					o.repeat = b.getRepeatBuild();
					o.level = "op";
					o.reason = "speed up factory (metal piles up)";
					prodOrders.push_back (std::move (o));
					budget -= extra;
				}
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
			for (size_t i = 0; i < b.getBuildListSize(); ++i)
				o.buildList.push_back ({b.getBuildListItem (i).getType().firstPart, b.getBuildListItem (i).getType().secondPart});
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
		for (const auto& bp : player->getBuildings())
		{
			const cBuilding& b = *bp;
			if (b.subBase != main || b.isDisabled() || b.isUnitWorking() || !b.buildingCanBeStarted()) continue;
			const auto& sd = b.getStaticUnitData();
			const bool isMine = sd.buildingData.canMineMaxRes > 0;
			const bool isResearch = sd.buildingData.canResearch;
			if (!isMine && !isResearch) continue;
			if (sd.needsHumans > humanFree) continue;
			if (!energyFeasible (energyNeed + sd.needsEnergy, econ.stations, econ.generators, econ.oil)) continue;
			cOrder o;
			o.type = eOrderType::StartWork;
			o.playerId = playerId;
			o.unitId = b.getId();
			o.level = "op";
			o.reason = isMine ? "start mine" : "start research";
			out.push_back (std::move (o));
			energyNeed += sd.needsEnergy;
			humanFree -= sd.needsHumans;
		}
	}
}

//------------------------------------------------------------------------------
void cBaseRuleBrain::microDecisions (int playerId, const cModel& model, std::vector<cOrder>& out)
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
		if (std::find (opMoved_.begin(), opMoved_.end(), vehicle->getId()) != opMoved_.end()) continue; // отогнан с площадки

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
					o.position = o.path.back(); // цель пути — для журнала и дневника
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
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = "approach enemy";
					out.push_back (std::move (o));
					continue;
				}
			}

			// 4) врагов не видно — выдвигаемся к ближайшей неразведанной клетке
			//    (разведка боем; на симметричной карте армии встретятся и вступят в бой)
			{
				auto path = pathToUnexplored (*vehicle, view, scoutCache);
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = "advance (scout)";
					out.push_back (std::move (o));
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
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = "approach damaged unit";
					out.push_back (std::move (o));
					break;
				}
			}
			continue;
		}

		// --- разведчик ---
		// Он один и заменить его нечем (малой фабрики правила не строят): сначала
		// разведать ресурсы у своей базы (там ставятся шахты), от врагов — назад.
		if (sd.vehicleData.canSurvey)
		{
			const cPosition landing = player->getLandingPos();
			bool danger = false;
			for (const auto& e : enemies)
				if (e.damage > 0 && (e.pos - vehicle->getPosition()).l2NormSquared() <= kSurveyorDanger * kSurveyorDanger)
					danger = true;
			if (danger && (vehicle->getPosition() - landing).l2NormSquared() > 4)
			{
				cPathCalculator pc (*vehicle, view, landing, false);
				auto path = pc.calcPath();
				if (!path.empty())
				{
					cOrder o;
					o.type = eOrderType::Move;
					o.playerId = playerId;
					o.unitId = vehicle->getId();
					o.path = pathToVector (path);
					o.position = o.path.back(); // цель пути — для журнала и дневника
					o.level = "micro";
					o.reason = "surveyor away from enemy";
					out.push_back (std::move (o));
				}
				continue;
			}
			auto path = pathToUnexplored (*vehicle, view, scoutCache, &landing, kSurveyorHome);
			if (path.empty()) path = pathToUnexplored (*vehicle, view, scoutCache);
			if (!path.empty())
			{
				cOrder o;
				o.type = eOrderType::Move;
				o.playerId = playerId;
				o.unitId = vehicle->getId();
				o.path = pathToVector (path);
				o.position = o.path.back(); // цель пути — для журнала и дневника
				o.level = "micro";
				o.reason = "survey unexplored";
				out.push_back (std::move (o));
			}
			continue;
		}

		// конструкторы и прочие — оперативка (этап 4)
	}
}

} // namespace ai
