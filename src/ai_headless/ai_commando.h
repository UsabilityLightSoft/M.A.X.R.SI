/***************************************************************************
 * ai_commando.h — свои коммандос ИИИ (П2, П18; шаг 13 PLAN.md, часть 3). Решения простые и пишутся в
 * журнал (событие «commando»): что выбирали и почему — потом это возьмёт Модель (шаги 18–19).
 *
 * Механика (proposals_2026-10-26.md, «Проверено в движке», «Скрытность»; cCommandoData, cActionStealDisable):
 *  - коммандос действует только с соседней клетки, одна попытка за ход (тратит выстрел), стреляет на ходу —
 *    подошёл в заходе 1, действует в заходе 2;
 *  - шанс выключить вчетверо выше, чем захватить; растёт со званием (опыт — только от удачного выключения),
 *    падает с ценой цели; потолок 90%;
 *  - неудача у цели с хозяином — коммандос обнаружен, если хозяин видит его клетку; у ничейной техники
 *    чужих — тревоги нет. Ничейная техника не просыпается вовсе (срок выключения убывает только в начале хода
 *    игрока-хозяина), а захватить её можно и выключенной — она «целая» у нового хозяина;
 *  - видят коммандос только пехота и коммандос врага (обзор 4) — при шаге в их радиусе.
 *
 * Задачи коммандос (по приоритету):
 *  1. обнаружен — уйти из зоны огня и подальше от врага; со следующего своего хода шаг из клетки вне радиуса
 *     сыщиков снимает обнаружение;
 *  2. налёт (ai_air, «AWAC + коммандос + бомбардировщики»): подойти незаметно к зенитке на стороне удара и
 *     ждать рядом; в ход удара — выключить её в первом заходе, бомбардировщики бьют следом;
 *  3. чужие (Владыка: «новичков выгодно тренировать на технике чужих: неудача не поднимает тревогу»): сначала
 *     выключить каждую машину места (опыт), потом захватывать выключенные (захват — без риска, раз в ход);
 *  4. диверсия (П18, обученный — звание ≥ 2): выключить электростанцию, шахту, завод, радар, зенитку врага,
 *     если рядом с целью нет известной пехоты и коммандос врага и шанс высок (неудача — тревога);
 *  5. без дела — ждать у центра базы.
 * Пути — в обход радиуса сыщиков врага (пехота, коммандос из памяти: обзор + 1). Далеко (другая суша или путь
 * дальше 25 клеток) — просьба авиатранспорту (ai_air), иначе БМП (под водой её видит только корвет).
 * Всё детерминированно: обход по id и клеткам.
 ***************************************************************************/

#ifndef ai_headless_ai_commandoH
#define ai_headless_ai_commandoH

#include "ai_types.h"
#include "game/data/units/id.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

class cMapView;
class cModel;
class cPlayer;
class cUnit;
class cVehicle;

namespace ai
{

class cBaseDefense;
class cRoleBook;
class cWorldModel;

class cCommandos
{
public:
	// раз в ход, после обороны: память о чужих, сыщики врага, нужды (учебный центр, сколько коммандос, БМП)
	void update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, int income, int factories, int turn);

	// стройка и производство (оперативка правил)
	bool wantTrainingHall() const { return wantTraining_; }
	int commandosWanted() const { return wanted_; }
	int apcWanted() const { return apcWanted_; }
	// после отказа от тренировки (нет учебного центра) — для журнала
	const std::string& programWhy() const { return why_; }

