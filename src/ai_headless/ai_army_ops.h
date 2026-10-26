/***************************************************************************
 * ai_army_ops.h — операции армии (шаг 8 PLAN.md).
 *
 * Операция атаки — автомат состояний, живущий между ходами в объекте мозга:
 *   Wait    — ждать: копить силы у точки сбора (у своего края, вне угроз),
 *             разведка 1–2 машинами вместо всей армии;
 *   Gather  — собраться: цель есть и сил хватает — все к точке сбора,
 *             отставших ждать ограниченно;
 *   Advance — идти: лидер (самый медленный, затем ближайший к цели) шагает по
 *             клеткам с меньшей угрозой, только когда группа подтянулась;
 *             ведомые — в свободные клетки рядом с точкой лидера;
 *   Attack  — бить: у края зоны огня врага, вся группа входит в неё в один ход,
 *             когда сила ≥ нужной; бой в радиусе — микро правил (фокус-огонь);
 *   Search  — искать: цели на месте нет — ближайшая другая, иначе осмотреться;
 *   Retreat — отход к точке сбора, когда сила стала меньше нужной (потери).
 * Цель — из памяти шага 7: шахты, затем ключевые здания, иначе армия врага.
 * Сила — по Ланчестеру: sqrt(урон группы за ход × её hp) против того же у
 * защитников цели (память и видимые), с запасом. Резерв обороны у базы и
 * разведчики — роли вне операций. Каждое решение — событие журнала (level
 * "operational"); состояние — toJson().
 *
 * Вход для директивы советника (шаг 13): sArmyDirective — цель / собрать / отойти.
 * Всё упорядочено по id и обходу карты — детерминированно.
 *
 * Шаг 9 — роли (ai_roles): в операциях — строй (танки) и поддержка (штурмовые
 * орудия, артиллерия, ракеты); лидер — танк; поддержка идёт в хвосте строя, а в
 * бою встаёт вне «удара с подъезда» врага (дальность + сколько он проедет с
 * выстрелом) так, чтобы бить с предельной дальности (findStandoff), и стреляет в
 * конце пути, если выстрел остался; танк, доехав до цели в дальности, — тоже.
 * Разведка — разведчиками малой фабрики (нет их — танками, как в шаге 8);
 * разведчики сверх нужных — корректировщики: держат врага в обзоре для
 * артиллерии, сами вне угроз.
 ***************************************************************************/

#ifndef ai_headless_ai_army_opsH
#define ai_headless_ai_army_opsH

#include "ai_types.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

class cMapView;
class cModel;
class cPlayer;
class cVehicle;

namespace ai
{

class cRoleBook;
class cWorldModel;
struct sEnemyMemory;
enum class eRole;

// Шаг 9: клетка хода для машины поддержки (и корректировщика) — см. findStandoff
struct sStandoff
{
	std::vector<cPosition> path; // пусто — стоять
	unsigned int attackId = kNoUnit; // цель выстрела в конце пути (шаг 13: kNoUnit — без выстрела)
	cPosition cell {-1, -1};
	int danger = 0;              // 0 — вне «удара с подъезда» врага, 1 — в нём, 2 — в его зоне огня
	bool inRange = false;        // с клетки цель ближе ring
};

// Лучшая клетка хода машины v: опасность не выше maxDanger (иначе — наименьшая), с неё
// цель из targets ближе ring (иначе — ближе к этому кольцу), при fire — выстрел в конце
// пути, если после движения выстрел останется (cVehicle::DecSpeed); затем — меньше пути.
// Кассетная машина не выбирает цель, у которой свои ближе 2 клеток (бьёт площадь и своих).
sStandoff findStandoff (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& v,
	const std::vector<const sEnemyMemory*>& targets, int ring, bool fire, int maxDanger, const std::vector<cPosition>& reserved);
// цель выстрела в конце пути path (в дальности последней клетки, выстрел остался); kNoUnit — нет
unsigned int endMoveTarget (const cModel& model, const cVehicle& v, const std::vector<cPosition>& path,
	const std::vector<const sEnemyMemory*>& targets);
// видимые враги, по которым машина v может стрелять (здания-мины и соединители — нет)
std::vector<const sEnemyMemory*> attackableTargets (const cModel& model, const cWorldModel& world, const cVehicle& v);
// сколько выстрелов останется после движения ценой cost очков хода
int shotsAfterMove (const cVehicle& v, int cost);
// допустимая опасность клетки машины поддержки (findStandoff, maxDanger): штурмовому орудию — 1, прочим — 0
int supportMaxDanger (eRole r);

enum class eOpState
{
	Wait,
	Gather,
	Advance,
	Search,
	Attack,
	Retreat
};
const char* opStateName (eOpState s);

enum class eOpTarget
{
	None,
	Building, // здание врага из памяти
	Army,     // скопление вооружённых машин врага из памяти
	Point     // клетка (директива или осмотр места)
};

// директива операциям извне (будущий советник, шаг 13)
struct sArmyDirective
{
	enum class eKind
	{
		None,
		Target,  // цель у клетки pos
		Gather,  // точка сбора — pos
		Retreat  // все операции в поход/бою — отойти (разово)
	};
	eKind kind = eKind::None;
	cPosition pos {-1, -1};
};

// задание машине на ход от операции: путь (пусто — стоять) и причина для журнала
struct sOpTask
{
	std::vector<cPosition> path;
	std::string reason;
	unsigned int attackId = kNoUnit; // шаг 9: выстрел в конце пути («подъехал — выстрелил»); шаг 13: kNoUnit — нет
};

struct sOperation
{
	int id = 0;
	eOpState state = eOpState::Wait;
	int since = 0;    // ход входа в состояние
	int created = 0;
	eOpTarget targetKind = eOpTarget::None;
	unsigned int targetId = 0;
	cPosition target {-1, -1};
	int targetClass = -1; // 0 шахта, 1 ключевое здание, 2 прочее здание, 3 армия, 4 клетка
	cPosition rally {-1, -1};
	unsigned int leader = 0;
	std::vector<unsigned int> members; // по возрастанию id
	int force = 0; // sqrt(урон за ход × hp) группы
	int need = 0;  // нужно на цель с обороной (с запасом)
	int launchMembers = 0;
	int launchForce = 0;
	cPosition searchFrom {-1, -1}; // где искали (Search)

