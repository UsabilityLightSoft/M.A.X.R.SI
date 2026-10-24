/***************************************************************************
 * ai_metrics_collector.h — метрики партии для D-score и исход партии
 * (PLAN_AI, «Метрика силы ИИ»).
 *
 * Ряд по ходам снимается из клиентской модели по её сигналам (поток драйвера,
 * те же тики, что у сервера): живые юниты (техника + здания), потери из дельты
 * GameOverStat, собранные ресурсы на начало хода игрока. Итоги — в
 * metrics.json, ряд — в metrics_turns.jsonl. Мозг в сборе не участвует.
 ***************************************************************************/

#ifndef ai_headless_ai_metrics_collectorH
#define ai_headless_ai_metrics_collectorH

#include "utility/signal/signalconnectionmanager.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

class cModel;
class cPlayer;

namespace ai
{

// При лимите ходов (или сторожевом обрыве) без уничтожения победитель — тот,
// у кого суммарная стоимость живых юнитов и зданий не меньше kMaterialWinRatio ×
// стоимости соперника («material»); иначе ничья («draw»).
constexpr double kMaterialWinRatio = 1.5;

enum class eGameEnd
{
	Elimination, // движок признал игрока побеждённым (cPlayer::isDefeated)
	TurnLimit, // сыграны все ходы
	Watchdog // нет смены хода дольше лимита сторожа
};

struct sGameResult
{
	int winner = -1; // id победителя; -1 — нет
	std::string winReason; // elimination | material | draw
	std::string endReason; // elimination | turnLimit | watchdog
	int endTurn = 0; // ход, на котором партия остановлена (сыграны 1…endTurn−1)
};

// суммарная стоимость (buildCost) живых юнитов и зданий игрока
int materialValue (const cPlayer&);

// Исход партии. Уничтожение — по вердикту движка: cModel::checkDefeats на начале
// круга помечает побеждённым игрока без юнитов, способных стрелять или строить
// (cPlayer::mayHaveOffensiveUnit). Иначе — по материалу (kMaterialWinRatio).
sGameResult decideGameResult (const cModel&, eGameEnd, int endTurn);

class cMetricsCollector
{
public:
	// подписаться на сигналы модели (модель клиента живёт дольше сборщика)
	void attach (const cModel&);
	// снимок после высадки всех игроков, до первого хода
	void sampleStart (const cModel&);
	// закрыть ряд: в итог идут сыгранные ходы 1…endTurn−1
	void finish (const cModel&, int endTurn);

	// итоги игрока для metrics.json (после finish)
	nlohmann::json playerSummary (int playerId) const;
	// ряд по ходам: строка JSON на ход
	bool writeTurns (const std::string& path) const;

private:
	struct sRes
	{
		int metal = 0;
		int oil = 0;
		int gold = 0;
		int sum() const { return metal + oil + gold; }
	};
	struct sSubbase
	{
		sRes stored;
		sRes maxStored;
		sRes needed;
		sRes prod;
	};
	struct sPlayerTurn
	{
		int units = 0; // живые юниты на конец хода
		int lostVehicles = 0; // потери за ход
		int lostBuildings = 0;
		sRes resources; // собрано на начало хода игрока (в склад или в расход)
		int prod = 0; // выработка шахт без учёта переполнения склада
	};
	struct sPlayerTotal
	{
		int unitsPeak = 0;
		unsigned int lostVehiclesAtRoundStart = 0;
		unsigned int lostBuildingsAtRoundStart = 0;
	};

	void onTick (const cModel&);
	void onTurnStart (const cModel&);
	void sampleUnits (const cModel&);

	std::map<int, std::vector<sSubbase>> subbasesBeforeTick; // по игроку
	std::map<int, std::map<int, sPlayerTurn>> turns; // ход → игрок → строка
	std::map<int, sPlayerTotal> totals;
	std::vector<int> playerIds;
	int openRound = 0; // текущий (ещё не закрытый) круг
	int endTurn = 0;
	cSignalConnectionManager connections;
};

} // namespace ai

#endif // ai_headless_ai_metrics_collectorH
