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

enum class eOrderType
{
	None,
	EndTurn,        // playerId
	Move,           // unitId + path
	Attack,         // unitId + targetPosition
	Build,          // unitId (конструктор) + buildingType + position
	BuildPath,      // unitId + buildingType + position (старт) + position2 (конец)
	ChangeBuildList,// unitId (фабрика) + buildList + buildSpeed + repeat
	ChangeResearch, // playerId + researchAreas
	Repair,         // unitId (источник) + unitId2 (цель)
	Rearm,          // unitId (источник) + unitId2 (цель)
	Transfer,       // unitId (источник) + unitId2 (цель) + value + resType
	StartWork,      // unitId (здание)
	StopWork,       // unitId (юнит)
	ToggleSentry,   // unitId
	ToggleManualFire,// unitId
	ToggleLayMines, // unitId (миноукладчик)
	ToggleCollectMines, // unitId (тральщик)
	SelfDestroy,    // unitId (здание)
	Load,           // unitId (транспорт) + unitId2 (грузимый юнит)
	ActivateUnit,   // unitId (хранилище) + unitId2 (юнит) + position (куда выйти)
	FinishBuild,    // unitId (конструктор/фабрика) + position (точка выезда)
};

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
	unsigned int unitId2 = 0;        // вторичный юнит (Repair/Rearm/Transfer/Load/Activate)

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

	// аудит
	std::string level;               // "micro" / "op" / "strategy"
	std::string reason;              // зачем (для журнала решений)
};

} // namespace ai

#endif // ai_headless_ai_typesH
