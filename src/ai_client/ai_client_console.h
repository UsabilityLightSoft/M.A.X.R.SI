/***************************************************************************
 * ai_client_console.h — консоль клиента ИИИ (шаг 15): вывод UTF-8 и аккуратный выход
 * по Ctrl+C или закрытию окна (итоги партии успевают записаться). windows.h — только
 * в ai_client_console.cpp, чтобы его макросы не встречались с заголовками движка.
 ***************************************************************************/

#ifndef ai_client_ai_client_consoleH
#define ai_client_ai_client_consoleH

#include <atomic>

namespace ai_client
{

// stop — выставляется при Ctrl+C / закрытии окна; done — основной поток выставляет, когда итоги записаны
void installConsole (std::atomic<bool>* stop, std::atomic<bool>* done);

// переменная окружения процесса (MAXR_PASSES для движка ИИИ)
void setEnv (const char* name, const char* value);

} // namespace ai_client

#endif // ai_client_ai_client_consoleH
