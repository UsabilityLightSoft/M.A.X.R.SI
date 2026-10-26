/***************************************************************************
 * ai_red_brain.h — сценарный противник «красная команда-1» (шаг 14, П23).
 *
 * Мозг «red1» — не игрок на победу, а налётчик по сценарию: проверка обороны ИИИ
 * за минуты, а не за долгий турнир. Сценарий — переменная MAXR_RED:
 *  - rear  — налёт с тыла: AWAC облетает базу врага и ищет сторону без ПВО; авиатранспорт
 *            высаживает коммандос у радара или зенитки, тот выключает её; бомбардировщики
 *            со стоянки на этой стороне бьют шахты, электростанции, соединители и уходят;
 *  - sea   — высадка с моря: морской транспорт берёт танки и везёт к берегу у базы,
 *            канонерки обстреливают берег, танки выгружаются и атакуют;
 *  - subs  — подлодки: охота на корабли, транспорты, верфь и док врага у его берега;
 *  - combo — комбинированный удар по заходам одного хода (замечание Владыки к П23):
 *            коммандос выключил зенитку → бомбардировщик разбил ракетную установку (или
 *            самое опасное орудие) → танки подъехали и атаковали; танки к сбору у базы врага
 *            привозит второй авиатранспорт (П20: не тратить ход танков на переезд).
 * Машины сценария покупаются при высадке (redLandingUnits; ai_headless_main — как у
 * «scenario»); экономика — MAXR_RED_ECO: none (умолчание — машины сценария и стартовая
 * шахта, все потери врага — от налёта) или base2 (замороженные правила шага 10 ведут
 * остальное хозяйство, приказы машинам налёта отбрасываются). MAXR_RED_START — ход, с
 * которого машины налёта выходят (умолчание 1).
 * Знание: где база врага — точка его высадки (облегчение налётчику); цели и ПВО — только
 * видимые его глазами и память о них. Итоги этапов — события «red_stage» в journal.jsonl
 * (stage, ok, подробности); удары и попадания — «red_step». Сводку партий собирает
 * TargetFiles/tools/red_run.py. Отчёт — TargetFiles/research/rules/14_red_team.md.
 *
 * Два режима (шаг 14б, Владыка 26.10.2026: «тесты нужны честные»):
 *  - «red1» — краш-тест: всё выше (покупка самолётов, кораблей и коммандос при высадке — человек
 *    так не может; знает точку высадки врага; без перезарядки);
 *  - «red2» — честный соперник (ai_red_fair.cpp): при высадке — только то, что окно высадки продаёт
 *    человеку (наземные машины без пехоты: конструктор, инженер, скаут, танки) на те же 150 кредитов;
 *    хозяйство — замороженные правила base2 (MAXR_RED_ECO другим быть не может); свой конструктор строит
 *    авиазавод, ангар, жилой блок и учебный центр (rear, combo) или верфь (sea, subs), инженер тянет к
 *    ним соединители; заводы выпускают машины сценария на металле базы; база врага неизвестна — её ищут
 *    скаут и AWAC; уничтоженное вне обзора — не знает; самолёты перезаряжаются и чинятся в ангаре.
 *    Сценарий начинается, когда база врага найдена и отряд собран (и не раньше MAXR_RED_START).
 ***************************************************************************/

#ifndef ai_headless_ai_red_brainH
#define ai_headless_ai_red_brainH

#include "ai_brain.h"
#include "ai_types.h"

#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

class cModel;
class cMapView;
class cPlayer;
class cUnit;
class cVehicle;

namespace ai
{

enum class eRedScenario
{
	Rear,
	Sea,
	Subs,
	Combo
};

const char* toString (eRedScenario);
// MAXR_RED: rear | sea | subs | combo (умолчание — combo); неизвестное — combo и предупреждение в лог
eRedScenario redScenarioFromEnv();
// машины сценария (имена по данным), покупаются при высадке из стартовых кредитов (red1)
std::vector<std::string> redLandingUnits (eRedScenario);
// покупка при высадке: {тип, груз}. red1 — машины сценария; red2 — только то, что окно высадки продаёт человеку
std::vector<std::pair<std::string, int>> redLandingPurchase (eRedScenario, bool fair);
// отряд сценария честного режима, который строят свои заводы (тип — по штуке на запись)
std::vector<std::string> redFairForce (eRedScenario);
// здания, которые строит свой конструктор честного режима (по порядку)
std::vector<std::string> redFairBuildings (eRedScenario);

class cRedBrain : public cAiBrain
{
public:
	explicit cRedBrain (bool fair = false);
	~cRedBrain() override;

	const char* name() const override { return fair_ ? "red2" : "red1"; }

