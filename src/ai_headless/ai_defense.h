/***************************************************************************
 * ai_defense.h — анализ карты (П3) и оборона базы от налёта (П4–П9, П18, П22),
 * шаг 13 PLAN.md, часть 1. Решения — простые и записываются в журнал: сложные
 * выборы (сколько металла в оборону, где ждать налёта) потом возьмёт Модель
 * (шаги 18–19).
 *
 * П3 — карта решает упор «суша / море / воздух»: доля воды, области суши танка
 * (наша, всего, острова), до моря от базы, где здания врага (известны — по
 * памяти, честно). Нет пути по суше — флот (часть 2) или авиация (часть 3).
 *
 * Оборона — уровни по экономике (добыча металла) и ходу; тревоги поднимают их:
 *   ур. 1 — фабрика есть и добыча ≥ 16 (своя шахта сверх стартовой) или ход ≥ 60:
 *           радар у центра базы;
 *   ур. 2 — добыча ≥ 20 или ход ≥ 100: 2 радара в разных местах, кольцо из 4
 *           ПВО-пушек по краю базы, стены вокруг электростанций;
 *   ур. 3 — добыча ≥ 28 или ход ≥ 150: 3 радара, 6 ПВО-пушек, стены вокруг
 *           радаров и главных шахт, учебный центр и пехота у ключевых зданий,
 *           мины на подходах, депо и мобильный радар в нём;
 *   видна авиация врага — радаров ≥ 2 и пушек ≥ 6 (ур. 3 — 8) сразу;
 *   своё здание выключено (коммандос) — учебный центр, пехота и стены вокруг
 *   выключенного здания сразу.
 * Пехота стоит на постах — так, что каждое ключевое здание в её обзоре (4):
 * коммандос, подошедший вплотную, попадает в обзор и обнаруживается; раз в
 * 4 хода пехотинец шагает на соседнюю клетку и обратно — проверка обнаружения
 * идёт только при шаге (раздел «Скрытность» proposals_2026-10-26.md).
 * Стены (block) закрывают кольцо клеток вокруг здания: коммандос действует
 * только с соседней клетки, стену выключить нельзя. Клетки выезда фабрик,
 * ресурсов под шахты и проходы (местная связность) стенами не занимаются.
 * Шаг 13, часть 2 (П25): ракетная оборона берега — ракетные пушки (дальность 11) у моря против канонерок
 * (7) и ракетных кораблей (10): видны корабли или верфь врага — 2–6 пушек; упор «море» и ур. 2 — 2 заранее;
 * своя верфь и ур. 2 — 1 у неё. Клетка — суша у воды (2 клетки), нет суши — платформа у берега (П14,
 * замечание Владыки: на платформе — то, что не жалко); пушки не ближе 6 клеток друг к другу.
 * Всё детерминированно: обход по id и клеткам.
 ***************************************************************************/

#ifndef ai_headless_ai_defenseH
#define ai_headless_ai_defenseH

#include "game/data/units/id.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

class cBuilding;
class cModel;
class cPlayer;
class cSubBase;
class cVehicle;

namespace ai
{

class cComposition;
class cRoleBook;
class cWorldModel;

// П3: что видно на карте
struct sMapPicture
{
	int waterPct = 0;         // вода и берег, % клеток карты
	int landRegion = -1;      // область суши танка у базы
	int landRegionCells = 0;
	int landTotal = 0;        // суша танка в областях от 20 клеток
	int landSharePct = 0;     // наша область — доля этой суши, %
	int bigRegions = 0;       // областей суши от 300 клеток (материки, большие острова)
	int islands = 0;          // областей 20…299 клеток
	int seaDistance = -1;     // от базы до большого моря (Чебышёв, до 40); −1 — дальше или нет
	int seaCells = 0;         // клеток самого большого моря
	int enemyLand = -1;       // −1 — не знаем, где враг; 1 — здание врага на нашей суше; 0 — только за водой
	bool sure = false;        // упор — по известным зданиям врага, а не по догадке
	const char* emphasis = "land"; // "land" / "sea" / "air"
	std::string why;

