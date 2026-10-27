/***************************************************************************
 * ai_paths.cpp — пути вывода партии широкими строками (см. ai_paths.h).
 ***************************************************************************/

#include "ai_paths.h"

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
# include <shellapi.h>
#endif

namespace ai
{

std::filesystem::path argPath (int argc, char** argv, int index, const char* fallback)
{
#ifdef _WIN32
	int wargc = 0;
	if (LPWSTR* wargv = CommandLineToArgvW (GetCommandLineW(), &wargc))
	{
		// разбор широкой строки должен совпасть с argv по числу аргументов
		std::filesystem::path result;
		const bool same = wargc == argc;
		if (same)
			result = index < wargc ? std::filesystem::path (wargv[index]) : std::filesystem::path (fallback);
		LocalFree (wargv);
		if (same) return result;
	}
#endif
	return index < argc ? std::filesystem::path (argv[index]) : std::filesystem::path (fallback);
}

std::string pathText (const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return std::string (text.begin(), text.end());
}

} // namespace ai