	std::vector<cOrder> decideTurn (int playerId, const cModel& model) override;
	bool wantsAnotherPass() const override { return wantPass_; }
	std::vector<cOrder> decidePass (int playerId, const cModel& model, int pass) override;
	std::vector<nlohmann::json> takeJournalEvents() override;
	// шаг 14б: «нет цели» — kNoUnit (0 — настоящая машина); приказы хозяйства base2 переводятся здесь
	bool zeroMeansNoUnit() const override { return false; }

private:
	// что налётчик знает о враге: видимое и память о виденном
	struct sSeen
	{
		cPosition pos;
		std::string type;
		bool building = false;
		bool big = false;
		bool aa = false;      // бьёт по воздуху
		bool armedGround = false; // бьёт по земле
		bool armedSea = false;    // бьёт по воде (корабли)
		bool naval = false;   // корабль, верфь, док — цель подлодок
		int range = 0;
		int cost = 0;
		int seenTurn = 0;
		bool disabled = false;
		bool visible = false; // виден сейчас (red2: положение машины вне обзора — только память)
	};
	// одна машина налёта
	struct sRaider
	{
		unsigned int id = 0;
		std::string type;
		int lastTurnSeen = 0;
		bool lost = false;
	};
	// удар машины в этом заходе: смотреть итог в следующем
	struct sShot
	{
		unsigned int unit = 0;
		unsigned int target = 0;
		std::string targetType;
		int hpBefore = 0;
		std::string what;
	};

	std::vector<cOrder> act (int playerId, const cModel& model, int pass);
	void begin (int playerId, const cModel& model);
	void updateMemory (const cModel& model, const cMapView& view);
	void checkShots (const cModel& model);
	void checkLosses (const cModel& model);