	nlohmann::json toJson() const;
};

class cBaseDefense
{
public:
	// здание обороны на постройку: тип, клетка, зачем
	struct sBuild
	{
		sID type;
		cPosition pos {-1, -1};
		std::string why;
		bool valid() const { return pos.x() >= 0; }
	};
	// пост пехотинца: где стоять и куда шагать в ход проверки
	struct sPost
	{
		cPosition post {-1, -1};
		cPosition patrol {-1, -1};
		bool search = false; // у выключенного здания — шагать каждый ход (искать диверсанта)
	};

	// свои юниты, выключенные коммандос врага с прошлого хода (движок ИИИ, сигнал модели) — тревога
	void noteDisabled (const std::vector<unsigned int>& ids) { reportedDisabled_.insert (reportedDisabled_.end(), ids.begin(), ids.end()); }
	// раз в ход, до оперативки: картина карты, уровни, что построить, посты охраны
	void update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cRoleBook& roles,
		const cComposition& comp, const cSubBase* main, int income, int turn);

	const sMapPicture& map() const { return map_; }
	int level() const { return level_; }
	bool airAlarm() const { return airAlarm_; }
	bool commandoAlarm() const { return commandoAlarmUntil_ > turn_; }

	// следующее здание обороны для пионера (малые здания: радар, ПВО-пушка, стена); planned — что уже
	// в планах стройщиков и строится (тип и левый верхний угол), reserved — их площадки (угол + большое)
	sBuild nextSmall (const cModel& model, const cPlayer& player, const cWorldModel& world,
		const std::vector<std::pair<sID, cPosition>>& planned, const std::vector<std::pair<cPosition, bool>>& reserved) const;

	bool wantTrainingHall() const { return wantTrainingHall_; }
	bool wantDepot() const { return wantDepot_; }
	int guardsWanted() const { return guardsWanted_; }
	int pioneersWanted() const { return pioneersWanted_; }
	int minelayersWanted() const { return minelayersWanted_; }
	int surveyorsWanted() const { return surveyorsWanted_; }
	int scannersWanted() const { return scannersWanted_; }
	// клетки, где машинам не стоять (выезды фабрик, депо, учебного центра) — для постов и стен
	bool exitCell (const cPosition& p) const;

	// пост пехотинца id (post.x() < 0 — поста нет)
	sPost postOf (unsigned int id) const;
	// ход проверки поста: шагнуть (обнаружение идёт только при шаге)
	bool patrolTurn (unsigned int id) const;
	// клетки минного поля без мин (по порядку закладки)
	const std::vector<cPosition>& mineField() const { return mineField_; }
	cPosition baseCenter() const { return center_; }
	// последнее выключенное своё здание (клетка, большое) за 4 хода; x < 0 — нет
	std::pair<cPosition, bool> alarmSite (const cModel& model, const cPlayer& player) const;
	int baseRadius() const { return radius_; }
	// враг-самолёт рядом с клеткой (виден или был виден за 2 хода, ближе r): мобильной ПВО — не ехать
	bool airRaidNear (const cWorldModel& world, const cPosition& p, int r) const;

	std::vector<nlohmann::json> takeEvents();
	nlohmann::json toJson() const;
	// счётчики для строки журнала движка
	struct sCounts
	{
		int radars = 0, radarsWanted = 0, radarsWorking = 0;
		int aa = 0, aaWanted = 0;
		int walls = 0, wallsOpen = 0;
		int guards = 0;
		int posts = 0;
		int disabled = 0;
		int cutPoints = 0; // П9: соединителей — точек отказа подбазы
		int coastGuns = 0, coastGunsWanted = 0; // шаг 13, часть 2 (П25): ракетные пушки у моря
	};
	const sCounts& counts() const { return counts_; }
	// шаг 13, часть 2: водоёмы карты (для флота) — номер водоёма клетки (−1 — не вода), его размер, самое большое море
	int seaLabelOf (const cPosition& p) const;
	int seaSizeOf (int label) const { return label >= 0 && label < static_cast<int> (seaSizes_.size()) ? seaSizes_[label] : 0; }
	int bigSea() const { return bigSea_; }
	// П25: металла в вооружённых кораблях врага (память, не старше 30 ходов); видна ли его верфь
	int enemyShipMetal() const { return enemyShipMetal_; }
	bool enemyShipyardSeen() const { return enemyShipyardSeen_; }
	// П14, П25: клетка у берега (вода или берег рядом с сушей) под платформу и здание на ней
	bool platformSpotOk (const cModel& model, const cPlayer& player, const cWorldModel& world, const cPosition& c,
		const std::vector<std::pair<cPosition, bool>>& reserved) const;

private:
	void updateMap (const cModel& model, const cPlayer& player, const cWorldModel& world);
	void updatePosts (const cModel& model, const cPlayer& player, const cRoleBook& roles);
	void updateMineField (const cModel& model, const cPlayer& player, const cWorldModel& world);
	// П9: точки отказа подбазы — соединитель, без которого здания отрезаны; клетка обходной связи
	void updateRing (const cModel& model, const cPlayer& player, const cSubBase* main);
	// клетка годна под здание обороны data (без машин на ней): суша, место, не ресурс под шахту, не выезд,
	// не площадка из reserved, не под угрозой, не запирает проход
	bool spotOk (const cModel& model, const cPlayer& player, const cWorldModel& world, const sID& type, const cPosition& c,
		const std::vector<std::pair<cPosition, bool>>& reserved, const std::vector<cPosition>& extraBlocked) const;
	// у клетки (Чебышёв ≤ r) вода водоёма от 100 клеток (море, по которому придёт флот)
	bool seaNear (const cPosition& c, int r) const;