	nlohmann::json toJson() const;
};

class cArmyOps
{
public:
	struct sContext
	{
		const cModel* model = nullptr;
		const cPlayer* player = nullptr;
		const cMapView* view = nullptr;
		const cWorldModel* world = nullptr;
		int turn = 0;
		std::vector<const cVehicle*> fighters; // свои боевые наземные машины ролей строя и поддержки
		std::vector<const cVehicle*> scoutUnits; // шаг 9: разведчики (роль Scout) — разведка и корректировка
		const cRoleBook* roles = nullptr;      // шаг 9: роли машин
	};

	// начало хода: состав, роли (резерв, разведка), цели, силы, переходы состояний
	void refresh (const sContext& ctx);
	// машина в операции, которая в походе, бою или отходе (тревога у базы её не берёт)
	bool committed (unsigned int id) const;
	// задания на ход машинам без приказа (busy — уже получили приказ: огонь, отход, тревога)
	void plan (const sContext& ctx, const std::vector<unsigned int>& busy, std::map<unsigned int, sOpTask>& tasks);

	void setDirective (const sArmyDirective& d);
	std::vector<nlohmann::json> takeEvents();
	size_t pendingEvents() const { return events_.size(); }
	nlohmann::json toJson() const;
	const std::vector<sOperation>& operations() const { return ops_; }
	// шаг 9: куда держаться сопровождению (ПВО, ремонтник): лидер операции в походе или
	// бою, иначе точка сбора формирующейся, иначе база; рядом с армией ли
	cPosition escortAnchor (bool* withArmy = nullptr) const;
	// точка сбора формирующейся операции (нет — база)
	cPosition rallyPoint() const;

private:
	struct sForce
	{
		long long dmg = 0; // урон за ход
		long long hp = 0;
		int armor = 0;     // броня (наибольшая) — для урона по ним
		int count = 0;
		int value() const;
	};

	void buildLandDist (const sContext& ctx);
	int landDist (const cPosition& p) const;
	bool reachableNear (const cPosition& p, bool big, int* dist = nullptr) const;
	sForce defenseAt (const sContext& ctx, const cPosition& p, int radius) const;
	sForce forceOf (const sContext& ctx, const std::vector<unsigned int>& ids, int enemyArmor) const;
	int needOf (const sForce& def) const;
	// withTotal — для выхода: нужное и не меньше доли всей известной армии врага
	void evaluate (const sContext& ctx, sOperation& op, bool withTotal = false);
	bool chooseTarget (const sContext& ctx, sOperation& op, const sForce* extra, bool feasibleOnly, const cPosition* near, int nearRadius);
	bool targetValid (const sContext& ctx, sOperation& op) const;
	cPosition computeRally (const sContext& ctx, const cPosition& dest) const;
	void setState (sOperation& op, eOpState s, int turn, const char* why);
	void noteRetreat (const sOperation& op);
	void event (const char* what, const sOperation& op, nlohmann::json extra = {});
	unsigned int chooseLeader (const sContext& ctx, const sOperation& op) const;
	void updateRoles (const sContext& ctx, std::vector<unsigned int>& pool);
	sOperation* forming();

