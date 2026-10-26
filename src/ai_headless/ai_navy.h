/***************************************************************************
 * ai_navy.h — флот ИИИ (П11–П14, шаг 13 PLAN.md, часть 2). Решения простые и пишутся в журнал
 * (событие «navy»): что выбирали и почему — потом это возьмёт Модель (шаги 18–19).
 *
 * Программа флота — по анализу карты (П3, ai_defense) и угрозе с моря:
 *   «sea»   — враг только за водой (или догадка: наша суша мала, море рядом): верфь сразу после
 *             первой фабрики, флот ради высадки — корвет, канонерки, морской транспорт, подлодка…;
 *   «coast» — упор «суша», но рядом море: верфь из лишнего металла (ур. 2) или в ответ на корабли и
 *             верфь врага; флот малый — корвет у своего берега, канонерки — если с нашей воды видны
 *             здания врага или есть его корабли;
 *   «none»  — моря у базы нет или флот не нужен.
 * Верфь и док — 2×2 на воде у базы, связаны с ней соединителями (место ищет оперативка правил,
 * ai_rule_brain). Охрана стройки верфи (П13): 2 бойца у конструктора, что её строит; готовую верфь
 * стерегут корвет и ракетные пушки берега (П25, ai_defense).
 *
 * Корабли (роли — ai_roles, по способностям):
 *   корвет — только он видит подлодки (и БМП под водой): половина — у верфи и платформ (шаг на клетку
 *            через ход: обнаружение идёт только при шаге), прочие — охрана транспорта;
 *   канонерка, ракетный корабль — бьют берег: цель из памяти (здание у воды), клетка в дальности вне
 *            «удара с подъезда» врага; свой обзор у них 4, поэтому корректировщик — подлодка (обзор 7,
 *            скрыта) у цели; при высадке — охрана транспорта;
 *   подлодка — охота на корабли, транспорты, верфь и постройки на воде врага (выстрел раскрывает её —
 *            только по таким целям), иначе корректировка или разведка моря;
 *   морской транспорт — высадка (П14): берёт до 6 бойцов у своего берега, плывёт к берегу земли врага
 *            вне его зоны огня, с охраной (груз ценится как сумма: без охраны дорогой груз не плывёт),
 *            выгружает (выгрузка не тратит ход машин) и возвращается за следующей волной;
 *   раненый корабль — в док (ремонт за металл подбазы) или к своей верфи.
 * Высаженные бойцы — на чужой суше: операции армии их не берут (у тех путь только по своей суше),
 * воюют по общим правилам микро (сближение с видимым врагом, разведка).
 * Всё детерминированно: обход по id и клеткам.
 ***************************************************************************/

#ifndef ai_headless_ai_navyH
#define ai_headless_ai_navyH

#include "ai_types.h"
#include "ai_world_model.h"
#include "game/data/units/id.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

class cBuilding;
class cMapView;
class cModel;
class cPlayer;
class cVehicle;

namespace ai
{

class cBaseDefense;
class cComposition;
class cRoleBook;
class cWorldModel;
struct sEnemyMemory;
enum class eRole;

class cNavy
{
public:
	enum class eMode
	{
		None,
		Coast,
		Sea
	};

	// раз в ход, после обороны (П3 — упор по карте): программа флота, нужды верфи и дока, охрана стройки
	void update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, bool rich, int income, int turn);

	eMode mode() const { return mode_; }
	const char* modeName() const;
	// стройка (оперативка правил): верфь (П11), раньше шахт — при упоре «море»; док (П14)
	bool wantShipyard() const { return wantShipyard_; }
	// верфь — после шахт и фабрик (опыт: верфь на 36-м ходу на Gen_Archipelago задержала шахты, доход вдвое ниже)
	bool shipyardEarly() const { return false; }
	bool wantDock() const { return wantDock_; }
	// водоём, где ставить верфь и док (номер — cBaseDefense::seaLabelOf)
	int homeSea() const { return homeSea_; }
	// оперативка запланировала верфь: клетка стройки — охранять (П13)
	void noteShipyardSite (const cPosition& p);
	cPosition guardPoint() const { return guardPoint_; }
	int guardsWanted() const { return guardsWanted_; }

