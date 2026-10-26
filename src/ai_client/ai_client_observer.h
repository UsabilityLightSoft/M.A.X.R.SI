/***************************************************************************
 * ai_client_observer.h — журнал действий других игроков (человека) в формате ИИИ (шаг 15).
 *
 * В сетевой игре сервер рассылает каждое принятое действие (cAction) всем клиентам —
 * модель у всех общая. Клиент ИИИ ставит «кран» (cActionTap) между сетью и своим
 * cClient: каждое сообщение сервера идёт через основной поток, где действие чужого
 * игрока переводится в приказ Order API (ai_types.h) и пишется в journal_human.jsonl
 * теми же полями, что приказы ИИИ в journal.jsonl (orderToJson ai_engine.cpp), плюс
 * "source": "human". На начале хода человека — снимок digest/turn<N>_p<id>.json с его
 * точки зрения (туман войны — как у ИИИ). Модель при записи — до исполнения действия.
 ***************************************************************************/

#ifndef ai_client_ai_client_observerH
#define ai_client_ai_client_observerH

#include "ai_decision_journal.h"
#include "ai_types.h"
#include "game/connectionmanager.h"
#include "utility/thread/concurrentqueue.h"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>

class cModel;
class cNetMessage;

namespace ai_client
{

// Приёмник сообщений сервера вместо cClient: копит их, основной поток разбирает и отдаёт клиенту.
class cActionTap : public INetMessageReceiver
{
public:
	void pushMessage (std::unique_ptr<cNetMessage> message) override { queue.push (std::move (message)); }
	std::unique_ptr<cNetMessage> popMessage() override { return queue.try_pop().value_or (nullptr); }

private:
	cConcurrentQueue<std::unique_ptr<cNetMessage>> queue;
};

// Действие движка (JSON сетевого сообщения) → приказ Order API. Нет такого приказа — nullopt.
std::optional<ai::cOrder> actionToOrder (const nlohmann::json& action, const cModel& model);

// Приказ → JSON журнала (копия orderToJson из ai_engine.cpp: формат тот же).
nlohmann::json orderToJson (const ai::cOrder&);

class cObserver
{
public:
	cObserver (std::string outDir, int aiPlayerId);
	~cObserver();

	// сообщение сервера перед передачей cClient (модель ещё не исполнила его)
	void onMessage (cNetMessage&, const cModel&);
	// раз за круг цикла: начало хода другого игрока — снимок digest и событие turn_start
	// (когда начало хода доиграно; forcePlayer — его действие ждёт: снимок сразу, до действия)
	void tick (const cModel&, int forcePlayer = -1);
	// действие этого сообщения — другого игрока и ещё не его ход в модели (Turns): придержать сообщение
	bool mustWait (const cNetMessage&, const cModel&) const;
	// итоги для client.json
	nlohmann::json summary() const;
	void close();

private:
	struct sPlayerState
	{
		int digestTurn = -1;  // ход последнего снимка
		int turnOrders = 0;   // приказов за текущий ход
		int orders = 0;       // приказов всего
		int otherActions = 0; // действий без приказа Order API (ResumeMove, SetAutoMove, Clear, …)
		std::map<std::string, int> byType;
	};

	void logOrder (int player, int turn, const nlohmann::json& action, const cModel&);

	std::string outDir;
	int aiPlayerId;
	ai::cDecisionJournal journal;
	long seq = 0;
	std::map<int, sPlayerState> players;
	std::map<std::string, int> ownEchoes; // свои действия, принятые сервером и разосланные всем (по типу)
	int digests = 0;
};

} // namespace ai_client

#endif // ai_client_ai_client_observerH
