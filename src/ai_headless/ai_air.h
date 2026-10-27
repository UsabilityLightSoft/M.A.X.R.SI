/***************************************************************************
 * ai_air.h — авиация ИИИ (П15–П17, П10; шаг 13 PLAN.md, часть 3). Решения простые и пишутся в журнал
 * (событие «air»): из чего выбирали и почему — потом это возьмёт Модель (шаги 18–19).
 *
 * Программа (простой вариант; сколько металла в авиацию — решает Модель):
 *   «strike» — авиазавод, ангар, AWAC, бомбардировщики (и авиатранспорт для коммандос): враг только за водой
 *              без моря у базы (П3, упор «воздух» — иначе его не достать), экономика поднялась (добыча и ход),
 *              или подлодки врага у нашей верфи (бомбардировщик бьёт подлодку — флаг AreaSub);
 *   «none»   — авиации нет; видна авиация врага — истребители (если авиазавод есть или строится).
 * Самолёты садятся только в ангар (ремонт и боезапас за металл подбазы); в воздухе их бьют только ПВО и
 * истребители. Опасность клетки — известные зенитки (здания и наземные машины, что бьют воздух, из памяти;
 * выключенные — не в счёт) с запасом в клетку; бомбардировщик и AWAC не заходят в неё.
 *
 *   AWAC (П15)        — облетает базу врага по кольцу вне ПВО (обзор 12 > досягаемость зенитки 8): у базы врага
 *                       — по 4 клетки за заход, чтобы зенитку увидеть раньше, чем войти в её огонь; ищет стороны
 *                       без ПВО (по 8 сторонам — ценность целей, до которых есть клетка удара вне ПВО), в налёт —
 *                       висит над стороной удара; П25: разведка AWAC, а не скаутом под ракетными пушками;
 *   бомбардировщик (П16) — цель, у которой есть клетка удара вне ПВО, а округа видна недавно (неизвестной
 *                       зенитки там нет): подлодки и корабли врага у нашего берега и верфи (п. 100), шахты,
 *                       электростанции, заводы, радары; «ударил — ушёл»: подлёт и выстрел в конце пути, второй
 *                       выстрел в следующем заходе, затем отход вне ПВО и обзора;
 *   налёт «AWAC + коммандос + бомбардировщики» — ценная цель прикрыта одной зениткой: коммандос (ai_commando)
 *                       подходит к ней незаметно, в ход удара выключает её в первом заходе, бомбардировщики бьют
 *                       следом, AWAC смотрит; неудача коммандос — удар отменяется;
 *   истребитель (П10) — охота на авиацию врага вне его ПВО: бомбардировщики, транспорты, AWAC; выстрел в конце
 *                       подлёта и уход; база — зенитками (П5), истребители — охотники; дальность — за золото;
 *   авиатранспорт (П17) — коммандос к далёкой цели (тренировка на чужих, налёт, диверсия): подлететь, взять,
 *                       высадить вне ПВО у цели (выгрузка не тратит ход коммандос), вернуться; груз ценится как
 *                       сумма — в зону ПВО не летит. Раненых к ремонту и зенитки на угрожаемую сторону — варианты
 *                       для Модели (в журнал — почему не выбраны).
 * Всё детерминированно: обход по id и клеткам.
 ***************************************************************************/

#ifndef ai_headless_ai_airH
#define ai_headless_ai_airH

#include "ai_types.h"
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
class cUnit;
class cVehicle;

namespace ai
{

class cBaseDefense;
class cCommandos;
class cComposition;
class cNavy;
class cRoleBook;
class cWorldModel;
struct sEnemyMemory;

class cAirForce
{
public:
	enum class eProgram
	{
		None,
		Strike
	};

	// раз в ход, после обороны и флота: программа, нужды авиазавода и ангара, картина ПВО и авиации врага
	void update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, const cNavy& navy, const cCommandos& commandos, int income, int factories, int turn);

	eProgram program() const { return program_; }
	const char* programName() const;
	bool wantAirFactory() const { return wantFactory_; }
	bool wantHangar() const { return wantHangar_; }
	// производство авиазавода: следующий самолёт (sID() — ничего); пишет в журнал, из чего выбирали
	sID nextPlane (const cModel& model, const cPlayer& player, const cRoleBook& book, cComposition& comp, const std::string& buildAs);
	// срочно (истребители против видимой авиации, бомбардировщики против подлодок у верфи)
	bool urgent() const { return urgent_; }
	// авиазавод и ангар — раньше шахт: подлодки у нашей верфи (п. 100) или упор «воздух» уверенно
	bool factoryUrgent() const { return factoryUrgent_; }

