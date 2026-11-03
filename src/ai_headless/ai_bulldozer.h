/***************************************************************************
 * ai_bulldozer.h — бульдозер у ИИИ (И1, механика Владыки 28.10.2026).
 *
 * На клетке, где что-то сломано, строить нельзя (обломки, cMap::possiblePlaceBuilding); бульдозер расчищает её (приказ Clear,
 * cActionClear: малые обломки — 1 ход, большие — 4), металл обломков ложится в его груз (до 30, сверх — теряется), и клетку
 * снова можно застраивать. Металл с поля боя бульдозер передаёт ремонтнику и грузовику боеприпасов (Transfer), те чинят и
 * перезаряжают свою технику из него — в затяжном бою не нужно возить металл с базы.
 *
 * Что делает модуль (только мозг rule; замороженные base* его не знают):
 *   - память обломков по видимому (обломки — ничьи здания: только исчезают и растут), digest — раздел «rubble»;
 *   - (а) у базы: обломки там, где они мешают стройке — на разведанном ресурсе под шахту и у зданий главной подбазы, —
 *     расчистить; металл — ремонтнику и грузовику у базы, иначе в склад базы;
 *   - (б) затяжной бой: бульдозер при армии собирает металл с обломков поля (вне «удара с подъезда» врага) и отдаёт ремонтнику и
 *     грузовику боеприпасов, чей запас ниже половины; ремонтник и грузовик с бульдозером рядом не едут на базу за металлом,
 *     получив металл — сразу чинят и перезаряжают соседей (в следующем заходе);
 *   - (в) когда строить: малой фабрике больше нечего строить (её нужды — ПВО, разведчики, ремонтники… — раньше), а бульдозер
 *     нужен: у базы — обломки мешают стройке или металла в обломках у центра базы от 10 (один), при армии — армия от 6 машин с
 *     ремонтником или грузовиком и металл в обломках поля у армии (один; армия от 20 и металла от 40 — два); всего не больше
 *     трёх. Обломки на месте шахты — бульдозер срочно: раньше прочих нужд малой фабрики, кроме ПВО.
 * Выключатель опыта MAXR_BULLDOZER=0 — поведение и digest как до И1 (для сравнения).
 * Детерминированно: без случайных чисел, порядок — по id и клеткам.
 ***************************************************************************/

#ifndef ai_headless_ai_bulldozerH
#define ai_headless_ai_bulldozerH

#include "ai_types.h"

#include "game/data/units/id.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class cBuilding;
class cMapView;
class cModel;
class cPlayer;
class cSubBase;
class cVehicle;

namespace ai
{

class cRoleBook;
class cWorldModel;

// И1: опыт бульдозера включён (по умолчанию — да; MAXR_BULLDOZER=0 — поведение и digest как до И1)
bool bulldozerOn();

// порядок клеток для map/set (у cPosition нет operator<): по строкам, потом по столбцам
struct sPosLess
{
	bool operator() (const cPosition& a, const cPosition& b) const { return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x(); }
};

// обломки, которые игрок видел: клетка — левый верхний угол (большие — 2×2)
struct sRubbleMemory
{
	cPosition pos;
	int value = 0;     // металл при расчистке (cBuilding::getRubbleValue)
	bool big = false;
	int seenTurn = 0;  // ход, когда видели
};

class cBulldozers
{
public:
	// задание бульдозера внутри хода (заход 1 → заходы 2+)
	enum class eTask
	{
		None,
		ClearBase,  // (а) доехал до обломков у базы, мешающих стройке, — расчистить
		ClearField, // (б) доехал до обломков поля боя — расчистить
		Deliver     // доехал до получателя — отдать металл
	};

	struct sCounts
	{
		int bulldozers = 0;     // своих бульдозеров (живые)
		int wantedBase = 0;     // нужно бульдозеров у базы
		int wantedField = 0;    // нужно бульдозеров при армии
		int baseRubble = 0;     // обломков у базы, мешающих стройке
		int baseMetal = 0;      // металла в них
		int onResource = 0;     // из них на разведанном ресурсе — место шахты (бульдозер срочно)
		int baseMetalOnly = 0;  // металла в прочих обломках у центра базы (не мешают, но металл рядом)
		int fieldRubble = 0;    // обломков на поле у армии
		int fieldMetal = 0;     // металла в них
		int known = 0;          // обломков в памяти
	};

	// итоги за партию (событие bulldozer «summary» — в ходу, где они изменились: последняя сводка в журнале — итог)
	struct sTotals
	{
		int built = 0;            // бульдозеров заказано фабрике
		int clearOrders = 0;      // приказов Clear
		int cleared = 0;          // обломков расчищено (исчезли под нашим бульдозером)
		int cellsCleared = 0;     // клеток расчищено (большие — 4)
		int metalCollected = 0;   // металла пришло в груз бульдозеров с расчистки
		int metalLost = 0;        // металла обломков сверх вместимости груза (потерян)
		int transfers = 0;        // приказов Transfer от бульдозера
		int metalToRepair = 0;    // передано ремонтникам
		int metalToSupply = 0;    // передано грузовикам боеприпасов
		int metalToBase = 0;      // сдано в склад базы
		int repairsFromIt = 0;    // приказов Repair ремонтника, в грузе которого металл бульдозера (оценка)
		int rearmsFromIt = 0;     // приказов Rearm грузовика с металлом бульдозера (оценка)
		int buildsOnCleared = 0;  // приказов Build на расчищенной нами клетке (стройщик, площадка — один раз)
		int repairerHeld = 0;     // ходов, когда ремонтник или грузовик не поехал на базу за металлом: бульдозер с металлом рядом
		int lost = 0;             // бульдозеров потеряно

