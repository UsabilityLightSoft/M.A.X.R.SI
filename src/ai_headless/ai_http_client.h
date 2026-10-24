/***************************************************************************
 * ai_http_client.h — минимальный HTTP-клиент для LLM-советника (этап 5).
 *
 * Только POST с JSON-телом, только схема http и только localhost
 * (127.0.0.1, localhost, ::1). Без TLS и без сторонних библиотек:
 * Winsock на Windows, BSD-сокеты в остальных системах. Исключений наружу
 * не бросает — любая ошибка возвращается в sHttpResult.
 ***************************************************************************/

#ifndef ai_headless_ai_http_clientH
#define ai_headless_ai_http_clientH

#include <string>

namespace ai
{

struct sHttpResult
{
	bool ok = false;       // ответ получен и разобран (статус проверяет вызывающий)
	int status = 0;        // HTTP-статус (200, 500, …); 0 — ответа нет
	std::string body;      // тело ответа (chunked уже собран)
	std::string error;     // что пошло не так, когда ok == false
	long elapsedMs = 0;    // сколько заняла вся операция
};

// POST url c телом body (Content-Type: application/json). timeoutMs — общий
// предел на connect + send + recv.
sHttpResult httpPostJson (const std::string& url, const std::string& body, int timeoutMs);

} // namespace ai

#endif // ai_headless_ai_http_clientH