	// производство верфи: следующий корабль (sID() — ничего не нужно); пишет в журнал, из чего выбирали
	sID nextShip (const cModel& model, const cPlayer& player, const cRoleBook& book, cComposition& comp, const std::string& buildAs);

	// микро хода (заход 1): приказы кораблям, доку и десанту — в out; busy — машины, уже получившие
	// приказ (огонь); armyFree — свободные бойцы армии на своей суше (кандидаты в десант)
	void plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, const std::vector<unsigned int>& busy, const std::vector<const cVehicle*>& armyFree,
		std::vector<cOrder>& out);
	// заходы 2+: выгрузка, если транспорт доплыл; второй выстрел подлодки по кораблю
	std::vector<cOrder> planPass (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles, int pass);
	bool wantsPass() const { return wantPass_; }
	// машина под командой флота (корабль, груз на пути к транспорту и в нём) — армия и микро её не трогают
	bool claimed (unsigned int id) const { return claimed_.count (id) != 0; }
	// высаженная машина (на чужой суше), врагов не видно: куда идти — к ближайшему виденному зданию врага на её
	// суше, иначе к невиданной клетке у фокуса врага (разведка); false — некуда
	// explore — разведчик (ищет невиданное у фокуса врага); боец — только к «мягкой» цели (здание без вооружённых врагов в
	// 10 клетках), иначе держит берег (goal == своя клетка)
	bool landedGoal (const cMapView& view, const cWorldModel& world, const cVehicle& v, cPosition& goal, bool explore);
	cPosition enemyFocus() const { return enemyFocus_; }
	// точка сбора армии (операции) — причал транспорта ищется у неё
	void setRally (const cPosition& p) { rally_ = p; }

	std::vector<nlohmann::json> takeEvents();
	nlohmann::json toJson() const;

	struct sCounts
	{
		int corvettes = 0, guns = 0, subs = 0, transports = 0, other = 0;
		int fleetMetal = 0;
		int shipyards = 0, docks = 0;
		int cargo = 0;     // машин в транспорте и идущих к нему
		int landed = 0;    // высажено машин за партию
		int waves = 0;     // волн высадки за партию
		int reconWaves = 0; // волн разведки высадкой за партию
		int tankWaves = 0;  // волн десанта бойцами за партию
		int bombardOrders = 0, subOrders = 0, corvetteOrders = 0, dockOrders = 0;
	};
	const sCounts& counts() const { return counts_; }
	const char* landingStateName() const;

