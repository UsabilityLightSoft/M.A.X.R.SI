/***************************************************************************
 * ai_world_model.h — память о враге и карта угроз (шаг 7 PLAN.md).
 *
 * Память: по id вражеского юнита — тип, последняя известная клетка, ход и
 * боевые характеристики. Видимость честная — то, что игрок видит в тумане
 * войны (cPlayer::canSeeUnit / canSeeAt), как у digest. Здания помнятся, пока
 * не увидим их клетку пустой; машины — забываются через kForgetVehicleTurns
 * ходов без наблюдения или когда их последняя клетка видна, а их там нет.
 *
 * Карта угроз: по клеткам — ожидаемый урон врага за его ход по нашей наземной
 * машине (и отдельно — по воздушной, для шага 9): каждый вооружённый враг из
 * памяти бьёт клетки в пределах «дальность + ход», урон за выстрел с поправкой
 * на броню (max(1, урон − броня), как cUnit::calcHealth) × выстрелы.
 *
 * Безопасный путь: A* по клеткам, как cPathCalculator, но каждый шаг в клетку
 * под угрозой стоит дороже — путь обходит опасное, если есть иначе.
 *
 * Состояние живёт в объекте мозга (у каждого игрока свой), всё упорядочено по
 * id и обходу карты — детерминированно.
 ***************************************************************************/

#ifndef ai_headless_ai_world_modelH
#define ai_headless_ai_world_modelH

#include "utility/position.h"

#include <map>
#include <vector>

class cMap;
class cMapView;
class cModel;
class cPlayer;
class cVehicle;

namespace ai
{

// какой слой угроз смотреть: никакой, зона огня с места или «дальность + ход»
enum class eThreatLayer
{
	None,
	Fire,
	Reach
};

// что помним о вражеском юните
struct sEnemyMemory
{
	unsigned int id = 0;
	int owner = -1;
	int typeFirst = 0;
	int typeSecond = 0;
	bool building = false;
	bool big = false;
	bool plane = false;     // летает (бьётся только ПВО)
	bool ship = false;      // плавает
	cPosition pos;          // последняя известная клетка (левый верхний угол здания)
	int seenTurn = -1;      // ход, когда видели последний раз
	bool visible = false;   // виден в этот ход
	int damage = 0;
	int shots = 0;          // выстрелов за ход (с поправкой на боезапас при последнем наблюдении)
	int range = 0;
	int armor = 0;
	int hp = 0;
	int hpMax = 0;
	int speed = 0;          // клеток за ход (speedMax / 4), у зданий 0
	int canAttack = 0;      // eTerrainFlag: по кому бьёт

	bool armed() const { return damage > 0 && shots > 0 && range > 0 && canAttack != 0; }
	// бьёт по наземным машинам
	bool hitsGround() const { return armed() && (canAttack & 4) != 0; }
	// бьёт по воздушным машинам
	bool hitsAir() const { return armed() && (canAttack & 1) != 0; }
	// ожидаемый урон за ход по цели с бронёй armor
	int damageVs (int targetArmor) const;
	// «дальность + ход»: докуда достанет за свой ход
	int reach() const { return range + speed; }
};

class cWorldModel
{
public:
	// машину, не виденную столько ходов, забываем (подобрано турниром: 4 и 8 — в пределах шума)
	static constexpr int kForgetVehicleTurns = 8;
	// штраф пути за шаг в клетку под угрозой (обычный шаг по суше — 4)
	static constexpr int kThreatStepPenalty = 40;

	// обновить память по честной видимости игрока self на ход turn
	void update (const cModel& model, const cPlayer& self, int turn);

	// карта угроз по памяти: armorGround / armorAir — броня эталонной наземной
	// и воздушной машины игрока
	void buildThreat (const cMap& map, int armorGround, int armorAir);

	int groundThreat (const cPosition& p) const { return at (ground_, p); }
	int airThreat (const cPosition& p) const { return at (air_, p); }
	// зона огня с места: клетка в дальности вооружённого врага, видимого сейчас
	// или прошлым ходом (без его хода), — по наземной машине
	int groundFire (const cPosition& p) const { return at (fire_, p); }
	// угроза для конкретной машины (наземная или воздушная карта; у воздушной
	// зоны огня нет — берётся «дальность + ход»)
	int threatFor (const cVehicle& v, const cPosition& p, eThreatLayer layer = eThreatLayer::Reach) const;
	// путь (без стартовой клетки) проходит через клетку под угрозой для v
	bool pathUnsafe (const cVehicle& v, const std::vector<cPosition>& path, eThreatLayer layer = eThreatLayer::Reach) const;

	// Безопасный путь до dest: A* со штрафом kThreatStepPenalty за шаг в клетку
	// под угрозой. Пусто — пути нет (или dest == старт).
	std::vector<cPosition> safePath (const cVehicle& v, const cMapView& view, const cPosition& dest,
		eThreatLayer layer = eThreatLayer::Reach) const;
	// То же, но до любой клетки не дальше nearDist (евклидово) от target, куда можно
	// встать (шаг 8: поход операции к цели — здание занято, встать у него). Пусто —
	// пути нет или старт уже у цели.
	std::vector<cPosition> safePathNear (const cVehicle& v, const cMapView& view, const cPosition& target, int nearDist,
		eThreatLayer layer = eThreatLayer::Reach) const;

	// ход, когда клетку последний раз видели (шаг 8: разведка 1–2 машинами); −1 — не видели
	int seenTurn (const cPosition& p) const
	{
		if (p.x() < 0 || p.y() < 0 || p.x() >= w_ || p.y() >= h_ || seen_.empty()) return -1;
		return seen_[static_cast<size_t> (p.y()) * w_ + p.x()];
	}

	// Ближайшая к near клетка без угрозы для v, куда можно встать (кольца по
	// Чебышёву до maxR; при равной дальности — ближе к prefer). false — нет.
	bool nearestSafeCell (const cVehicle& v, const cMapView& view, const cPosition& near, int maxR,
		const cPosition& prefer, cPosition& out, eThreatLayer layer = eThreatLayer::Reach) const;

	const std::map<unsigned int, sEnemyMemory>& enemies() const { return memory_; }
	int turn() const { return turn_; }
	// сколько ходов назад видели (0 — виден сейчас)
	int age (const sEnemyMemory& m) const { return turn_ - m.seenTurn; }

private:
	int at (const std::vector<int>& grid, const cPosition& p) const
	{
		if (p.x() < 0 || p.y() < 0 || p.x() >= w_ || p.y() >= h_) return 0;
		return grid[static_cast<size_t> (p.y()) * w_ + p.x()];
	}

	std::map<unsigned int, sEnemyMemory> memory_; // по id — детерминированно
	int turn_ = 0;
	int w_ = 0;
	int h_ = 0;
	std::vector<int> ground_;
	std::vector<int> air_;
	std::vector<int> fire_;
	std::vector<int> seen_; // ход последнего наблюдения клетки (−1 — не видели)
};

} // namespace ai

#endif // ai_headless_ai_world_modelH
