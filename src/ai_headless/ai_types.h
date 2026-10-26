/***************************************************************************
 * ai_types.h — Order API: типизированные приказы движка ИИ (PLAN_AI этап 2).
 *
 * Один формат приказов для всех уровней (микро/оперативка/стратегия) и для
 * будущего LLM-советника. Приказ — это НАМЕРЕНИЕ; исполнение идёт через
 * cClient, легальность проверяет cOrderValidator (валидатор приказов).
 ***************************************************************************/

#ifndef ai_headless_ai_typesH
#define ai_headless_ai_typesH

#include "game/data/resourcetype.h"
#include "utility/position.h"

#include <array>
#include <string>
#include <vector>

namespace ai
{

// Шаг 13: «нет юнита» в полях-ссылках приказа (unitId2). Id 0 — настоящая машина (первый конструктор
// игрока 0), поэтому 0 больше не значит «нет цели». Мозги, писавшие 0 как «нет» (base, base2, сторонние),
// переводит движок ИИИ (cAiBrain::zeroMeansNoUnit): у Move с выстрелом в конце пути, Attack и UpgradeVehicle.
inline constexpr unsigned int kNoUnit = 0xFFFFFFFFu;

enum class eOrderType
{
	None,
	EndTurn,        // playerId
	Move,           // unitId + path (+ unitId2 — что сделать с ним в конце пути: moveEnd; по умолчанию выстрел, шаг 9)
	Attack,         // unitId + targetPosition
	Build,          // unitId (конструктор) + buildingType + position
	BuildPath,      // unitId + buildingType + position (старт) + position2 (конец)
	ChangeBuildList,// unitId (фабрика) + buildList + buildSpeed + repeat
	ChangeResearch, // playerId + researchAreas
	Repair,         // unitId (источник) + unitId2 (цель)
	Rearm,          // unitId (источник) + unitId2 (цель)
	Transfer,       // unitId (источник) + unitId2 (цель) + value + resType
	StartWork,      // unitId (здание)
	StopWork,       // unitId (юнит): здание — стоп; машина — остановить движение (и долгий путь), стройку, расчистку
	ToggleSentry,   // unitId — дозор: включить / выключить (огонь по любому видимому врагу, вошедшему в дальность)
	ToggleManualFire,// unitId
	ToggleLayMines, // unitId (миноукладчик)
	ToggleCollectMines, // unitId (тральщик)
	SelfDestroy,    // unitId (здание)
	Load,           // unitId (транспорт или здание-хранилище) + unitId2 (грузимый юнит рядом; авиатранспорт — под собой)
	ActivateUnit,   // unitId (хранилище) + unitId2 (юнит) + position (куда выйти: рядом; из авиатранспорта — под ним)
	FinishBuild,    // unitId (конструктор/фабрика) + position (точка выезда)
	// шаг 10 (исследования и золото) — в конце списка: советник перебирает типы EndTurn…FinishBuild
	BuyUpgrades,    // playerId + unitType + upgradeSteps — купить улучшения типа за кредиты (золото)
	UpgradeVehicle, // unitId (хранилище: депо/ангар/док) + unitId2 (машина внутри; kNoUnit — все устаревшие)
	UpgradeBuilding,// unitId (здание) + allUnits (и все такие же здания подбазы) — до текущей версии за металл
	SetMining,      // unitId (здание подбазы) + mining — распределение добычи шахт подбазы (металл, нефть, золото)
	// шаг 11 (новые приказы П2) — в конце списка
	Disable,        // unitId (коммандос) + unitId2 (цель на соседней клетке) — выключить (cActionStealDisable)
	Steal,          // unitId (коммандос) + unitId2 (машина на соседней клетке) — захватить
};

// шаг 11: что машина делает с unitId2 приказа Move в конце пути (cEndMoveAction движка)
enum class eMoveEnd
{
	Attack, // выстрелить по unitId2 (шаг 9, «подъехал — выстрелил»)
	Load,   // погрузить unitId2 в себя (транспорт подъехал; авиатранспорт — встал над ним)
	GetIn,  // войти в unitId2 (транспорт или здание-хранилище рядом)
};

inline const char* toString (eMoveEnd e)
{
	switch (e)
	{
		case eMoveEnd::Load: return "load";
		case eMoveEnd::GetIn: return "getIn";
		default: return "attack";
	}
}

inline const char* toString (eOrderType t)
{
	switch (t)
	{
		case eOrderType::EndTurn: return "EndTurn";
		case eOrderType::Move: return "Move";
		case eOrderType::Attack: return "Attack";
		case eOrderType::Build: return "Build";
		case eOrderType::BuildPath: return "BuildPath";
		case eOrderType::ChangeBuildList: return "ChangeBuildList";
		case eOrderType::ChangeResearch: return "ChangeResearch";
		case eOrderType::Repair: return "Repair";
		case eOrderType::Rearm: return "Rearm";
		case eOrderType::Transfer: return "Transfer";
		case eOrderType::StartWork: return "StartWork";
		case eOrderType::StopWork: return "StopWork";
		case eOrderType::ToggleSentry: return "ToggleSentry";
		case eOrderType::ToggleManualFire: return "ToggleManualFire";
		case eOrderType::ToggleLayMines: return "ToggleLayMines";
		case eOrderType::ToggleCollectMines: return "ToggleCollectMines";
		case eOrderType::SelfDestroy: return "SelfDestroy";
		case eOrderType::Load: return "Load";
		case eOrderType::ActivateUnit: return "ActivateUnit";
		case eOrderType::FinishBuild: return "FinishBuild";
		case eOrderType::BuyUpgrades: return "BuyUpgrades";
		case eOrderType::UpgradeVehicle: return "UpgradeVehicle";
		case eOrderType::UpgradeBuilding: return "UpgradeBuilding";
		case eOrderType::SetMining: return "SetMining";
		case eOrderType::Disable: return "Disable";
		case eOrderType::Steal: return "Steal";
		default: return "None";
	}
}

// sID (тип юнита/здания) храним как пару чисел, чтобы не тащить game-заголовки
// в контракт; при исполнении конвертируем обратно в sID.
struct sTypeId
{
	int firstPart = 0;
	int secondPart = 0;