	// микро, заход 1: самолёты, ангар, налёт с коммандос
	void plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, cCommandos& commandos, std::vector<cOrder>& out);
	// заходы 2+: второй выстрел, отход, этапы налёта, высадка коммандос
	std::vector<cOrder> planPass (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
		const cRoleBook& roles, cCommandos& commandos, int pass);
	bool wantsPass() const { return wantPass_; }
	// самолёт под командой авиации — микро правил его не трогает
	bool claimed (unsigned int id) const { return claimed_.count (id) != 0; }

	std::vector<nlohmann::json> takeEvents();
	nlohmann::json toJson() const;

	struct sCounts
	{
		int awacs = 0, bombers = 0, fighters = 0, transports = 0, other = 0;
		int planeMetal = 0;
		int airFactories = 0, hangars = 0;
		int strikes = 0;       // ударов бомбардировщиков за партию (приказов)
		int hits = 0;          // попаданий (по итогу)
		int kills = 0;         // уничтожено целей
		int subStrikes = 0;    // ударов по подлодкам
		int intercepts = 0;    // атак истребителей
		int raids = 0;         // налётов с коммандос (ход удара)
		int raidDisables = 0;  // зенитка выключена в ход удара
		int ferries = 0;       // высадок коммандос с авиатранспорта
		int aaFerries = 0;     // зениток поставлено транспортом на угрожаемую сторону (П17)
		int lost = 0;          // потеряно самолётов
		int enemyAa = 0;       // известных зениток врага (здания и машины)
		int enemyAir = 0;      // металла в авиации врага (память)
	};
	const sCounts& counts() const { return counts_; }