	int turn_ = 0;
	int level_ = 0;
	bool airAlarm_ = false;
	int commandoAlarmUntil_ = 0;
	std::vector<unsigned int> disabledSeen_;    // свои выключенные юниты, по которым уже подняли тревогу
	std::vector<unsigned int> reportedDisabled_; // выключены с прошлого хода (noteDisabled)
	std::vector<std::pair<cPosition, int>> recentDisables_; // клетка выключенного здания, ход — пост рядом ищет каждый ход
	std::vector<cPosition> alarmSites_;          // клетки (левый верхний угол) выключенных зданий — стены туда сначала
	cPosition center_ {-1, -1};
	int radius_ = 0;
	sMapPicture map_;
	std::string lastEmphasis_;
	bool lastSure_ = false;
	// статичное: вода карты (считается раз)
	bool waterDone_ = false;
	int waterPct_ = 0;
	std::vector<int> seaLabel_;  // номер водоёма клетки (−1 — не вода)
	std::vector<int> seaSizes_;
	int bigSea_ = -1;
	int W_ = 0;
	int H_ = 0;

	int radarsWanted_ = 0;
	int aaWanted_ = 0;
	std::vector<std::pair<cPosition, bool>> wallTargets_; // здания под стены (угол, большое), по порядку
	bool wantTrainingHall_ = false;
	bool wantDepot_ = false;
	int guardsWanted_ = 0;
	int pioneersWanted_ = 1;
	int minelayersWanted_ = 0;
	int surveyorsWanted_ = 0;
	int scannersWanted_ = 0;
	std::vector<cPosition> exits_;       // клетки выезда фабрик и хранилищ
	std::vector<cPosition> radars_;      // свои радары (построенные)
	std::vector<cPosition> aaGuns_;      // свои ПВО-пушки
	std::vector<sPost> posts_;           // посты охраны
	std::map<unsigned int, int> postOf_; // пехотинец → номер поста
	std::vector<cPosition> prevPostCells_; // клетки постов прошлого хода (по номеру) — пост за пехотинцем не скачет
	std::vector<cPosition> mineField_;
	cPosition linkCell_ {-1, -1};  // П9: клетка обходной связи (соединитель или стена) — строить следующей
	cPosition linkCut_ {-1, -1};   // соединитель — точка отказа, которую обходим
	cPosition frontDir_ {0, 0};    // откуда ждать врага (от центра базы) — стены на фронтовой стороне
	sCounts counts_;
	std::vector<nlohmann::json> events_;
	int lastLevel_ = -1;
	// шаг 13, часть 2 (П25): ракетная оборона берега
	int coastGunsWanted_ = 0;
	std::string coastWhy_;
	std::vector<cPosition> coastGuns_;  // свои ракетные пушки (построенные)
	cPosition shipyard_ {-1, -1};       // своя верфь (левый верхний угол) — пушки ближе к ней
	int enemyShipMetal_ = 0;
	bool enemyShipyardSeen_ = false;
	int lastCoastWanted_ = 0;
};

} // namespace ai

#endif // ai_headless_ai_defenseH
