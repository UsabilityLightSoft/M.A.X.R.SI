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
	// outDir — куда писать digest/, journal.jsonl, metrics.json.
	explicit cAiEngine (std::string outDir);
	~cAiEngine();

	// подключить AI-игрока (client + мозг). playerId == id игрока.
	void addPlayer (int playerId, cClient* client, std::shared_ptr<cAiBrain> brain);

	// опрос каждый тик драйвера: действует, когда ход AI-игрока активен.
	void run();

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
	};

	void act (sCtx& ctx);
	void sendEndTurn (sCtx& ctx);
	void retryEndTurn (sCtx& ctx, int turn);
	void executeOrder (cClient& client, const cOrder& order);
	void writeMetrics (const sGameResult& result);

	std::vector<sCtx> players_;
	std::string outDir_;
	cDecisionJournal journal_;
	long orderCounter_ = 0;
	cMetricsCollector metrics_; // ряд по ходам — из модели первого игрока
	bool metricsStarted_ = false;
	cSignalConnectionManager signalConnections_; // последним: отключается первым
};

} // namespace ai

#endif // ai_headless_ai_engineH
