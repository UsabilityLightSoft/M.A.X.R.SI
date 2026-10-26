/***************************************************************************
 * ai_base2_roles.h — замороженная копия ai_roles.h (мозг «base2»).
 *
 * ЗАМОРОЖЕН 26.10.2026, НЕ ПРАВИТЬ — эталон турниров с шага 11 (PLAN.md §6, §7 п. 71):
 * итог шага 10 (exe ai_headless_s10final, full D = 0.909 против base). Копия сделана
 * скриптом tmp/step11/tools/make_base2.py; отличия от ai_roles.h — только пространство имён
 * ai::base2, стражи заголовков, include копий и name() = "base2".
 ***************************************************************************/

/***************************************************************************
 * ai_roles.h — роли машин и состав армии (шаг 9 PLAN.md).
 *
 * Роль типа машины — по данным движка (data/vehicles/*\/data.json через
 * cUnitsData и текущую версию у игрока), без номеров типов в коде: эталон
 * строя — вооружённая наземная машина большой фабрики с наибольшей бронёй
 * (танк); дальше — по дальности относительно него: штурмовое орудие (+1…2),
 * артиллерия (+3…4), ракеты (дальше); ПВО бьёт только воздух; разведчик —
 * зоркий (обзор ≥ 2 дальности эталона) и быстрый (≥ 1.5 его хода); ремонтник,
 * снабженец (пополняет боезапас), конструктор, геолог — по способностям.
 *
 * Состав (идея klei1984, aiplayer.cpp — весовые таблицы ролей, стратегия —
 * множители): у большой фабрики — веса долей металла армии по ролям строя,
 * стратегия по тому, что видно у врага (оборона из зданий — осада, дальнобойные
 * машины — ответ ракетами); следующая машина — роль с наибольшей нехваткой
 * доли. У малой фабрики — по нужде: разведчики, ремонтники и снабженцы (по
 * числу боевых машин), ПВО — только когда видна авиация врага или её фабрика (идея
 * CircuitAI: доля ПВО — от стоимости авиации врага, с потолком).
 * Детерминированно: без случайных чисел, всё по id и порядку данных.
 ***************************************************************************/

#ifndef ai_headless_ai_base2_rolesH
#define ai_headless_ai_base2_rolesH

#include "game/data/units/id.h"

#include <array>
#include <map>
#include <string>
#include <vector>

class cModel;
class cPlayer;
class cStaticUnitData;
class cVehicle;

namespace ai::base2
{

class cWorldModel;
struct sEnemyMemory;

enum class eRole
{
	None,
	Tank,        // строй: броня, ближний бой
	Assault,     // штурмовое орудие: дальность чуть больше танка
	Artillery,   // артиллерия: бьёт из-за строя
	Missile,     // ракеты: наибольшая дальность
	AntiAir,     // ПВО: только по воздуху
	Scout,       // разведчик
	Repair,      // ремонтник
	Supply,      // снабженец: пополняет боезапас (у ракет его на 6 выстрелов)
	Constructor, // стройщик (конструктор, пионер)
	Surveyor,    // геолог
	Other
};
constexpr int kRoleCount = static_cast<int> (eRole::Other) + 1;
const char* roleName (eRole r);

// боевые роли операций армии: строй и поддержка
inline bool isArmyRole (eRole r) { return r == eRole::Tank || r == eRole::Assault || r == eRole::Artillery || r == eRole::Missile; }
// поддержка: стоит за строем и бьёт с предельной дальности
inline bool isSupportRole (eRole r) { return r == eRole::Assault || r == eRole::Artillery || r == eRole::Missile; }

// что видно у врага (память шага 7): стоимость по металлу
struct sEnemyPicture
{
	int air = 0;          // авиация (пик с таянием: самолёты быстро уходят из виду)
	int airFactories = 0; // фабрик авиации видно
	int longRange = 0;    // наземные машины дальше эталона строя
	int staticDef = 0;    // вооружённые здания
	int ground = 0;       // вооружённые наземные машины
};

class cRoleBook
{
public:
	// раз в ход: роли всех типов машин игрока по текущим версиям
	void build (const cModel& model, const cPlayer& player);

	eRole roleOfType (const sID& id) const;
	eRole roleOf (const cVehicle& v) const;
	// роль чужой машины из памяти (по её типу и наблюдённым данным)
	eRole roleOfMemory (const cModel& model, const sEnemyMemory& m) const;
	// тип машины роли r, который строит фабрика с canBuild == buildAs (первый по данным); sID() — нет
	sID typeFor (eRole r, const std::string& buildAs) const;
	int costOf (const sID& id) const;
	int refRange() const { return refRange_; }

private:
	eRole classify (const cStaticUnitData& sd, int range, int scan, int speedMax, int shotsMax, int damage) const;

	std::map<sID, eRole> roles_;
	std::map<sID, int> cost_;
	std::vector<sID> order_; // типы в порядке данных — детерминированный выбор типа роли
	int refRange_ = 4;
	int refScan_ = 4;
	int refSpeed_ = 24;
	const cModel* model_ = nullptr;
};

// Состав армии: что строить фабрике.
class cComposition
{
public:
	struct sNeeds
	{
		int scouts = 0;
		int repair = 0;
		int supply = 0;
		int antiAir = 0;
	};

	// начало хода: свои машины и очереди по ролям, картина врага, стратегия, нужды малой фабрики
	void update (const cModel& model, const cPlayer& player, const cRoleBook& book, const cWorldModel& world, int turn);
	// следующая машина большой фабрики (роли строя, по нехватке доли); sID() — нет подходящей
	sID nextBig (const cRoleBook& book, const std::string& buildAs);
	// следующая машина малой фабрики (разведчик, ремонтник, ПВО — по нужде); sID() — ничего не нужно
	sID nextSmall (const cRoleBook& book, const std::string& buildAs);
	// учесть машину, поставленную в очередь в этом ходу
	void noteQueued (const cRoleBook& book, const sID& id);

	const sEnemyPicture& enemy() const { return enemy_; }
	const sNeeds& needs() const { return needs_; }
	const char* strategy() const { return strategy_; }
	int count (eRole r) const { return count_[static_cast<int> (r)]; }
	// шаг 10: вес роли строя в составе (‰ металла армии, со стратегией) — польза исследований и улучшений
	int weight (eRole r) const { return weight_[static_cast<int> (r)]; }
	int metal (eRole r) const { return metal_[static_cast<int> (r)]; }
	// машин боевых ролей (строй + поддержка), с очередью
	int armyCount() const;

private:
	std::array<int, kRoleCount> count_ {}; // живые + в очереди фабрик
	std::array<int, kRoleCount> metal_ {};
	std::array<int, kRoleCount> weight_ {}; // ‰ доли металла армии (только роли строя)
	sEnemyPicture enemy_;
	sNeeds needs_;
	const char* strategy_ = "balanced";
	double airPeak_ = 0; // пик стоимости авиации врага (тает)
};

} // namespace ai::base2

#endif // ai_headless_ai_base2_rolesH
