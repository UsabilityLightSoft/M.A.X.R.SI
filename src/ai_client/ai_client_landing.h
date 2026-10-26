/***************************************************************************
 * ai_client_landing.h — выбор точки высадки клиентом ИИИ в сетевой игре (шаг 15).
 *
 * В сетевой игре где высадились другие, клиент не знает: лобби сообщает только
 * «игрок выбрал» и состояние своей точки (Clear / Warning / TooClose / Confirmed,
 * cLandingPositionManager: ближе 28 клеток — предупреждение, ближе 10 — нельзя).
 * Поэтому: первая точка — просторная годная точка в самой большой области суши
 * (как planLanding ai_headless: здания базы встают, юниты высадки помещаются, выход
 * не отрезан), вторая и дальше (после Warning / TooClose) — самая дальняя от всех
 * прежних точек, где нас предупредили.
 ***************************************************************************/

#ifndef ai_client_ai_client_landingH
#define ai_client_ai_client_landingH

#include "game/data/units/landingunit.h"
#include "utility/position.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class cStaticMap;
class cUnitsData;

namespace ai_client
{

class cLandingChooser
{
public:
	// fixedBridgehead — «Definite» (стартовые шахта и генератор встают рядом с точкой)
	void init (const cStaticMap&, const cUnitsData&, bool fixedBridgehead, std::vector<sLandingUnit> units, std::uint32_t seed);

	// первая точка (случайная среди просторных, по сиду)
	std::optional<cPosition> first();
	// следующая после отказа: одна из самых дальних от прежних точек
	std::optional<cPosition> next();
	// последняя выбранная
	std::optional<cPosition> current() const { return tried.empty() ? std::nullopt : std::optional<cPosition> (tried.back()); }

	std::size_t candidateCount() const { return candidates.size(); }
	const std::string& describe() const { return info; }

private:
	bool valid (const cPosition&) const;
	std::optional<cPosition> pickFarthest();

	const cStaticMap* map = nullptr;
	const cUnitsData* unitsData = nullptr;
	bool fixedBridgehead = true;
	std::vector<sLandingUnit> units;
	std::vector<cPosition> candidates; // просторные клетки самой большой области (проверка годности — при выборе)
	std::vector<cPosition> tried;      // где нас предупредили или не пустили
	std::uint32_t seed = 0;
	std::string info;
};

} // namespace ai_client

#endif // ai_client_ai_client_landingH
