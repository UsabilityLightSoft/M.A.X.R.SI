/***************************************************************************
 * ai_llm_advisor.h — LLM-советник (PLAN_AI этап 5, шаги 1–2 + выключатели).
 *
 * Гибрид: ход всегда считает cRuleBrain; локальная модель (OpenAI-совместимый
 * endpoint, LM Studio) получает digest + каталог техники + сводку и
 * возвращает JSON-план: директивы (production / research / army), которые
 * советник переводит в приказы, и редкие поюнитные orders. Принятые приказы
 * ЗАМЕНЯЮТ приказы правил с тем же ключом конфликта, остальные приказы правил
 * остаются. Непригодный план — один повтор с текстом ошибки, затем fallback
 * на приказы правил. Подряд идущие транспортные отказы включают
 * предохранитель (модель не зовётся несколько ходов).
 * Директивы и orders можно выключить (sLlmSwitches): выключенное убирается
 * из промпта и схемы, его решения остаются за правилами; выключено всё —
 * модель не зовётся.
 * Журнал запросов/ответов/отказов — отдельный llm.jsonl; полные ответы
 * сервера (тело целиком) — llm_replies.jsonl, из него партию можно повторить
 * без модели (MAXR_LLM_REPLAY, cLlmReplay).
 ***************************************************************************/

#ifndef ai_headless_ai_llm_advisorH
#define ai_headless_ai_llm_advisorH

#include "ai_brain.h"
#include "ai_llm_directives.h"
#include "ai_rule_brain.h"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class cModel;

namespace ai
{

class cDecisionJournal;

// Повтор партии: ответы сервера по (игрок, ход, вызов) из записи вместо запроса
// к модели (MAXR_LLM_REPLAY). Вызов — номер HTTP-обмена советника в ходе с 1
// (повтор плана и повторная отправка после 400 — следующий вызов).
// Читает llm_replies.jsonl (полное тело ответа) и llm.jsonl прежних прогонов
// (события request/response/error: content там обрезан до 500 байт —
// такие записи помечены clipped). Время ответа и сколько бюджета хода прошло
// до вызова тоже берутся из записи: llm.jsonl повтора совпадает с исходным.
class cLlmReplay
{
public:
	struct sRecord
	{
		bool skipped = false;   // повтор плана пропущен: не хватило бюджета хода
		bool ok = false;        // как sHttpResult
		int status = 0;
		std::string body;
		std::string error;
		long ms = 0;            // сколько занял запрос
		long elapsedMs = 0;     // сколько бюджета хода прошло до вызова
		std::string requestHash; // FNV-1a тела запроса; пусто — не записан (llm.jsonl)
		long long requestChars = -1;
		bool clipped = false;   // content обрезан в llm.jsonl: повтор разойдётся
	};

	// false и причина в error — файл не прочитан или в нём нет ни одного ответа
	bool load (const std::string& path, std::string& error);
	const sRecord* find (int playerId, int turn, int call) const;
	const std::string& getPath() const { return path_; }
	std::size_t size() const { return records_.size(); }
	const nlohmann::json& getGame() const { return game_; } // строка «game» записи (пусто — нет)
	const nlohmann::json* getConfig (int playerId) const;  // событие «config» игрока

private:
	std::string path_;
	std::map<std::tuple<int, int, int>, sRecord> records_;
	nlohmann::json game_;
	std::map<int, nlohmann::json> configs_;
};

// FNV-1a 64 бит, 16 hex-цифр: отпечаток тела запроса в llm_replies.jsonl
std::string llmRequestHash (const std::string& body);

// Настройки советника. Из окружения: MAXR_LLM_URL, MAXR_LLM_MODEL,
// MAXR_LLM_TIMEOUT_MS, MAXR_LLM_TURN_BUDGET_MS, MAXR_LLM_MAX_TOKENS,
// MAXR_LLM_SCHEMA, MAXR_LLM_REASONING, MAXR_LLM_RETRIES; выключатели —
// MAXR_LLM_DIRECTIVES, MAXR_LLM_ORDERS, MAXR_LLM_PROD_BASE_SHARE;
// повтор — MAXR_LLM_REPLAY (путь к llm_replies.jsonl или llm.jsonl записи).
struct sLlmConfig
{
	std::string url = "http://127.0.0.1:1234/v1/chat/completions";
	std::string model;            // пусто — поле model в запросе не отправляем
	int timeoutMs = 60000;        // предел на один запрос (connect+send+recv); поздние ходы с большой армией — до ~30 с
	int turnBudgetMs = 120000;    // предел на все запросы одного хода (с повтором)
	int maxTokens = 1024;         // max_tokens; 0 — не отправлять
	bool schema = true;           // response_format json_schema (strict)
	std::string reasoning = "none"; // reasoning_effort: none|low|medium|high; "default" — не отправлять
	int retries = 1;              // повторов при непригодном плане (0..3)
	sLlmSwitches switches;        // какие директивы / orders включены, доля базовой машины
	std::string replay;           // MAXR_LLM_REPLAY: ответы из записи вместо запросов к модели
	std::string note;             // замечания о неверных значениях переменных (для stdout)

