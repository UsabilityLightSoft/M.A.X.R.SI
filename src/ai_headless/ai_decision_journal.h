/***************************************************************************
 * ai_decision_journal.h — журнал решений (JSONL), PLAN_AI этап 2.
 *
 * Каждая строка — одно решение/событие: приказ, план уровня, отклонение.
 * Потребители: аудит, память LLM-советника, данные для A/B и дообучения.
 ***************************************************************************/

#ifndef ai_headless_ai_decision_journalH
#define ai_headless_ai_decision_journalH

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace ai
{

// Простой append-only JSONL-журнал. Небуферизованная запись строки целиком —
// при обрыве процесса файл остаётся валидным до последней полной строки.
class cDecisionJournal
{
public:
	explicit cDecisionJournal (std::string path) : path (std::move (path)) {}

	bool open()
	{
		out.open (path, std::ios::app);
		return out.is_open();
	}

	bool isOpen() const { return out.is_open(); }

	void log (const nlohmann::json& entry)
	{
		if (!out.is_open()) return;
		out << entry.dump() << "\n";
		out.flush();
	}

	void close() { out.close(); }

	const std::string& getPath() const { return path; }

private:
	std::string path;
	std::ofstream out;
};

} // namespace ai

#endif // ai_headless_ai_decision_journalH