private:
	enum class eLand
	{
		Idle,
		Board,
		Sail,
		Unload,
		Return
	};
	struct sLanding
	{
		eLand state = eLand::Idle;
		unsigned int transport = 0;
		std::vector<unsigned int> cargo; // выбранные машины (в транспорте и идущие к нему), по возрастанию id
		cPosition pickup {-1, -1};
		cPosition beach {-1, -1};
		cPosition target {-1, -1};       // точка на суше врага, к которой высаживаемся
		int targetRegion = -1;
		int since = 0;
		int beachFails = 0;
		int beachDanger = 0;              // 0 — вне досягаемости врага и нет сильнее груза рядом; 1 — досягаемость; 2 — зона огня
		int waitNoted = -100;
		bool escortWaitNoted = false;
		cPosition moveEnd {-1, -1};      // куда транспорт плывёт в этот ход (для охраны)
		bool recon = false;              // волна-разведка: разведчики ищут базу врага (её ещё не видели)
		bool beachStale = false;         // лучший берег давно не видели — сначала посмотреть (подлодка)
		int lastUnload = -100;           // ход последней выгрузки (срок «выгрузить не удаётся» — от неё)
	};

	void event (const char* what, nlohmann::json extra);
	void setLanding (eLand s, const char* why, nlohmann::json extra = {});
	// клетка, где транспорт берёт груз: вода или берег у своей суши, ближе к центру базы
	cPosition choosePickup (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
		const cBaseDefense& defense, const cVehicle& t, const cPosition& near) const;
	// берег высадки: вода или берег у суши области цели, вне зоны огня врага, ближе к цели
	// recon — врага ещё не видели: разведка высадкой (разведчики) у области суши напротив нас
	bool chooseBeach (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& t, bool recon);
	// выгрузить груз на сушу области цели у транспорта; сколько приказов
	int unload (const cModel& model, const cVehicle& t, std::vector<cOrder>& out, const char* why);
	void landingPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, const std::vector<const cVehicle*>& transports, const std::vector<unsigned int>& busy,
		const std::vector<const cVehicle*>& armyFree, int escortsReady, std::vector<cOrder>& out);
	// приказ движения по пути (обрезан до хода), конец пути — в занятые клетки
	bool pushPath (const cVehicle& v, const cMapView& view, std::vector<cPosition> path, const char* reason, std::vector<cOrder>& out,
		unsigned int target = kNoUnit);
	// длинный путь до клетки (A* правил мимо угроз слоя), обрезанный до хода
	bool moveTo (const cVehicle& v, const cMapView& view, const cWorldModel& world, const cPosition& dest, eThreatLayer layer, const char* reason,
		std::vector<cOrder>& out);
	// свободная клетка рядом с anchor (кольца rmin…rmax), куда машина v встанет, ближе к v; (−1,−1) — нет
	cPosition freeNear (const cMapView& view, const cWorldModel& world, const cVehicle& v, const cPosition& anchor, int rmin, int rmax,
		int maxDanger) const;
	bool reservedCell (const cPosition& p) const;
	// опасность клетки воды для корабля: 2 — в дальности вооружённого врага сейчас, 1 — достанет за свой ход
	// (дальность + подъезд с выстрелом), 0 — нет. Наземный враг (танк) бьёт корабль, только если с суши в его
	// дальности есть клетка (у ракет 10 — далеко от берега, у танка 4 — у самого берега): угрозы правил считают
	// подъезд и по воде, для корабля это лишнее — канонерка у берега врага стояла бы без дела
	int seaDanger (const cPosition& c) const;
	void buildThreats (const cModel& model, const cWorldModel& world);
	struct sThreat
	{
		cPosition pos;
		bool big = false;
		bool landBound = false;
		int fire = 0;
		int reach = 0;
	};
	std::vector<sThreat> threats_;
	std::vector<int> landDist_; // до ближайшей суши, где встанет танк (Чебышёв, до 16), — раз за партию

	int turn_ = 0;
	int playerId_ = -1;
	int W_ = 0;
	int H_ = 0;
	eMode mode_ = eMode::None;
	std::string modeWhy_;
	int coastSince_ = -1000; // ход входа в «берег» по лишнему металлу — держать 20 ходов (без мигания)
	std::vector<cPosition> unloadSpots_; // где транспорт выгружал на чужой суше (последние 4): высаженные там не стоят
	void noteUnloadSpot (const cPosition& p);
	bool modeLogged_ = false;
	bool wantShipyard_ = false;
	bool wantDock_ = false;
	int homeSea_ = -1;
	int homeRegion_ = -1;
	cPosition baseCenter_ {-1, -1};
	cPosition rally_ {-1, -1};
	cPosition enemyFocus_ {-1, -1};       // где искать врага: середина его виденных зданий, иначе — напротив нас
	bool enemyFocusKnown_ = false;
	std::vector<unsigned int> shipyards_; // свои верфи (id)
	std::vector<unsigned int> docks_;
	cPosition yardPos_ {-1, -1};           // своя верфь (левый верхний угол), иначе (−1,−1)
	cPosition guardPoint_ {-1, -1};
	int guardsWanted_ = 0;
	int enemySubs_ = 0;      // подлодок врага в памяти (не старше 20 ходов)
	int enemyShipMetal_ = 0; // металла в вооружённых кораблях врага (память)
	int coastTargets_ = 0;   // зданий врага, до которых с воды нашего моря достанет канонерка
	std::string lastPick_;
	sLanding landing_;
	std::set<unsigned int> claimed_;
	std::vector<cPosition> reserved_;    // концы путей этого хода — двум кораблям в одну клетку не идти
	std::map<unsigned int, cPosition> scoutGoal_; // подлодка → клетка разведки
	unsigned int strikeTarget_ = 0;       // цель обстрела (id из памяти), 0 — нет
	int strikeUnsafe_ = 0;                // ходов подряд у цели нет безопасной клетки — после 3 сменить
	std::map<unsigned int, cPosition> landedGoal_; // высаженная машина → клетка разведки
	cPosition strikePos_ {-1, -1};
	bool wantPass_ = false;
	sCounts counts_;
	std::vector<nlohmann::json> events_;
};

} // namespace ai

#endif // ai_headless_ai_navyH
