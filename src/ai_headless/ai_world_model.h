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
	Reach,
	Strike // шаг 9: «удар с подъезда» (дальность + проезд с выстрелом) видимых врагов и зданий
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
	// шаг 9: для ролей и «удара с подъезда»
	int shotsMax = 0;       // выстрелов за ход по данным
	int scan = 0;
	bool driveFire = false; // стреляет на ходу без потери выстрелов
	int moveFire = 0;       // клеток, которые проедет за свой ход и ещё выстрелит (у зданий 0)

	bool armed() const { return damage > 0 && shots > 0 && range > 0 && canAttack != 0; }
	// «удар с подъезда»: докуда достанет выстрелом в свой ход, подъехав (дальность + moveFire)
	int strike() const { return range + moveFire; }
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
	// шаг 9: «удар с подъезда» — клетка, куда видимый (сейчас или прошлым ходом) враг или
	// здание выстрелит в свой ход, подъехав и сохранив выстрел (дальность + moveFire), — по наземной машине
	int groundStrike (const cPosition& p) const { return at (strike_, p); }
	// угроза для конкретной машины (наземная или воздушная карта; у воздушной
	// зоны огня нет — берётся «дальность + ход»)
	int threatFor (const cVehicle& v, const cPosition& p, eThreatLayer layer = eThreatLayer::Reach) const;
	// путь (без стартовой клетки) проходит через клетку под угрозой для v
	bool pathUnsafe (const cVehicle& v, const std::vector<cPosition>& path, eThreatLayer layer = eThreatLayer::Reach) const;

	// Безопасный путь до dest: A* со штрафом kThreatStepPenalty за шаг в клетку
	// под угрозой. Пусто — пути нет (или dest == старт).
	// Шаг 13: maxExpand > 0 — не больше стольких клеток поиска (иначе пусто): строй не ищет по всей
	// области машину, запертую машинами.
	std::vector<cPosition> safePath (const cVehicle& v, const cMapView& view, const cPosition& dest,
		eThreatLayer layer = eThreatLayer::Reach, int maxExpand = 0) const;
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

	// Шаг 13 (п. 77 PLAN.md): область проходимости клетки p для класса движения машины v — связные
	// клетки, куда такая машина может встать без учёта машин (суша, берег, вода, мосты и платформы,
	// как cMap::possiblePlaceVehicle; с запасом — лишнее связывает, но не рвёт). Разные области —
	// пути нет вовсе: поиск пути не обходит всю область впустую. −1 — клетка непроходима для класса
	// (или вне карты); у самолётов — 0 (всё связано). Пересчёт — раз в ход, по запросу класса.
	int regionOf (const cVehicle& v, const cPosition& p) const;
	// размер области (клеток) по номеру regionOf для класса машины v; 0 — нет такой
	int regionSize (const cVehicle& v, int region) const;
	// то же для класса «танк» (наземная, не берег и не вода) — для анализа карты стратегией (П3)
	int tankRegionOf (const cPosition& p) const;
	int tankRegionSize (int region) const;

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

	// шаг 13: области проходимости по классу движения (ключ — биты: наземная 1, вода 2, берег 4)
	struct sRegions
	{
		long long bridges = -1; // подпись мостов и платформ на воде и берегу, с которой посчитано
		std::vector<int> label;
		std::vector<int> sizes;
	};
	// подпись клеток воды и берега со зданием, по которому едет наземная машина (мост, платформа,
	// дорога) — области пересчитываются, только когда она меняется (местность статична)
	long long bridgeSign_ = 0;
	const sRegions& regions (int cls) const;
	static int moveClass (const cVehicle& v);
	mutable std::map<int, sRegions> regions_;
	const cMap* map_ = nullptr; // карта модели этого хода (update)
	// шаг 13: буферы A* — общие на все вызовы (без массивов на всю карту на каждый вызов): клетка
	// годна в поиске, если её метка == поколению поиска; куча — вектор с push_heap/pop_heap в том же
	// порядке, что прежняя std::priority_queue (пути те же)
	mutable std::vector<int> pathG_;
	mutable std::vector<int> pathPrev_;
	mutable std::vector<unsigned int> pathSeen_;
	mutable std::vector<unsigned int> pathClosed_;
	mutable unsigned int pathGen_ = 0;
	mutable std::vector<std::pair<int, int>> pathHeap_;
	unsigned int beginPathSearch (size_t n) const;

	std::map<unsigned int, sEnemyMemory> memory_; // по id — детерминированно
	int turn_ = 0;
	int w_ = 0;
	int h_ = 0;
	std::vector<int> ground_;
	std::vector<int> air_;
	std::vector<int> fire_;
	std::vector<int> strike_; // шаг 9: дальность + подъезд с выстрелом (видимые и здания)
	std::vector<int> seen_; // ход последнего наблюдения клетки (−1 — не видели)
};

} // namespace ai

#endif // ai_headless_ai_world_modelH