	// микро, заход 1: приказы коммандос и БМП — в out; busy — машины, уже получившие приказ
	void plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cBaseDefense& defense, std::vector<cOrder>& out);
	// заходы 2+: выключить или захватить (коммандос подошёл в прошлом заходе)
	std::vector<cOrder> planPass (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, int pass);
	bool wantsPass() const { return wantPass_; }
	// коммандос и БМП под командой модуля — микро правил их не трогает
	bool claimed (unsigned int id) const { return claimed_.count (id) != 0; }

	// --- налёт (ai_air) ---
	// зенитка (или иное здание) g, которую надо выключить перед ударом: коммандос подходит и ждёт рядом
	void requestRaid (unsigned int target, const cPosition& pos, bool big, int turn);
	void cancelRaid (const char* why);
	unsigned int raidTarget() const { return raid_.target; }
	// коммандос налёта рядом с целью, скрыт, с выстрелом
	bool raidReady (const cModel& model) const;
	unsigned int raidCommando() const { return raid_.commando; }
	// до ближайшего своего коммандос (не в пути на транспорте, не обнаружен), Чебышёв; нет — очень много
	int nearestFree (const cModel& model, const cWorldModel& world, const cPosition& p) const;
	// приказ выключить цель налёта (в этом заходе); false — не может
	bool raidDisable (const cModel& model, const cPlayer& player, std::vector<cOrder>& out, int pass);

	// --- перевозка (ai_air: авиатранспорт) ---
	struct sFerry
	{
		unsigned int commando = kNoUnit;
		cPosition from {-1, -1};
		cPosition dest {-1, -1}; // клетка, у которой высадить (цель — рядом)
		const char* why = "";
		bool stealthOnly = false; // к врагу — только БМП под водой (авиатранспорт над его базой сбивают)
	};
	// коммандос, которым далеко идти (по id); авиатранспорт берёт первую
	const std::vector<sFerry>& ferryRequests() const { return ferry_; }
	// коммандос в пути на авиатранспорте (не трогать)
	void noteFerried (unsigned int commando, bool on);
	// есть авиатранспорт (ai_air) или БМП: чужие и цели на другой суше тоже годны
	void setFerryAvailable (bool air) { ferryAvailableAir_ = air; ferryAvailable_ = air || (counts_.apcs > 0 && turn_ >= apcStuckUntil_); }
	// клетка в радиусе известных сыщиков врага (пехота, коммандос): высаживать туда нельзя
	bool detectorNear (const cPosition& p) const { return inDetectorZone (p); }

	std::vector<nlohmann::json> takeEvents();
	nlohmann::json toJson() const;

	struct sCounts
	{
		int commandos = 0;
		int apcs = 0;
		int aliensKnown = 0;   // ничейных машин чужих в памяти
		int aliensFree = 0;    // из них не выключены
		int disables = 0;      // удачных выключений за партию
		int captures = 0;      // захватов за партию
		int fails = 0;         // неудач за партию
		int detected = 0;      // раз обнаружен (после своей неудачи или у сыщика)
		int lost = 0;          // погибло коммандос
		int sabotage = 0;      // выключено зданий врага за партию
		int maxRank = 0;
		int apcLandings = 0;   // высадок коммандос с БМП за партию
	};
	const sCounts& counts() const { return counts_; }

