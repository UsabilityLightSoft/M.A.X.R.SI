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

	// в конце партии: метрики в metrics.json.
	void finalize();

private:
	struct sCtx
	{
		int playerId = -1;
		cClient* client = nullptr;
		std::shared_ptr<cAiBrain> brain;
		int lastTurn = -1;
		bool acted = false;
	};

	void act (sCtx& ctx);
	void executeOrder (cClient& client, const cOrder& order);
	void writeMetrics();

	std::vector<sCtx> players_;
	std::string outDir_;
	cDecisionJournal journal_;
	long orderCounter_ = 0;
};

} // namespace ai

#endif // ai_headless_ai_engineH