private:
	struct sAaSource
	{
		cPosition pos;
		int reach = 0;      // дальность + ход (у здания — дальность), с запасом в клетку
		bool big = false;
		unsigned int id = 0;
		bool building = false;
	};
	struct sShot
	{
		unsigned int unit = 0;
		unsigned int target = 0;
		std::string type;
		int hpBefore = 0;
		std::string what;
		int turn = 0;
	};
	struct sStrike
	{
		unsigned int target = kNoUnit;
		int turn = -1;
		int pass = 0;
		bool retreat = false;
	};
	enum class eRaid
	{
		Idle,
		Prepare, // коммандос идёт к зенитке, бомбардировщики — на стоянку
		Strike   // ход удара: выключить → бить → добить → уйти
	};
	struct sRaid
	{
		eRaid state = eRaid::Idle;
		unsigned int target = kNoUnit;  // что бомбить (здание)
		unsigned int aa = kNoUnit;      // какую зенитку выключить
		cPosition targetPos {-1, -1};
		cPosition aaPos {-1, -1};
		cPosition fire {-1, -1};        // клетка удара (вне ПВО, когда зенитка выключена)
		cPosition staging {-1, -1};     // стоянка бомбардировщиков (вне ПВО и обзора)
		int since = 0;
		int phase = 0;                  // ход удара: 1 — выключить, 2 — бить, 3 — добить, 4 — уйти
		int strikeTurn = -1;
		int tries = 0;
	};
	struct sFerry
	{
		unsigned int transport = kNoUnit;
		unsigned int commando = kNoUnit;
		cPosition dest {-1, -1};
		cPosition drop {-1, -1};
		int state = 0; // 0 — лететь к коммандос, 1 — с грузом к высадке, 2 — домой
		int since = 0;
	};

	void event (const char* what, nlohmann::json extra);
	void checkShots (const cModel& model);
	// известные зенитки врага (не выключенные): клетка опасна для самолёта, если в досягаемости (+1)
	void buildAa (const cModel& model, const cPlayer& player, const cWorldModel& world);
	bool aaDanger (const cPosition& p, unsigned int ignore = kNoUnit) const;
	// округа клетки (радиус 9 — досягаемость зенитки + 1) видна не раньше kSeenFresh ходов назад: неизвестной зенитки нет
	bool seenAround (const cWorldModel& world, const cPosition& p) const;
	// путь самолёта (обрезан до хода); safe — без клеток в досягаемости ПВО (кроме ignore); stepCap — не больше клеток
	std::vector<cPosition> flyPath (const cVehicle& v, const cMapView& view, const cPosition& dest, bool safe, unsigned int ignore,
		int stepCap) const;
	bool pushMove (const cVehicle& v, std::vector<cPosition> path, const std::string& reason, std::vector<cOrder>& out,
		unsigned int target = kNoUnit, eMoveEnd end = eMoveEnd::Attack);
	bool reservedCell (const cPosition& p) const;
	// безопасная клетка рядом с anchor (кольца до r), свободная для самолёта; (−1,−1) — нет
	cPosition safeNear (const cMapView& view, const cVehicle& v, const cPosition& anchor, int r, bool farFromEnemy) const;
	// цель удара бомбардировщика: ценность (0 — не цель)
	int strikeValue (const cModel& model, const sEnemyMemory& m) const;
	// лучший удар для бомбардировщика b: цель (id из памяти) и клетка; ignoreAa — выключенная зенитка налёта
	bool pickStrike (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& b, unsigned int& target,
		cPosition& fire, int& value, unsigned int ignoreAa, bool onlyNaval) const;
	bool strike (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& b, std::vector<cOrder>& out,
		int pass, unsigned int ignoreAa, bool onlyNaval, const char* why);
	bool fireNow (const cModel& model, const cMapView& view, const cVehicle& v, unsigned int target, std::vector<cOrder>& out,
		const char* why);
	bool retreat (const cModel& model, const cMapView& view, const cVehicle& v, std::vector<cOrder>& out, const char* why);
	bool service (const cModel& model, const cPlayer& player, const cMapView& view, const cVehicle& v, std::vector<cOrder>& out);
	void awacPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& a,
		std::vector<cOrder>& out, int pass);
	void fighterPlan (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& f, std::vector<cOrder>& out,
		int pass);
	void ferryPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, cCommandos& commandos,
		std::vector<cOrder>& out, int pass);
	// П17: зенитка на угрожаемую сторону — true, если транспорт занят этим
	bool aaFerry (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cVehicle& t, std::vector<cOrder>& out, int pass);
	struct sAaFerry
	{
		unsigned int aa = kNoUnit;
		cPosition dest {-1, -1};
		int state = 0; // 0 — нет, 1 — к зенитке, 2 — с зениткой к месту
		int since = 0;
	};
	sAaFerry aaFerry_;
	const cRoleBook* roles_ = nullptr;
	void raidPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, cCommandos& commandos,
		std::vector<const cVehicle*>& bombers, std::vector<cOrder>& out, int pass);
	// сторона базы врага (0..7) — ценность целей с клеткой удара вне ПВО (журнал AWAC)
	void scoreSides (const cModel& model, const cWorldModel& world);
	const cBuilding* hangar (const cModel& model) const;

	int turn_ = 0;
	int playerId_ = -1;
	int W_ = 0;
	int H_ = 0;
	eProgram program_ = eProgram::None;
	std::string why_;
	bool fightersWanted_ = false;
	int fighterWant_ = 0;
	bool wantFactory_ = false;
	bool wantHangar_ = false;
	bool urgent_ = false;
	bool factoryUrgent_ = false;
	int strikeSince_ = -1; // ход включения программы «удар» (авиазавод раньше шахт, если его всё нет)
	bool antiSub_ = false;
	int antiSubUntil_ = -1;
	int bomberWant_ = 0;
	bool transportWant_ = false;
	cPosition home_ {-1, -1};
	cPosition focus_ {-1, -1};       // база врага: середина виденных зданий, иначе — напротив нас
	bool focusKnown_ = false;
	cPosition yard_ {-1, -1};        // своя верфь (подлодки у неё — цель бомбардировщиков)
	std::vector<unsigned int> hangars_;
	std::vector<sAaSource> aa_;
	std::vector<int> sideScore_;     // ценность сторон базы врага (по 8 направлениям), −1 — сторона в ПВО
	int bestSide_ = -1;
	int lastBestSide_ = -2;
	std::map<unsigned int, sStrike> strikes_;   // бомбардировщик → удар этого хода
	std::map<unsigned int, int> awacRing_;      // AWAC → номер точки облёта
	std::set<unsigned int> serviceSet_;         // самолёты на обслуживании (в ангаре или к нему)
	std::set<unsigned int> known_;              // свои самолёты (гибель — в журнал)
	std::map<unsigned int, std::string> typeOf_;
	std::vector<sShot> shots_;
	sRaid raid_;
	sFerry ferry_;
	std::set<unsigned int> claimed_;
	std::vector<cPosition> reserved_;
	bool wantPass_ = false;
	sCounts counts_;
	std::vector<nlohmann::json> events_;
	std::string lastProgramWhy_;
	int lastFighterWant_ = -1;
	// правка 1: уничтоженные нашим огнём цели (память врага обновляется только в начале хода); где погиб свой самолёт — опасно
	std::set<unsigned int> deadTargets_;
	std::map<unsigned int, cPosition> lastPos_;          // последняя клетка своих самолётов
	std::vector<std::pair<cPosition, int>> deathZones_; // клетка гибели и ход
	std::vector<int> lossTurns_;                        // ходы потерь самолётов
	int pauseUntil_ = -1;                               // производство самолётов на паузе до хода
	bool deathZone (const cPosition& p) const;
	// правка 2: армия врага (вооружённые машины, виденные за 3 хода) — с ней ходит мобильная ПВО: самолёту в обход
	std::vector<char> armyDanger_;
	bool armyNear (const cPosition& p) const
	{
		if (armyDanger_.empty() || p.x() < 0 || p.y() < 0 || p.x() >= W_ || p.y() >= H_) return false;
		return armyDanger_[static_cast<size_t> (p.y()) * W_ + p.x()] != 0;
	}
	// округа клетки видна не раньше fresh ходов назад (выборка клеток с шагом 2)
	bool seenWithin (const cWorldModel& world, const cPosition& p, int r, int fresh) const;
	// зенитки и места гибели — сетка на ход (aaDanger без исключения — по ней)
	std::vector<char> aaGrid_;
	void buildAaGrid();
	// цель из памяти ещё годна: не уничтожена нашим огнём; клетка видна — и она там видна
	bool targetAlive (const cModel& model, const cPlayer& player, const sEnemyMemory& m) const;
};

} // namespace ai

#endif // ai_headless_ai_airH