		bool operator== (const sTotals&) const = default;
	};

	// начало хода: память обломков, сверка расчистки, свои бульдозеры и нужды (сколько бульдозеров надо)
	void update (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cSubBase* main, const cPosition& baseCenter, int armyCount, int turn);

	// тип бульдозера, что строит малая фабрика игрока (sID() — нет)
	const sID& type() const { return type_; }
	// сколько бульдозеров ещё заказать (have — живые и в очереди фабрик)
	int toBuild (int have) const;
	// бульдозер нужен срочно: обломки на месте шахты у базы, а бульдозера нет (малой фабрике — раньше её прочих нужд, кроме ПВО)
	bool urgent (int have) const { return counts_.onResource > 0 && have == 0 && toBuild (have) > 0; }
	void noteQueued() { ++totals_.built; }

	// приказы бульдозерам хода (заход 1): уйти от угрозы, отдать металл, расчистить, к обломкам, к армии или базе.
	// armyAnchor — хвост армии в походе (или точка сбора), armyOut — армия в походе; reserved — клетки, занятые приказами этого
	// хода (дополняется). Машины, которым в out уже есть приказ, не трогает
	void plan (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world, const cRoleBook& roles,
		const cSubBase* main, const cPosition& armyAnchor, bool armyOut, std::vector<cPosition>& reserved, std::vector<cOrder>& out);
	// заходы 2+: доехал — Clear или Transfer; получатели металла — ремонт и перезарядка соседей
	std::vector<cOrder> planPass (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
		const cRoleBook& roles, const cSubBase* main, int pass);
	bool wantsPass() const { return wantPass_ || !fedThisTurn_.empty(); }
	// машина — бульдозер под модулем (микро её не трогает)
	bool claimed (unsigned int id) const { return claimed_.count (id) != 0; }

	// ремонтнику или грузовику не ехать на базу за металлом: бульдозер с металлом рядом
	bool feedsNear (const cVehicle& receiver) const;
	void noteRepairerHeld() { ++totals_.repairerHeld; }
	// приказы хода — Build на расчищенной клетке, Repair и Rearm с металлом бульдозера (для сводки)
	void noteOrders (const cModel& model, const cPlayer& player, const std::vector<cOrder>& orders);
	// в конце хода: сводка, если итоги изменились
	void noteSummary();

	const sCounts& counts() const { return counts_; }
	const sTotals& totals() const { return totals_; }
	const std::map<cPosition, sRubbleMemory, sPosLess>& rubble() const { return rubble_; }
	std::vector<nlohmann::json> takeEvents();

private:
	struct sJob
	{
		eTask task = eTask::None;
		cPosition target {-1, -1};        // клетка обломков (левый верх)
		unsigned int receiver = kNoUnit;  // кому отдать металл (машина или здание базы)
		int since = 0;
	};
	struct sClearing
	{
		cPosition pos;
		bool big = false;
		int value = 0;
		int cargoBefore = 0;
		const char* site = "";
	};

	bool baseBlocking (const cModel& model, const cPlayer& player, const cSubBase* main, const sRubbleMemory& r) const;
	unsigned int chooseReceiver (const cPlayer& player, const cWorldModel& world, const cRoleBook& roles, const cSubBase* main,
		const cVehicle& b, bool anyRoom, bool& hungry, bool& building) const;
	int pushTransfer (const cModel& model, const cPlayer& player, const cRoleBook& roles, const cVehicle& b, unsigned int receiver, int pass,
		std::vector<cOrder>& out);
	void pushClear (const cPlayer& player, const cVehicle& b, const cBuilding& rubble, int cargo, const char* site, int pass,
		std::vector<cOrder>& out);
	const cBuilding* clearableUnder (const cMapView& view, const cWorldModel& world, const cVehicle& b, int cargo, bool canDeliver) const;
	nlohmann::json summaryJson() const;

	std::map<cPosition, sRubbleMemory, sPosLess> rubble_;     // клетка (левый верх) → обломки
	std::map<cPosition, int, sPosLess> clearedBy_;            // наши расчищенные клетки (все клетки площади) → ход
	std::map<unsigned int, sJob> jobs_;             // бульдозер → задание (живёт между заходами хода)
	std::map<unsigned int, sClearing> clearing_;    // бульдозер → обломки, что он расчищает (сверка в следующем ходу)
	std::map<unsigned int, int> fedMetal_;          // ремонтник или грузовик → металла от бульдозера в его грузе (оценка)
	std::set<unsigned int> fedThisTurn_;            // получили металл бульдозера в этом ходу (ремонт в следующем заходе)
	std::set<unsigned int> feeds_;                  // ремонтники и грузовики с бульдозером с металлом рядом
	std::set<std::tuple<int, int, unsigned int>> buildsSeen_; // стройка на расчищенной клетке уже посчитана (площадка, стройщик)
	std::set<unsigned int> claimed_;
	std::set<unsigned int> alive_;                  // бульдозеры прошлого хода (потери)
	std::vector<cPosition> baseSites_;              // обломки у базы, мешающие стройке (по приоритету)
	std::vector<cPosition> fieldSites_;             // обломки на поле у армии (по приоритету)
	sID type_;
	sCounts counts_;
	sTotals totals_;
	sTotals lastTotals_;
	int turn_ = 0;
	int lastSummary_ = -1000;
	bool wantPass_ = false;
	std::vector<nlohmann::json> events_;
};

} // namespace ai

#endif // ai_headless_ai_bulldozerH