private:
	enum class eTask
	{
		Idle,
		Escape,
		Raid,
		Alien,
		Sabotage
	};
	struct sAlien
	{
		unsigned int id = 0;
		sID type;
		cPosition pos {-1, -1};
		bool disabled = false;
		int cost = 0;
		bool plane = false;
		bool ship = false;
		int seenTurn = -1;
	};
	struct sDetector
	{
		cPosition pos;
		int radius = 0; // обзор + 1
	};
	struct sTask
	{
		eTask task = eTask::Idle;
		unsigned int target = kNoUnit;
		cPosition goal {-1, -1};
		int since = 0;
		std::string why;
		std::vector<cPosition> route; // маршрут к goal (без старта) — держаться его, пока он годен
	};
	// попытка в этом ходу: что сделал, шанс — итог смотрим в следующий раз
	struct sTry
	{
		unsigned int commando = 0;
		unsigned int target = 0;
		bool steal = false;
		int chance = 0;
		int successBefore = 0; // удач коммандос до попытки: выросло — выключение удалось
		bool neutral = false;
		std::string type;
		int turn = 0;
	};
	struct sRaid
	{
		unsigned int target = kNoUnit;
		cPosition pos {-1, -1};
		bool big = false;
		unsigned int commando = kNoUnit;
		int since = 0;
	};

	void event (const char* what, nlohmann::json extra);
	void checkTries (const cModel& model, const cPlayer& player);
	bool inDetectorZone (const cPosition& p) const;
	// клетка рядом с юнитом u (8 соседей его клеток), куда коммандос встанет, вне радиуса сыщиков; ближе к from
	cPosition besideCell (const cMapView& view, const cVehicle& v, const cUnit& u, const cPosition& from) const;
	struct sTask;
	// то же, но клетка держится между ходами в t.goal, пока годна (без хождения туда-обратно)
	cPosition stickyBeside (const cMapView& view, const cVehicle& v, const cUnit& u, sTask& t) const;
	// путь к dest по маршруту задачи: коммандос на маршруте и ближайшие шаги свободны и вне радиуса сыщиков — остаток
	// маршрута; иначе — новый (stealth — в обход сыщиков, иначе travelPath) и он запоминается
	std::vector<cPosition> routeTo (const cModel& model, const cMapView& view, const cVehicle& c, const cPosition& dest, sTask& t,
		bool stealth) const;
	// путь коммандос в обход радиуса сыщиков (A*, окно вокруг старта и цели); пусто — нет
	std::vector<cPosition> stealthPath (const cModel& model, const cMapView& view, const cVehicle& v, const cPosition& dest, int maxExpand) const;
	bool pushPath (const cVehicle& v, const cMapView& view, std::vector<cPosition> path, const std::string& reason, std::vector<cOrder>& out);
	// дальний путь: у маршрута сыщики врага — в обход, иначе путь движка
	std::vector<cPosition> travelPath (const cModel& model, const cMapView& view, const cVehicle& c, const cPosition& dest) const;
	// клетка в kAlienEnemyGap от известного здания врага
	bool nearEnemy (const cPosition& p) const;
	bool act (const cModel& model, const cPlayer& player, const cVehicle& c, const cUnit& target, bool steal, const std::string& why,
		std::vector<cOrder>& out, int pass);
	void planOne (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& c,
		std::vector<cOrder>& out, int pass);
	// лучшая работа у чужих для коммандос c: машина-цель (выключить или захватить), иначе kNoUnit
	unsigned int pickAlien (const cModel& model, const cMapView& view, const cWorldModel& world, const cVehicle& c, bool& steal,
		bool ferry) const;
	unsigned int pickSabotage (const cModel& model, const cPlayer& player, const cWorldModel& world, const cVehicle& c) const;
	bool escape (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cVehicle& c,
		std::vector<cOrder>& out);
	void apcPlan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		std::vector<cOrder>& out);

	int turn_ = 0;
	int playerId_ = -1;
	int W_ = 0;
	int H_ = 0;
	cPosition home_ {-1, -1};
	int homeRegion_ = -1;
	std::map<unsigned int, sAlien> aliens_;
	std::vector<sDetector> detectors_;
	std::vector<cPosition> enemyBuildings_;
	mutable std::map<long long, int> blocked_;            // клетки, где путь коммандос упёрся в невидимое (клетка → ход)
	std::map<unsigned int, cPosition> lastPos_;          // последняя клетка своих коммандос (на земле)
	std::vector<std::pair<cPosition, int>> deaths_;      // где и когда погиб свой коммандос
	std::map<unsigned int, sTask> tasks_;
	std::map<unsigned int, int> rankSeen_;      // коммандос → звание (для журнала при росте)
	std::set<unsigned int> known_;              // свои коммандос, которых видели (гибель — в журнал)
	std::set<unsigned int> ferried_;            // в пути на авиатранспорте (или ждёт его на месте)
	std::set<unsigned int> freshDrop_;          // высажен в этом ходу — ход цел
	std::vector<sFerry> ferry_;
	std::vector<sTry> tries_;
	std::map<unsigned int, unsigned int> pending_; // коммандос → цель, к которой подошёл в заходе (действие — в следующем)
	std::map<unsigned int, bool> pendingSteal_;
	std::set<unsigned int> badTargets_;          // цели без подхода (окружены, вне досягаемости)
	sRaid raid_;
	bool wantTraining_ = false;
	int wanted_ = 0;
	int apcWanted_ = 0;
	std::string why_;
	std::string lastWhy_;
	int lastWanted_ = -1;
	bool wantPass_ = false;
	bool ferryAvailable_ = false;
	bool ferryAvailableAir_ = false;
	bool wantOverWater_ = false; // есть чужие за водой (пешком не дойти) — БМП
	bool enemyOverWater_ = false; // база врага только за водой (П3) — диверсанту туда на БМП
	struct sApcFerry
	{
		unsigned int apc = kNoUnit;
		unsigned int commando = kNoUnit;
		cPosition dest {-1, -1};
		cPosition landing {-1, -1};
		cPosition meet {-1, -1};   // встреча с коммандос (БМП в тесноте базы к нему не подойти)
		int state = 0; // 0 — взять коммандос, 1 — везти к берегу, 2 — выгрузить где стоит
		int since = 0;
	};
	sApcFerry apc_;
	int apcStuckUntil_ = -1; // БМП не смогла взять коммандос (заперта своими) — без поездок до этого хода
	std::set<unsigned int> claimed_;
	std::vector<cPosition> reserved_;
	sCounts counts_;
	std::vector<nlohmann::json> events_;
};

} // namespace ai

#endif // ai_headless_ai_commandoH
