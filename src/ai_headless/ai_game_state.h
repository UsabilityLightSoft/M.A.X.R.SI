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
// Шаг 11: map и mapChecksum; у чужих игроков — без кредитов, исследований и подбаз;
// видимые ничейные юниты (чужие, owner −1, без обломков); у машины — moveDest/pathLeft
// (едет по пути), disabledTurns, manualFire, loaded, cargo/cargoMax (транспорт и
// хранилища), stealth {hidden, detectedBy}, commando {successes, rank, targets: соседние
// цели с шансом выключить / захватить по cCommandoData::computeChance}.
nlohmann::json buildGameStateDigest (const cModel& model, int viewpointPlayerId = -1);

// Шаг 11: сжатый снимок захода 2+ (ход в несколько заходов): машины (свои, видимые чужие и
// ничейные) и здания, кроме своих целых, — они с первого захода не менялись; без игроков,
// подбаз и ресурсов (они — в полном снимке первого захода).
nlohmann::json buildPassDigest (const cModel& model, int viewpointPlayerId, int pass);

// Шаг 11 (для шага 16): видимость клеток игрока — обзор (scan) и обнаружение скрытных на суше и
// на воде; по строкам отрезки [y, x, длина]. Движок пишет её в файл снимка, в запрос советника — нет.
nlohmann::json buildVisibility (const cModel& model, int playerId);

} // namespace ai

#endif // ai_headless_ai_game_stateH
