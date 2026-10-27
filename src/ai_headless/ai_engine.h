/***************************************************************************
 * ai_engine.h — движок ИИ (обвязка, PLAN_AI этап 2).
 *
 * Оркестрация: на начало хода AI-игрока строит дайджест, зовёт «мозг»,
 * валидирует и исполняет приказы через cClient, пишет журнал решений,
 * завершает ход. Читает состояние из клиентской модели (наш поток, без гонки
 * с сервером); приказы шлёт через тот же cClient (паттерн surveyorai).
 ***************************************************************************/

#ifndef ai_headless_ai_engineH
#define ai_headless_ai_engineH

#include "ai_brain.h"
#include "ai_decision_journal.h"
#include "ai_metrics_collector.h"
#include "utility/signal/signalconnectionmanager.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

class cClient;

namespace ai
{

struct cOrder;

class cAiEngine
{
public:
	// outDir — куда писать digest/, journal.jsonl, metrics.json (широкий путь: ai_paths.h).
	explicit cAiEngine (std::filesystem::path outDir);
	~cAiEngine();

	// подключить AI-игрока (client + мозг). playerId == id игрока.
	void addPlayer (int playerId, cClient* client, std::shared_ptr<cAiBrain> brain);

	// опрос каждый тик драйвера: действует, когда ход AI-игрока активен.
	// Ход — несколько заходов (шаг 11): мозг решает, движок отдаёт приказы и просит
	// модель доиграть их (cClient::aiPass), ход остаётся у игрока; когда доиграно —
	// мозг решает снова (cAiBrain::wantsAnotherPass / decidePass). Без окна (headless)
	// модель стоит, пока мозг думает, и идёт только в заходе; в сетевой игре время идёт
	// само, заход лишь говорит «приказы доиграны» — движку и мозгу всё равно, где играть.
	void run();

	// предел заходов за ход (MAXR_PASSES, умолчание kDefaultMaxPasses); 1 — как до шага 11.
	// Умолчание 5: у машины до 3 выстрелов (чужие; танк — 2), а за заход машина стреляет
	// раз — «подъехал и выстрелил» (1) + ещё выстрелы (2–3) + добить или отъехать (4) + запас.
	static constexpr int kDefaultMaxPasses = 5;
	int getMaxPasses() const { return maxPasses_; }

	// движок признал кого-то побеждённым (cModel::checkDefeats) — партия решена
	bool isAnyPlayerDefeated() const;

	// в конце партии: исход (победитель), metrics.json и ряд metrics_turns.jsonl.
	// endTurn — ход, на котором партия остановлена (сыграны 1…endTurn−1).
	sGameResult finalize (eGameEnd end, int endTurn);

private:
	struct sCtx
	{
		int playerId = -1;
		cClient* client = nullptr;
		std::shared_ptr<cAiBrain> brain;
		int lastTurn = -1;
		bool acted = false;
		// EndTurn отправлен в этом ходу; движок может его отклонить (см. retryEndTurn)
		bool endTurnSent = false;
		bool endTurnRefused = false;
		int endTurnRetries = 0;
		// шаг 11: заходы хода
		int pass = 0;                   // номер текущего захода хода (1, 2, …)
		std::size_t turnOrders = 0;     // приказов мозга за ход (все заходы) — для turn_end
		bool passWaiting = false;       // модель доигрывает приказы захода
		bool passSendPending = false;   // запрос захода не ушёл (клиент заморожен) — отправить снова
		unsigned int passSerial = 0;    // cModel::getAiPassSerial до запроса
		unsigned int passStartTime = 0; // время модели при запросе захода (журнал: сколько тиков шёл)
		std::chrono::steady_clock::time_point passSentAt; // только страховка от потерянного запроса
		bool passResent = false;
		std::vector<unsigned int> disabledLog; // шаг 13: свои юниты, выключенные врагом с прошлого решения
	};

	// сводка заходов игрока за партию — в metrics.json
	struct sPassStats
	{
		int turns = 0;            // ходов
		int passes = 0;           // заходов всего
		int multiPassTurns = 0;   // ходов с заходами 2+
		int maxPasses = 0;        // больше всего заходов за ход
		int laterOrders = 0;      // приказов исполнено в заходах 2+
		int limitHits = 0;        // ходов, где мозг хотел ещё заход, но упёрся в предел MAXR_PASSES
		int lostRequests = 0;     // запрос захода повторён или брошен по страховке (норма — 0)
	};

	void act (sCtx& ctx);
	void continuePass (sCtx& ctx, int turn);
	std::size_t executeOrders (sCtx& ctx, const std::vector<cOrder>& orders, int turn);
	void endPass (sCtx& ctx, int turn, std::size_t executed);
	void sendAiPass (sCtx& ctx);
	bool canStillAct (const sCtx& ctx) const;
	void logBrainEvents (sCtx& ctx, int turn);
	void sendEndTurn (sCtx& ctx);
	void retryEndTurn (sCtx& ctx, int turn);
	void executeOrder (cClient& client, const cOrder& order);
	void writeMetrics (const sGameResult& result);

	std::vector<sCtx> players_;
	std::filesystem::path outDir_;
	cDecisionJournal journal_;
	long orderCounter_ = 0;
	cMetricsCollector metrics_; // ряд по ходам — из модели первого игрока
	bool metricsStarted_ = false;
	int maxPasses_ = 1;
	std::map<int, sPassStats> passStats_; // игрок → сводка заходов
	// шаг 13: время решений мозга (мс, по часам) — только в stdout партии, не в журналы и метрики
	// (они должны повторяться побайтно); замер скорости на больших картах (п. 77 PLAN.md)
	std::map<int, double> brainMs_;
	cSignalConnectionManager signalConnections_; // последним: отключается первым
};

} // namespace ai

#endif // ai_headless_ai_engineH
