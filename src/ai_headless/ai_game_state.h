/***************************************************************************
 * ai_game_state.h — GameState digest (PLAN_AI этап 2).
 *
 * Сериализуемый снимок игры на начало хода ИИ → nlohmann::json. Один формат
 * для алгоритмического «мозга» (через модель), LLM-советника и журнала.
 * Точка зрения: viewpointPlayerId; -1 = полный обзор (отладка/метрики).
 ***************************************************************************/

#ifndef ai_headless_ai_game_stateH
#define ai_headless_ai_game_stateH

#include <nlohmann/json.hpp>

class cModel;

namespace ai
{

// Собирает компактный JSON-снимок модели. Туман войны: юниты/здания
// противника попадают в дайджест только если видны viewpoint-игроку;
// собственные юниты — всегда; ресурсы — только разведанные viewpoint-игроком.
nlohmann::json buildGameStateDigest (const cModel& model, int viewpointPlayerId = -1);

} // namespace ai

#endif // ai_headless_ai_game_stateH
