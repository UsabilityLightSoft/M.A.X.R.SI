/***************************************************************************
 * ai_base3_tech.h — замороженная копия ai_tech.h (мозг «base3»).
 *
 * ЗАМОРОЖЕН 26.10.2026, НЕ ПРАВИТЬ — чемпион с шага 13, часть 2 (PLAN.md §6, мерило — последний
 * чемпион): итог шага 13, часть 1 (rules/13_1_defense.md: оборона, высадка из файла карты,
 * скорость; full D = 0.565 против base2) на движке с правками 09–12. Копия сделана скриптом
 * tmp/step13/tools/make_base3.py; отличия от ai_tech.h — только пространство имён ai::base3,
 * стражи заголовков, include копий и name() = "base3".
 ***************************************************************************/

/***************************************************************************
 * ai_tech.h — польза исследований и улучшений за золото (шаг 10 PLAN.md).
 *
 * Мерило — сила нашей будущей армии против видимого врага. Наша армия — типы
 * ролей строя с долями металла из состава шага 9 (ai_roles); враг — типы, что
 * видели (память шага 7) с их наблюдёнными данными, по числу машин (пик с
 * таянием: армия за туманом не исчезает); врага не видели — зеркало нашей
 * армии в исходной версии. Сила по Ланчестеру (квадратичный закон):
 *   V = ln(наш урон за ход по их броне) + ln(наше hp) − ln(их урон по нашей броне)
 * на единицу металла: урон — max(1, урон − броня) × выстрелы (как cUnit::calcHealth),
 * поправка на дальность (кто бьёт дальше, стреляет первым), малые веса — ход,
 * обзор, боезапас. Цена машины входит через число машин на металл — так
 * считается тема «стоимость».
 *
 * Тема исследований (идеи klei1984 и Freeciv daitech.c, forks_ai_survey.md §9):
 * польза темы — прирост V от следующих 1–3 уровней (бонус — как
 * cPlayer::upgradeUnitTypes от исходных данных), на число очков до них, с
 * поправкой на долю партии, что останется после (бонус действует на новые
 * машины). Тема меняется, только если новая заметно выгоднее текущей.
 *
 * Покупки за кредиты: шаг улучшения типа с наибольшим приростом V на кредит;
 * лучший не по карману — копить, если доход дотянет за kSaveTurns ходов.
 *
 * Детерминированно: без случайных чисел, всё по id и порядку данных.
 ***************************************************************************/

#ifndef ai_headless_ai_base3_techH
#define ai_headless_ai_base3_techH

#include "game/data/units/id.h"

#include <array>
#include <map>
#include <string>
#include <vector>

class cModel;
class cPlayer;

namespace ai::base3
{

class cComposition;
class cRoleBook;
class cWorldModel;

// боевые данные типа (наземный ход и обзор — в клетках)
struct sCombatStats
{
	sID id;
	int damage = 0;
	int shots = 0;
	int range = 0;
	int armor = 0;
	int hp = 0;
	int speed = 0;
	int scan = 0;
	int ammo = 0;
	int cost = 1;
	int canAttack = 0; // eTerrainFlag: по кому бьёт
	bool plane = false;
};

class cTechAdvisor
{
public:
	// раз в ход: наша армия по составу, картина врага по памяти
	void update (const cModel& model, const cPlayer& player, const cRoleBook& book, const cComposition& comp,
		const cWorldModel& world, int turn);

	// центров на область исследований (порядок cResearch::eResearchArea) при centers работающих
	std::array<int, 8> researchPlan (const cModel& model, const cPlayer& player, int centers);
	const std::string& researchWhy() const { return researchWhy_; }

	struct sPurchase
	{
		sID id;
		std::array<int, 8> steps {}; // по видам kUpgradeKinds
		int cost = 0;
		double gain = 0;
	};
	// что купить на credits (доход кредитов в ход — income); пусто — ничего (или копим)
	std::vector<sPurchase> purchases (const cModel& model, const cPlayer& player, int credits, int income);
	bool saving() const { return saving_; }

	// сила армии сейчас (для журнала)
	double value() const;
	int enemyTypes() const { return static_cast<int> (enemy_.size()); }
	bool enemySeen() const { return enemySeen_; }

private:
	struct sWeighted
	{
		sCombatStats s;
		double w = 0; // наша сторона — доля металла; враг — число машин
	};
	double valueOf (const std::vector<sWeighted>& ours) const;
	// наши типы с данными после n уровней исследования области area сверх текущих
	std::vector<sWeighted> withResearch (const cModel& model, const cPlayer& player, int area, int levels) const;

	std::vector<sWeighted> ours_;
	std::vector<sWeighted> enemy_;
	bool enemySeen_ = false;
	// пик числа машин врага по типу (тает на 2% за ход) и последние наблюдённые данные
	std::map<sID, double> enemyPeak_;
	std::map<sID, sCombatStats> enemyStats_;
	int turn_ = 0;
	int horizon_ = 200;
	int topic_ = -1; // текущая тема (все центры на одной) — для гистерезиса
	std::string researchWhy_;
	bool saving_ = false;
};

} // namespace ai::base3

#endif // ai_headless_ai_base3_techH
