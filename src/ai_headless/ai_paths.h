/***************************************************************************
 * ai_paths.h — пути вывода партии (журнал, метрики, снимки) широкими строками.
 *
 * На Windows узкий argv — в кодовой странице ANSI, а libstdc++ читает узкие
 * строки путей как UTF-8: каталог с русскими буквами молча не писался
 * (PLAN.md §7 п. 91). Путь берётся из широкой командной строки.
 ***************************************************************************/

#ifndef ai_headless_ai_pathsH
#define ai_headless_ai_pathsH

#include <filesystem>
#include <string>

namespace ai
{

// argv[index] как путь (на Windows — из широкой командной строки); нет аргумента — fallback
std::filesystem::path argPath (int argc, char** argv, int index, const char* fallback);

// путь текстом UTF-8 — для журнала движка
std::string pathText (const std::filesystem::path& path);

} // namespace ai

#endif // ai_headless_ai_pathsH