	// общие приёмы
	void awacScout (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void ferryCommando (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void ferryTanks (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	const cVehicle* transport (const cModel& model, int which) const; // 0 — для коммандос, 1 — для танков
	int commandoApproach (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	int commandoStep (const cModel& model, const cMapView& view, std::vector<cOrder>& out, const std::string& what);
	bool commandoNeeded (const cModel& model) const;
	bool tankDrop (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	bool commandoDisable (const cModel& model, std::vector<cOrder>& out, const std::string& what);
	bool bomberStrike (const cModel& model, const cMapView& view, const cVehicle& b, int mode, std::vector<cOrder>& out, const std::string& what);
	bool fireInRange (const cModel& model, const cMapView& view, const cVehicle& v, int mode, std::vector<cOrder>& out, const std::string& what);
	bool moveToward (const cModel& model, const cMapView& view, const cVehicle& v, const cPosition& dest, std::vector<cOrder>& out,
		const std::string& reason, bool keepAirSafe);
	bool tankAssault (const cModel& model, const cMapView& view, const cVehicle& t, std::vector<cOrder>& out);
	void chooseSide (const cModel& model, const cMapView& view);
	bool pickCommandoTarget (const cModel& model);

	// сценарии
	void rear (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void sea (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void subs (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void combo (const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);

	// разное
	bool airDanger (const cPosition&) const;
	bool groundDanger (const cPosition&, int margin) const;
	const cVehicle* own (const cModel& model, const std::string& type, int index = 0) const;
	std::vector<const cVehicle*> ownAll (const cModel& model, const std::string& type) const;
	int targetValue (const sSeen&, int mode) const; // mode: 0 — хозяйство, 1 — оборона, 2 — флот
	void stage (const std::string& stage, bool ok, nlohmann::json details);
	void stepEvent (const std::string& what, nlohmann::json details);
	cOrder order (eOrderType type, unsigned int unitId, const std::string& reason) const;

	eRedScenario scenario_ = eRedScenario::Combo;
	std::string eco_ = "none";
	int startTurn_ = 1;
	std::unique_ptr<cAiBrain> ecoBrain_;

	int playerId_ = -1;
	int enemyId_ = -1;
	int turn_ = 0;
	int pass_ = 1;
	bool wantPass_ = false;
	bool airStepCut_ = false; // путь самолёта у базы врага укорочен — лететь дальше в следующем заходе
	bool begun_ = false;
	cPosition home_;
	cPosition base_;          // где база врага: его точка высадки, потом середина виденных зданий
	std::map<unsigned int, sSeen> seen_;
	std::map<unsigned int, sRaider> raiders_;
	std::set<std::string> stagesDone_;
	std::vector<sShot> shots_;
	std::set<std::pair<int, int>> reserved_; // клетки, занятые приказами этого захода (самолёты не встают вдвоём)
	std::vector<nlohmann::json> events_;

	// AWAC
	int awacRing_ = -1;       // номер точки облёта (8 сторон), −1 — ещё не начат
	int awacVisited_ = 0;
	int side_ = -1;           // выбранная сторона удара (0..7), −1 — ещё нет
	cPosition staging_;       // стоянка самолётов на этой стороне
	cPosition tankStaging_;   // сбор танков (combo)
	// коммандос и его доставка
	unsigned int cmdTarget_ = kNoUnit;
	bool cmdHasTarget_ = false;
	std::set<unsigned int> cmdUnreachable_; // цели, к которым коммандос не подойти (окружены зданиями — П22)
	std::vector<unsigned int> airTransports_; // по id: первый — коммандос, второй — танки (combo)
	int ferryState_ = 0;      // 0 — забрать, 1 — везти, 2 — выгружен (или идёт сам), 3 — транспорт вернулся
	cPosition dropAt_;
	int cmdDisabledTurn_ = -1;
	int cmdTriedTurn_ = -1;   // ход последней попытки выключить (одна за ход)
	int cmdAttempts_ = 0;
	// удар
	int strikeTurn_ = -1;     // ход, в котором идёт удар
	int strikePhase_ = 0;
	int strikes_ = 0;
	int waitCommando_ = 0;
	// море
	int seaState_ = 0;        // 0 — погрузка, 1 — плыть, 2 — выгрузка, 3 — штурм
	cPosition beach_;         // клетка воды у берега, куда идёт транспорт
	bool beachChosen_ = false;
	int embarkTurns_ = 0;
	cPosition rvWater_, rvLand_; // встреча танков с морским транспортом (берег, куда танк не встаёт)
	bool rvChosen_ = false;
	// подлодки
	std::vector<cPosition> patrol_;
	std::map<unsigned int, int> patrolIdx_;
	bool patrolNone_ = false; // точек дозора нет — подлодки стоят
	// combo
	int comboPhase_ = 0;
	std::vector<int> comboPasses_;
	bool comboDisableOk_ = false;
	bool comboBombOk_ = false;
	bool comboTankOk_ = false;
	bool comboReport_ = false; // удар доигран — подвести итог в следующем заходе
	int comboWait_ = 0;
	cPosition dropZone_;      // где авиатранспорт высаживает танки в ход удара
	bool dropChosen_ = false;
	int comboTankPass_ = -1;
	int tankFerry_ = 0;       // 0 — погрузка, 1 — полёт к сбору, 2 — выгрузка, 3 — выгружены, 9 — транспорта нет: едут сами

	// --- честный режим red2 (ai_red_fair.cpp) ---
	struct sRaidSite
	{
		std::string type;               // здание по данным: airfactory, hangar, habitat, training, shipfactory
		unsigned int id = kNoUnit;      // построенное здание
		cPosition site;                 // левая верхняя клетка площадки
		bool chosen = false;
		int fails = 0;                  // площадка не подошла / пути нет
		int waited = 0;                 // ходов у площадки без стройки
		bool linked = false;            // в главной подбазе (металл, энергия, люди)
		int linkTries = 0;
	};
	void fairBegin (const cModel& model);
	void claimNew (const cModel& model);
	void markExplored (const cModel& model, const cMapView& view);
	void explore (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	void builderStep (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	void engineerStep (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	void produce (const cModel& model, std::vector<cOrder>& out);
	void service (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	void checkForce (const cModel& model);
	bool chooseSite (const cModel& model, const cMapView& view, sRaidSite& s);
	bool planLink (const cModel& model, const sRaidSite& s);
	bool refill (const cModel& model, const cMapView& view, const cVehicle& v, int want, std::vector<cOrder>& out);
	bool refillHere (const cModel& model, const cVehicle& v) const;
	void guardHome (const cModel& model, const cMapView& view, std::vector<cOrder>& out);
	bool finishIfBuilt (const cModel& model, const cVehicle& v, std::vector<cOrder>& out);
	bool owned (unsigned int id) const;

	bool fair_ = false;
	bool baseKnown_ = false;
	bool forceReady_ = false;
	unsigned int builderId_ = kNoUnit;
	unsigned int engineerId_ = kNoUnit;
	std::vector<sRaidSite> sites_;
	std::set<std::pair<int, int>> badSites_;
	std::vector<cPosition> connPath_;       // соединители от здания налёта к главной подбазе
	int connFor_ = -1;                      // для какой площадки (номер в sites_)
	std::vector<char> explored_;            // клетка была в обзоре
	int mapN_ = 0;
	std::map<unsigned int, cPosition> exploreTo_;
	std::set<std::pair<int, int>> exploreBad_;
	std::set<unsigned int> service_;        // самолёты в ангаре или на пути к нему
	std::set<unsigned int> claimedBuildings_;
};

} // namespace ai

#endif // ai_headless_ai_red_brainH
