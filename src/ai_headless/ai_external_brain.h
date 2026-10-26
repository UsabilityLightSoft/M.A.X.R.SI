/***************************************************************************
 * ai_external_brain.h — внешний мозг (шаг 16, часть Б): решения игрока принимает
 * внешняя программа (Python, сеть) по HTTP на localhost; движок ИИИ исполняет.
 *
 * Каждый заход хода (шаг 11) — один запрос POST на MAXR_EXT_URL:
 *   {"player", "turn", "pass", "mode", "digest": полный дайджест + visibility,
 *    "proposal": приказы правил (режим assist), "events": события правил (hold, world …)}
 * Ответ: {"orders": [...], "hold": [id …], "replace": bool, "wantAnotherPass": bool}.
 *
 * Режимы (MAXR_EXT_MODE):
 *   assist (умолчание) — правила решают как обычно (их состояние живёт), внешняя программа
 *     перекрывает приказы машин: её приказ машине или «hold» снимает приказы правил этой
 *     машине; replace = true — берутся только её приказы. Пустой ответ — ход правил.
 *   full — правила не работают, всё решает внешняя программа.
 * Ошибка связи — приказы правил (assist) или пустой заход (full) и событие external в журнале.
 *
 * Приказ ответа (позиция — [x, y] или {"x", "y"}; тип — «a.b»):
 *   Move {unitId, to | path, fire?: id, moveEnd?: "load"|"getIn", unitId2?}
 *   Attack {unitId, target}; Hold {unitId} — снять приказы правил машине;
 *   Load / Repair / Rearm / Disable / Steal {unitId, unitId2}; ActivateUnit {unitId, unitId2, pos};
 *   StartWork / StopWork / ToggleSentry / ToggleManualFire {unitId}; FinishBuild {unitId, pos};
 *   Build {unitId, building, pos, buildSpeed?}; BuildPath {+ pos2};
 *   ChangeBuildList {unitId, list, buildSpeed?, repeat?}; ChangeResearch {areas: [8]};
 *   Transfer {unitId, unitId2, value, resType: 1 металл / 2 нефть / 3 золото}.
 * Законность проверяет валидатор движка, отказ — в журнале с причиной.
 ***************************************************************************/

#ifndef ai_headless_ai_external_brainH
#define ai_headless_ai_external_brainH

#include "ai_brain.h"

#include <memory>
#include <string>
#include <vector>

namespace ai
{

class cRuleBrain;

struct sExternalConfig
{
	std::string url = "http://127.0.0.1:18300/decide";
	std::string mode = "assist"; // assist | full
	int timeoutMs = 120000;

	static sExternalConfig fromEnv();
};

class cExternalBrain : public cAiBrain
{
public:
	explicit cExternalBrain (sExternalConfig config);
	~cExternalBrain() override;

	const char* name() const override { return "external"; }
	std::vector<cOrder> decideTurn (int playerId, const cModel& model) override;
	bool wantsAnotherPass() const override { return wantPass_; }
	std::vector<cOrder> decidePass (int playerId, const cModel& model, int pass) override;
	std::vector<nlohmann::json> takeJournalEvents() override;
	// шаг 13, часть 2: «нет цели» — kNoUnit (0 — машина с id 0): ни правила режима assist, ни разбор ответа
	// программы не пишут 0 как «нет»; выключенные коммандос врага свои юниты — правилам режима assist
	bool zeroMeansNoUnit() const override { return false; }
	void noteDisabled (const std::vector<unsigned int>& ids) override;

private:
	std::vector<cOrder> decide (int playerId, const cModel& model, int pass, std::vector<cOrder> proposal, bool ruleWants);

	sExternalConfig config_;
	std::unique_ptr<cRuleBrain> rule_; // режим assist
	std::vector<nlohmann::json> events_;
	bool wantPass_ = false;
	int requests_ = 0;
	int failures_ = 0;
};

} // namespace ai

#endif // ai_headless_ai_external_brainH
