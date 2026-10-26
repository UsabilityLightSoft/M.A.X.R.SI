/***************************************************************************
 * ai_client_console.cpp — консоль клиента ИИИ (шаг 15).
 ***************************************************************************/

#include "ai_client_console.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

namespace ai_client
{

namespace
{
std::atomic<bool>* gStop = nullptr;
std::atomic<bool>* gDone = nullptr;

#ifdef _WIN32
BOOL WINAPI onConsoleEvent (DWORD type)
{
	if (gStop == nullptr) return FALSE;
	gStop->store (true);
	// закрытие окна, выход из системы: процесс снимут, как только обработчик вернётся —
	// дать основному потоку записать итоги (Windows ждёт обработчик до 5 с)
	if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
	{
		for (int i = 0; i < 45 && gDone != nullptr && !gDone->load(); ++i)
			std::this_thread::sleep_for (std::chrono::milliseconds (100));
	}
	return TRUE;
}
#endif
} // namespace

void installConsole (std::atomic<bool>* stop, std::atomic<bool>* done)
{
	gStop = stop;
	gDone = done;
#ifdef _WIN32
	SetConsoleOutputCP (CP_UTF8);
	SetConsoleCtrlHandler (onConsoleEvent, TRUE);
#endif
}

void setEnv (const char* name, const char* value)
{
#ifdef _WIN32
	_putenv_s (name, value);
#else
	setenv (name, value, 1);
#endif
}

} // namespace ai_client
