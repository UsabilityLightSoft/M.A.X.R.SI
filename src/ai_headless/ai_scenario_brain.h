/***************************************************************************
 * ai_scenario_brain.h — сценарная проверка новых приказов ИИИ (шаг 11).
 *
 * Мозг «scenario» — не игрок, а тест хода в несколько заходов и приказов П2:
 *  - игрок 0: БМП берёт пехоту (Move + GetIn) → везёт → выгружает (ActivateUnit);
 *    авиатранспорт подлетает к танку и берёт его (Move + Load) → летит → выгружает;
 *    всё — за один ход, по заходу на шаг; вторая пехота сама входит в БМП (Move + GetIn);
 *    конструктор строит казарму, третья пехота входит в неё и выходит; коммандос выключает технику чужих
 *    (ничейную: неудача без тревоги) и здание врага; второй танк — в дозоре;
 *  - игрок 1: ведёт геолога (без оружия: ответный огонь его не трогает) под дозорный
 *    танк игрока 0 и в следующем заходе смотрит, попал ли дозорный;
 *  - «rule,scenario»: у игрока 1 — коммандос, он выключает здания правил, пока его не
 *    обнаружат; проверка — правила убили обнаруженного диверсанта.
 * Тесту можно видеть всю модель (цели, позиции врага). Итог проверки — событие
 * «scenario» в journal.jsonl (check, ok, подробности); попытки — «scenario_step».
 * Высадка: ai_headless_main добавляет игроку 0 машины сценария (kScenarioUnits),
 * точки — MAXR_LANDING рядом друг с другом. Запуск и разбор — отчёт шага 11, §6.
 ***************************************************************************/

#ifndef ai_headless_ai_scenario_brainH
#define ai_headless_ai_scenario_brainH

#include "ai_brain.h"

#include "game/data/units/id.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class cModel;
class cMapView;
class cPlayer;
class cUnit;
class cVehicle;

namespace ai
{

// «нет машины»: id 0 — настоящая машина (первая высаженная у игрока 0); шаг 13: kNoUnit — общий, ai_types.h

// машины сценария для игрока 0 (покупаются при высадке сверх бесплатного набора)
inline const std::vector<std::string> kScenarioUnits = {"apc", "infantery", "infantery", "infantery", "air_transport",
	"tank", "tank", "commando", "commando"};

class cScenarioBrain : public cAiBrain
{
public:
	const char* name() const override { return "scenario"; }

	std::vector<cOrder> decideTurn (int playerId, const cModel& model) override;
	bool wantsAnotherPass() const override { return wantPass_; }
	std::vector<cOrder> decidePass (int playerId, const cModel& model, int pass) override;
	bool zeroMeansNoUnit() const override { return false; } // шаг 13: «нет» — kNoUnit
	std::vector<nlohmann::json> takeJournalEvents() override;

private:
	// погрузить → перевезти → выгрузить за один ход (по заходу на шаг)
	struct sTransport
	{
		std::string check;     // имя проверки
		std::string carrierType;
		std::string cargoType;
		bool afterFirst = false; // начинать, когда первая перевозка БМП проверена
		unsigned int cargoPick = kNoUnit; // груз назначен заранее (kNoUnit — первая подходящая машина)
		int state = 0;         // 0 — погрузить, 1 — везти, 2 — выгрузить, 3 — проверить
		unsigned int carrier = kNoUnit;
		unsigned int cargo = kNoUnit;
		int turn = -1;         // ход, в котором начали
		std::vector<int> passes; // заход погрузки, перевозки, выгрузки
		cPosition from;        // где был транспорт до перевозки
		int tries = 0;
		bool done = false;
	};
	// коммандос выключает цель: ничейную (чужие) или здание врага
	struct sCommando
	{
		std::string check;
		bool neutral = false;  // цель — ничейная техника чужих
		unsigned int id = kNoUnit;
		unsigned int target = kNoUnit;
		bool pending = false;  // приказ отдан в прошлом заходе — смотреть итог
		int chance = 0;
		int attempts = 0;
		bool done = false;
		bool infiltrator = false; // диверсант против правил: выключать, пока не обнаружат; итог — его гибель
		int detectedTurn = -1;
		int successes = 0;
	};

	// конструктор строит казарму; пехота входит в неё и выходит (погрузка в здание и обратно)
	struct sStore
	{
		int state = 0;          // 0 — стройка, 1 — ждать конца стройки, 2 — войти, 3 — выйти, 4 — проверить
		unsigned int builder = kNoUnit;
		sID buildingType;
		unsigned int building = kNoUnit;
		unsigned int cargo = kNoUnit;
		int waitTurns = 0;
		int movedTurn = -1;     // ход, когда конструктор ехал к месту стройки
		std::vector<int> passes;
		bool done = false;
	};

	std::vector<cOrder> act (int playerId, const cModel& model, int pass);
	void store (int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void transport (sTransport& t, int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void commando (sCommando& c, int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void sentry (int playerId, const cModel& model, int pass, std::vector<cOrder>& out);
	void victim (int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out);
	void report (const std::string& check, bool ok, nlohmann::json details);
	void step (const std::string& check, nlohmann::json details);

	int turn_ = 0;
	bool wantPass_ = false;
	sTransport apc_{"transport_apc", "apc", "infantery"};
	sTransport air_{"transport_air", "air_transport", "tank"};
	sTransport apc2_{"transport_apc_getin", "apc", "infantery", true};
	sStore store_;
	bool assigned_ = false; // пехота разведена по проверкам
	sCommando alien_{"commando_neutral", true};
	sCommando enemy_{"commando_enemy", false};
	sCommando infil_{"infiltrator_killed", false, kNoUnit, kNoUnit, false, 0, 0, false, true};
	unsigned int sentryTank_ = kNoUnit;
	bool sentryDone_ = false;
	// игрок 1: геолог под огонь дозорного
	unsigned int victimId_ = kNoUnit;
	int victimHp_ = -1;
	int victimTurn_ = -1;
	bool victimDone_ = false;
	std::vector<nlohmann::json> events_;
};

} // namespace ai

#endif // ai_headless_ai_scenario_brainH
