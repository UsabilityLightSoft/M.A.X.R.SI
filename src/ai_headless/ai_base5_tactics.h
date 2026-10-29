/***************************************************************************
 * ai_base5_tactics.h — замороженная копия ai_tactics.h (мозг «base5»).
 *
 * ЗАМОРОЖЕН 28.10.2026, НЕ ПРАВИТЬ — чемпион с шага 13, часть 4б (Р17; PLAN.md §6, мерило — последний
 * чемпион): ИИИ-4 + ответ на армаду без переправы и «пути по суше нет», налёт, ранний раш, тактика заходов
 * П19–П22 и огонь наугад, осторожный геолог, свежая верфь и сфера после жилья, выбор клана, колонна в узостях
 * (rules/13_4b_tactics.md). Копия сделана скриптом tmp/R17/tools/make_base5.py; отличия от ai_tactics.h — только
 * пространство имён ai::base5, стражи заголовков, include копий, name() = "base5" и выключатели опытов
 * (в копии не читаются).
 ***************************************************************************/

/***************************************************************************
 * ai_tactics.h — тактика заходов ИИИ (П19–П22, огонь наугад; Р17, шаг 13, часть 4б). Решения простые, каждое — в журнал
 * (событие «tactic»): из чего выбирали и почему — потом это возьмёт Модель (шаги 18–19; Владыка: «уводить раненую или давить —
 * решает Модель, правила — вариант по умолчанию»).
 *
 *  П19, П20 — «выстрелил — отъехал»: в заходе 2+ своя наземная машина, что уже стреляла в этот ход, ранена (hp < kRunHpPct),
 *             стоит в «ударе с подъезда» врага и не переживёт его ход (ожидаемый урон врага в клетке ≥ её hp), остатком хода
 *             уходит туда, где враг её не достанет (ближе к своим); под второй выстрел врага остаются целые и те раненые, что
 *             переживут (их ответный огонь дороже отхода). Исключение «давить»: машина в операции, что бьёт цель с силой ≥ 2 ×
 *             нужной, — решающая атака, раненая остаётся.
 *  П22      — урон из ниоткуда: свой юнит потерял hp с прошлого хода, а ни один враг из памяти (не старше хода) его не доставал.
 *             У воды (корабль, вода или берег рядом) — подлодка: флоту нужен корвет (только он её видит); у базы на суше —
 *             диверсант (или пушка из тумана): тревога «диверсант» — пехота обходит клетки (ai_defense).
 *  Огонь наугад — свой юнит выключен с прошлого хода (диверсант действует только с соседней клетки и в ход удачи не уходит):
 *             свободные стрелки (выстрел есть, видимой цели в дальности нет) бьют принудительной атакой по соседним клеткам
 *             выключенного — честно: где диверсант, мы не знаем, в журнал — признаки и клетки.
 * Всё детерминированно: обход по id и клеткам.
 ***************************************************************************/

#ifndef ai_headless_ai_base5_tacticsH
#define ai_headless_ai_base5_tacticsH

#include "ai_types.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

class cMapView;
class cModel;
class cPlayer;
class cVehicle;

namespace ai::base5
{

class cArmyOps;
class cBaseDefense;
class cRoleBook;
class cWorldModel;

class cTactics
{
public:
	// начало хода, после памяти о враге: урон из ниоткуда, свои выключенные с прошлого хода
	void update (const cModel& model, const cPlayer& player, const cWorldModel& world, const cBaseDefense& defense, int turn);

	struct sHiddenHit
	{
		cPosition pos;
		unsigned int unit = 0;
		std::string type;
		int damage = 0;
		bool water = false; // у воды — подлодка
		bool base = false;  // у базы на суше — диверсант
	};
	const std::vector<sHiddenHit>& hiddenHits() const { return hits_; }

	// заходы 2+: П19/П20 — раненая, что уже стреляла, уходит из «удара с подъезда» врага; busy — уже с приказом
	std::vector<cOrder> hitAndRun (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
		const cRoleBook& roles, const cArmyOps& ops, const std::set<unsigned int>& skip, int pass);
	// заходы 2+: огонь наугад по клеткам у своих выключенных (диверсант рядом); shooters — свободные стрелки
	std::vector<cOrder> blindFire (const cModel& model, const cPlayer& player, const cMapView& view, const cWorldModel& world,
		const std::vector<const cVehicle*>& shooters, int pass);

	// П21 (микро, заход 1): враг бьёт дальше нас — сблизиться и выстрелить первым (closeIn) или уйти из его досягаемости
	void noteOutranged (unsigned int id, bool closeIn, const cPosition& from, const cPosition& to, int enemyRange, int ownRange);

	std::vector<nlohmann::json> takeEvents();

	struct sCounts
	{
		int hiddenHits = 0;  // урон из ниоткуда (за партию)
		int subHits = 0;     // из них у воды
		int baseHits = 0;    // из них у базы
		int runs = 0;        // «выстрелил — отъехал»
		int presses = 0;     // «давить» (раненая осталась: решающая атака)
		int blindShots = 0;  // выстрелов наугад
		int blindSites = 0;  // мест, по которым стреляли наугад
		int closeIns = 0;    // П21: враг дальнобойнее — сблизились и выстрелили первыми
		int stepOuts = 0;    // П21: враг дальнобойнее — ушли из его досягаемости
	};
	const sCounts& counts() const { return counts_; }

private:
	void event (const char* what, nlohmann::json extra);

	int turn_ = 0;
	int playerId_ = -1;
	std::map<unsigned int, int> hp_;            // hp своих юнитов в начале прошлого хода
	std::set<unsigned int> disabledLast_;       // свои выключенные в начале прошлого хода
	std::vector<sHiddenHit> hits_;
	struct sSite
	{
		cPosition pos;
		bool big = false;
		unsigned int unit = 0;
		std::string type;
	};
	std::vector<sSite> sites_;                  // выключены с прошлого хода — у них диверсант
	std::set<long long> blindDone_;             // клетки, по которым уже били наугад в этот ход
	std::set<unsigned int> ranThisTurn_;        // уже отъехали в этот ход
	sCounts counts_;
	std::vector<nlohmann::json> events_;
};

} // namespace ai::base5

#endif // ai_headless_ai_base5_tacticsH
