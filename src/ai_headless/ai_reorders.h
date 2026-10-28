/***************************************************************************
 * ai_reorders.h — Р22: пересмотр приказов каждый ход (решение Владыки 28.10.2026: «после загрузки, да и вообще на каждом
 * ходе нужно пересматривать все приказы, т.к. могли произойти важные изменения и лучше другой приказ»).
 *
 * Машина с путём длиннее хода едет по заданию движения движка: доехать за ход не хватило очков — задание ждёт (cMoveJob
 * Waiting), а в конце каждого своего хода движок сам ведёт машину дальше (cModel::resumeMoveJobs) — после всех решений
 * хода. До Р22 мозг правил такие машины пропускал (isIdle: «есть задание движения — занята»): они доезжали по старому
 * приказу, что бы ни случилось за эти ходы, а после загрузки сохранения — по пути, которого этот мозг не давал (Р19:
 * единственный конструктор три хода ехал без приказов).
 *
 * Теперь в начале хода (заход 1, до решений) каждая такая машина пересматривается. Память намерений — какой приказ Move
 * мозг дал машине: вид (по причине приказа), цель пути, ход. Машина «отпущена» (мозг решает за неё, как за свободную;
 * дела не нашлось — стоп), если:
 *  — намерения нет (загрузка сохранения, чужой путь) или цель пути не та;
 *  — приказ задуман на ход (строй операций, сопровождение, посты, приказы флота, авиации, коммандос): путь остался
 *    случайно — обход занятой клетки движком;
 *  — приказу больше kMaxCarry ходов;
 *  — цель потеряла смысл (план стройки снят или занят, машину не ждут в депо, клетка разведана, цель выстрела пропала);
 *  — путь стал опасен: клетка, где машина встанет в этот ход, — под угрозой врага;
 *  — есть заметно лучшая задача: враг в дальности (выстрел), раненой — отход, стройщику под угрозой — уход, бойцу у
 *    тревоги базы — защита.
 * Иначе машина едет дальше (гистерезис: отход, уход от угрозы, путь к площадке и в депо, разведка не дёргаются туда-сюда).
 * Отпущенной машине, которой мозг не дал Move, — StopWork (иначе в конце хода движок продолжит старый путь); с другим
 * приказом (выстрел, стройка, погрузка) — StopWork перед ним. Всё — в журнал (событие «review»).
 *
 * Выключатель опытов MAXR_REVIEW: «0» — как до Р22 (машины с путём не трогать), «all» — отпускать всех с путём (без
 * гистерезиса), «now» — оставленная машина едет дальше в заходе 1 (тот же путь приказом Move), а не в конце хода. Замороженные base* его не читают.
 ***************************************************************************/

#ifndef ai_headless_ai_reordersH
#define ai_headless_ai_reordersH

#include "ai_types.h"
#include "utility/position.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

class cModel;
class cPlayer;
class cVehicle;

namespace ai
{

// вид приказа Move по его причине — от вида зависит, когда приказ пересматривать
enum class eIntent
{
	Unknown,   // приказа нет в памяти: загрузка сохранения, чужой путь
	PerTurn,   // задуман на ход (строй и разведка операций, сопровождение, посты, тревога базы, модули): путь остался случайно
	BuildSite, // стройщик к площадке плана
	Errand,    // стройщик или снабженец к базе за металлом, пионер к цепочке соединителей
	Evacuate,  // стройщик или геолог уходит из-под угрозы
	Retreat,   // раненая — к ремонтнику, за строй, к базе
	Refit,     // в депо на обновление; радар — в депо или к центру базы
	Explore,   // разведка неразведанного, кольца и жилы геолога
	Approach,  // сближение с врагом, к краю его зоны огня
};

const char* intentName (eIntent k);

class cOrderReview
{
public:
	// MAXR_REVIEW: 0 — выкл. (как до Р22), all — отпускать всех с путём, now — пересмотр, оставленные едут в заходе 1;
	// иначе — пересмотр с гистерезисом, оставленные едут в конце хода (движок, cModel::resumeMoveJobs)
	enum class eMode
	{
		Off,
		All,
		Review,
		Now,
	};
	cOrderReview();
	eMode mode() const { return mode_; }

	struct sIntent
	{
		int turn = 0;
		int pass = 1;
		eIntent kind = eIntent::Unknown;
		std::string reason;
		cPosition dest {-1, -1};
		unsigned int target = kNoUnit; // цель выстрела в конце пути (unitId2), погрузки
		eMoveEnd moveEnd = eMoveEnd::Attack; // что сделать с target в конце пути
	};
	static eIntent classify (const std::string& reason);

	// приказы захода → память: Move — намерение машины; любой другой приказ машине (стройка, выстрел, погрузка, стоп) —
	// намерение снято
	void note (const cModel& model, const std::vector<cOrder>& orders, int turn, int pass);
	// начало хода: машины, которых больше нет или которые не свои, — из памяти; отпущенные прошлого хода — забыть
	void beginTurn (const cModel& model, const cPlayer& player, int turn);
	const sIntent* intentOf (unsigned int id) const;

	// машина ждёт продолжения пути (задание движения не кончилось, сейчас не едет) — кандидат пересмотра
	static bool carried (const cVehicle& v);
	// итог пересмотра машины: keep — едет дальше по старому пути; иначе — отпущена (why — почему)
	void decide (const cVehicle& v, bool keep, const std::string& why);
	bool released (unsigned int id) const { return released_.count (id) != 0; }
	// занята движением: едет сейчас или ждёт продолжения пути и не отпущена пересмотром
	bool moveBusy (const cVehicle& v) const;

	// после решений хода: отпущенная машина без нового Move — StopWork (иначе в конце хода движок продолжит старый путь);
	// с другим приказом (выстрел, стройка, погрузка) — StopWork перед ним. Событие «review» — в журнал
	void finish (std::vector<cOrder>& orders, int playerId);
	std::vector<nlohmann::json> takeEvents();

	struct sCounts
	{
		int carried = 0;  // машин с путём из прошлых ходов (за партию, машино-ходов)
		int kept = 0;     // из них едут дальше
		int released = 0; // отпущены: новый приказ или стоп
		int moved = 0;    // отпущенным — новый Move
		int other = 0;    // отпущенным — другой приказ (со стопом перед ним)
		int stopped = 0;  // отпущенным — только стоп
	};
	const sCounts& counts() const { return total_; }

private:
	eMode mode_ = eMode::Review;
	int turn_ = 0;
	std::map<unsigned int, sIntent> intents_;
	struct sDecision
	{
		unsigned int id = 0;
		std::string type;
		std::string old;  // причина прежнего приказа (пусто — неизвестен)
		std::string kind; // вид намерения
		bool keep = false;
		std::string why;
		int age = 0;      // ходов приказу
		int left = 0;     // клеток пути осталось
		std::vector<cPosition> path; // MAXR_REVIEW=now: оставшийся путь — тем же приказом в заходе 1
		unsigned int target = kNoUnit;
		eMoveEnd moveEnd = eMoveEnd::Attack;
	};
	std::vector<sDecision> decisions_; // этого хода, по порядку машин игрока
	std::set<unsigned int> released_;
	sCounts total_;
	std::vector<nlohmann::json> events_;
};

} // namespace ai

#endif // ai_headless_ai_reordersH