	static sLlmConfig fromEnv();
	nlohmann::json toJson() const; // событие «config» llm.jsonl / llm_replies.jsonl
};

class cLlmAdvisorBrain : public cAiBrain
{
public:
	// journal — общий llm.jsonl (один на всех LLM-игроков партии); replies —
	// общий llm_replies.jsonl (полные ответы, может быть nullptr); replay — запись,
	// из которой брать ответы вместо запросов (nullptr — обычный режим).
	cLlmAdvisorBrain (std::shared_ptr<cDecisionJournal> journal, sLlmConfig config,
	                  std::shared_ptr<cDecisionJournal> replies = nullptr, std::shared_ptr<const cLlmReplay> replay = nullptr);

	const char* name() const override { return "llm"; }

	std::vector<cOrder> decideTurn (int playerId, const cModel& model) override;
	// шаг 11: заходы 2+ — правила внутри советника, без запроса к модели (запрос — только в заходе 1)
	bool wantsAnotherPass() const override { return rule_.wantsAnotherPass(); }
	// шаг 13: «нет цели» — kNoUnit (свои правила и разбор ответа модели не пишут 0 как «нет»)
	bool zeroMeansNoUnit() const override { return false; }
	// шаг 13, часть 2: свои юниты, выключенные коммандос врага, — своим правилам (тревога «диверсант»)
	void noteDisabled (const std::vector<unsigned int>& ids) override { rule_.noteDisabled (ids); }
	std::vector<cOrder> decidePass (int playerId, const cModel& model, int pass) override { return rule_.decidePass (playerId, model, pass); }

private:
	std::vector<cOrder> adviseOrFallback (int playerId, int turn, const cModel& model, const std::vector<cOrder>& ruleOrders);
	std::vector<cOrder> fallback (int playerId, int turn, const std::string& reason, const std::vector<cOrder>& ruleOrders);
	void transportFailure (int playerId, int turn, const std::string& what);
	void logEvent (const char* event, int playerId, int turn, nlohmann::json fields);
	void logReply (nlohmann::json entry);

	cRuleBrain rule_;
	std::shared_ptr<cDecisionJournal> journal_;
	std::shared_ptr<cDecisionJournal> replies_;
	std::shared_ptr<const cLlmReplay> replay_;
	sLlmConfig config_;
	bool configLogged_ = false;
	bool replayWarned_ = false; // повтор разошёлся с записью: строка в stdout — одна
	bool disabledLogged_ = false; // всё выключено: событие «disabled» — один раз
	int transportFailures_ = 0; // подряд (connect / timeout / «думает слишком долго»)
	int skipTurnsLeft_ = 0;     // предохранитель: сколько своих ходов ещё не звать модель
};

} // namespace ai

#endif // ai_headless_ai_llm_advisorH