	void planFormation (const sContext& ctx, const sOperation& op, const cPosition& center, bool holdNear,
		const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks,
		const std::string& reason, bool fireEdge, int pacePct);
	// шаг 13 (п. 77 PLAN.md): строй без A* на каждую машину. Дальние от центра машины идут по одному
	// полю расстояний до центра (Дейкстра от центра, шаги в обратную сторону — цена шага к центру,
	// машина-образец — с самой узкой проходимостью); ближние — коротким поиском в окне у центра.
	// Поле строится до тех пор, пока все дальние машины не окажутся рядом с посчитанной клеткой.
	void buildFormationField (const sContext& ctx, const cPosition& center, const std::vector<const cVehicle*>& far);
	// путь машины v по полю: первый шаг — в соседнюю посчитанную клетку, дальше — к центру, пока не
	// войдёт в окно (Чебышёв ≤ window от центра) или не исчерпает ход с запасом; пусто — поле её не достало
	std::vector<cPosition> descendFormationField (const sContext& ctx, const cVehicle& v, const cPosition& center, int window) const;
	// короткий путь машины v из from в to внутри окна (Чебышёв ≤ window от center); пусто — нет
	std::vector<cPosition> localPath (const sContext& ctx, const cVehicle& v, const cPosition& from, const cPosition& to,
		const cPosition& center, int window) const;
	void planAdvance (const sContext& ctx, sOperation& op, const std::vector<unsigned int>& busy,
		std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks);
	void planAttack (const sContext& ctx, sOperation& op, const std::vector<unsigned int>& busy,
		std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks);
	void planScouts (const sContext& ctx, const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved,
		std::map<unsigned int, sOpTask>& tasks);
	// шаг 9: корректировщики при операции в походе или бою (иначе — в разведку)
	void planSpotters (const sContext& ctx, const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved,
		std::map<unsigned int, sOpTask>& tasks);
	// шаг 9: машина поддержки — клетка вне «удара с подъезда» врага, бить с предельной дальности
	bool planSupport (const sContext& ctx, const cVehicle& v, const std::vector<const sEnemyMemory*>& foes,
		std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks, const std::string& tag);
	bool isSupport (unsigned int id) const;
	eRole roleOfId (unsigned int id) const;
	void planReserve (const sContext& ctx, const std::vector<unsigned int>& busy, std::vector<cPosition>& reserved,
		std::map<unsigned int, sOpTask>& tasks);
	// формирующаяся операция: враг у точки сбора — к краю его зоны огня (как одиночки шага 7)
	bool planDefense (const sContext& ctx, const sOperation& op, const std::vector<unsigned int>& busy,
		std::vector<cPosition>& reserved, std::map<unsigned int, sOpTask>& tasks);

	std::vector<sOperation> ops_;     // по id
	std::vector<unsigned int> scouts_;  // разведчики (вне операций)
	std::vector<unsigned int> spotters_; // шаг 9: корректировщики — разведчики сверх нужных
	std::vector<unsigned int> reserve_; // резерв обороны у базы
	std::map<unsigned int, const cVehicle*> scoutById_; // шаг 9: машины-разведчики (роль Scout)
	std::map<unsigned int, int> roleById_; // шаг 9: роль своих машин (eRole) — для строя и лидера
	int nextId_ = 1;
	int cooldownUntil_ = 0; // после отхода — не выходить раньше (если сил не стало больше)
	int cooldownForce_ = 0;
	sArmyDirective directive_;
	std::vector<nlohmann::json> events_;
	// за ход: расстояние по суше от базы (×2: прямо 2, по диагонали 3; −1 — не дойти) и откуда пришли
	int W_ = 0;
	int H_ = 0;
	cPosition home_ {-1, -1};
	std::vector<int> landDist_;
	std::vector<int> landPrev_;
	// шаг 13: поле строя (buildFormationField) — буферы на всю карту заводятся раз, клетка годна,
	// если её метка == поколению поля
	std::vector<int> fieldDist_;
	std::vector<int> fieldNext_;
	std::vector<unsigned int> fieldSeen_;
	std::vector<unsigned int> fieldClosed_;
	unsigned int fieldGen_ = 0;
	int fieldClass_ = 0; // класс движения образца поля (биты: наземная 1, вода 2, берег 4)
	// клетки у своих зданий (кольцо выезда фабрик, площадки стройки) — строю там не стоять
	std::vector<char> avoid_;
	bool avoided (const cPosition& p) const
	{
		if (p.x() < 0 || p.y() < 0 || p.x() >= W_ || p.y() >= H_ || avoid_.empty()) return false;
		return avoid_[static_cast<size_t> (p.y()) * W_ + p.x()] != 0;
	}
	int ourArmor_ = 0;
	sForce totalDef_; // вся известная вооружённая наземная армия врага (память; с пиком — см. armyPeak_)
	// пик известной армии врага: память машин — 8 ходов, а армия за туманом не исчезает;
	// пик тает на 1% за ход, новое наблюдение больше пика — новый пик
	double peakDmg_ = 0;
	double peakHp_ = 0;
	std::map<unsigned int, const cVehicle*> fighterById_;
};

} // namespace ai

#endif // ai_headless_ai_army_opsH