	sTypeId() = default;
	sTypeId (int a, int b) : firstPart (a), secondPart (b) {}

	bool operator== (const sTypeId& o) const = default;
};

// Один приказ. Поля интерпретируются по типу (см. eOrderType).
struct cOrder
{
	eOrderType type = eOrderType::None;

	int playerId = -1;               // чей приказ (EndTurn, ChangeResearch)

	unsigned int unitId = 0;         // основной юнит
	unsigned int unitId2 = kNoUnit;  // вторичный юнит (Repair/Rearm/Transfer/Load/Activate/Disable/Steal; Move — цель
	                                 // в конце пути); шаг 13: kNoUnit — нет (0 — настоящая машина)
	eMoveEnd moveEnd = eMoveEnd::Attack; // Move с unitId2: что сделать с ним в конце пути (шаг 11)

	sTypeId buildingType;            // Build/BuildPath: что строим
	cPosition position;              // Attack/Build/Activate: целевая клетка
	cPosition position2;             // BuildPath: конец строящейся дороги

	std::vector<cPosition> path;     // Move: путь БЕЗ стартовой клетки

	std::vector<sTypeId> buildList;  // ChangeBuildList: очередь типов
	int buildSpeed = 0;
	bool repeat = false;

	std::array<int, 8> researchAreas{}; // ChangeResearch: центров на область

	int transferValue = 0;           // Transfer: сколько
	eResourceType resType = eResourceType::None; // Transfer: что

	// шаг 10: улучшения и добыча
	sTypeId unitType;                // BuyUpgrades: тип юнита
	// BuyUpgrades: шагов улучшения по видам, порядок kUpgradeKinds (ai_upgrades.h):
	// урон, выстрелы, дальность, боезапас, броня, прочность, обзор, скорость
	std::array<int, 8> upgradeSteps{};
	int upgradeCost = 0;             // BuyUpgrades: цена по расчёту мозга (кредиты) — для журнала
	bool allUnits = false;           // UpgradeBuilding: и все такие же здания подбазы
	std::array<int, 3> mining{};     // SetMining: металл, нефть, золото в ход

	// аудит
	std::string level;               // "micro" / "op" / "strategy"
	std::string reason;              // зачем (для журнала решений)
};

} // namespace ai

#endif // ai_headless_ai_typesH
