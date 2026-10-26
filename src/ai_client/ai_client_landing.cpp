/***************************************************************************
 * ai_client_landing.cpp — выбор точки высадки клиентом ИИИ (шаг 15).
 *
 * Проверки годности — те же, что у planLanding в ai_headless_main.cpp (копия: тот
 * файл правит шаг 13, а там всё в безымянном пространстве имён): суша танка — не
 * вода, не берег, не блок; стартовые шахта и генератор встают на месте; юниты высадки
 * помещаются (cActionInitNewGame::isValidLandingPosition); здания базы не отрезают от
 * области клетки рядом с базой.
 ***************************************************************************/

#include "ai_client_landing.h"

#include "game/data/map/map.h"
#include "game/data/units/unitdata.h"
#include "game/logic/action/actioninitnewgame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ai_client
{

namespace
{
constexpr int kMinRegion = 300;    // клеток суши в области — меньше (острова) не годится армии
constexpr int kRoomRadius = 18;    // радиус «простора» — как kRoomRadius ai_headless (площадки шахт правил)
constexpr double kRoomShare = 0.5; // просторная точка — не меньше этой доли от лучшей (у planLanding 0.8: там
                                   // выбирают пару сразу; здесь соседа не видно — точки нужны по всей области)
constexpr std::size_t kTopFarthest = 5; // следующая точка — случайная из стольких самых дальних: два клиента
                                        // ИИИ, сдвигаясь одновременно, не прыгают в одну и ту же клетку

bool isTankGround (const cStaticMap& map, const cPosition& p)
{
	return map.isValidPosition (p) && !map.isWater (p) && !map.isCoast (p) && !map.isBlocked (p);
}

struct sLandGrid
{
	int w = 0;
	int h = 0;
	std::vector<int> region;
	std::vector<int> sizes;

	int at (const cPosition& p) const
	{
		return (p.x() < 0 || p.y() < 0 || p.x() >= w || p.y() >= h) ? -1 : region[p.y() * w + p.x()];
	}
};

sLandGrid buildLandGrid (const cStaticMap& map)
{
	sLandGrid g;
	g.w = map.getSize().x();
	g.h = map.getSize().y();
	g.region.assign (g.w * g.h, -1);
	std::vector<int> queue;
	for (int start = 0; start < g.w * g.h; ++start)
	{
		if (g.region[start] != -1 || !isTankGround (map, cPosition (start % g.w, start / g.w))) continue;
		const int id = static_cast<int> (g.sizes.size());
		g.region[start] = id;
		queue.assign (1, start);
		for (std::size_t qi = 0; qi < queue.size(); ++qi)
		{
			const int cx = queue[qi] % g.w;
			const int cy = queue[qi] / g.w;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const cPosition n (cx + dx, cy + dy);
					if (n.x() < 0 || n.y() < 0 || n.x() >= g.w || n.y() >= g.h) continue;
					const int o = n.y() * g.w + n.x();
					if (g.region[o] != -1 || !isTankGround (map, n)) continue;
					g.region[o] = id;
					queue.push_back (o);
				}
		}
		g.sizes.push_back (static_cast<int> (queue.size()));
	}
	return g;
}

bool startMineFits (const cStaticMap& map, const cUnitsData& unitsData, const cPosition& l)
{
	const auto& mine = unitsData.getStaticUnitData (unitsData.getMineID());
	const auto& generator = unitsData.getStaticUnitData (unitsData.getSmallGeneratorID());
	return map.possiblePlace (generator, l + cPosition (-1, 0)) && map.possiblePlace (mine, l + cPosition (0, -1));
}

// клетки стартовых зданий (как getInitialBuildingPositions в actioninitnewgame.cpp)
std::array<cPosition, 5> startBaseCells (const cPosition& l)
{
	return {l + cPosition (-1, 0), l + cPosition (0, -1), l + cPosition (1, -1), l + cPosition (1, 0), l};
}

// здания базы в l не отрезают от области ни одной её клетки рядом с базой
bool baseLeavesExit (const cStaticMap& map, const cPosition& l)
{
	const auto base = startBaseCells (l);
	const auto isBase = [&base] (const cPosition& p) { return std::find (base.begin(), base.end(), p) != base.end(); };
	const auto free = [&] (const cPosition& p) { return isTankGround (map, p) && !isBase (p); };
	std::vector<cPosition> near;
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
		{
			const cPosition p = l + cPosition (dx, dy);
			if (free (p)) near.push_back (p);
		}
	if (near.empty()) return false;
	// связность клеток рядом с базой в окне 9×9 (дальше база ничего не отрезает)
	std::vector<cPosition> seen (1, near.front());
	for (std::size_t qi = 0; qi < seen.size(); ++qi)
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition n = seen[qi] + cPosition (dx, dy);
				if (std::abs (n.x() - l.x()) > 4 || std::abs (n.y() - l.y()) > 4) continue;
				if (!free (n) || std::find (seen.begin(), seen.end(), n) != seen.end()) continue;
				seen.push_back (n);
			}
	return std::all_of (near.begin(), near.end(), [&] (const cPosition& p) { return std::find (seen.begin(), seen.end(), p) != seen.end(); });
}
} // namespace

void cLandingChooser::init (const cStaticMap& map_, const cUnitsData& unitsData_, bool fixedBridgehead_, std::vector<sLandingUnit> units_, std::uint32_t seed_)
{
	map = &map_;
	unitsData = &unitsData_;
	fixedBridgehead = fixedBridgehead_;
	units = std::move (units_);
	seed = seed_;
	candidates.clear();
	tried.clear();

	const sLandGrid g = buildLandGrid (map_);
	if (g.sizes.empty())
	{
		info = "no land for tanks";
		return;
	}
	const int region = static_cast<int> (std::max_element (g.sizes.begin(), g.sizes.end()) - g.sizes.begin());

	// простор: клеток суши танка в квадрате радиуса kRoomRadius (префиксные суммы)
	const int w = g.w;
	const int h = g.h;
	std::vector<int> sum ((w + 1) * (h + 1), 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			sum[(y + 1) * (w + 1) + x + 1] = sum[y * (w + 1) + x + 1] + sum[(y + 1) * (w + 1) + x] - sum[y * (w + 1) + x]
				+ (g.region[y * w + x] >= 0 ? 1 : 0);
	const auto roomAt = [&] (int x, int y) {
		const int x0 = std::max (0, x - kRoomRadius);
		const int y0 = std::max (0, y - kRoomRadius);
		const int x1 = std::min (w - 1, x + kRoomRadius) + 1;
		const int y1 = std::min (h - 1, y + kRoomRadius) + 1;
		return sum[y1 * (w + 1) + x1] - sum[y0 * (w + 1) + x1] - sum[y1 * (w + 1) + x0] + sum[y0 * (w + 1) + x0];
	};

	std::vector<std::pair<cPosition, int>> fit;
	int best = 0;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			if (g.region[y * w + x] != region) continue;
			const cPosition p (x, y);
			if (fixedBridgehead && !startMineFits (map_, unitsData_, p)) continue;
			const int room = roomAt (x, y);
			best = std::max (best, room);
			fit.emplace_back (p, room);
		}
	for (const auto& [p, room] : fit)
		if (room >= kRoomShare * best) candidates.push_back (p);

	info = "map " + std::to_string (w) + "x" + std::to_string (h) + ", largest land region " + std::to_string (g.sizes[region])
		+ " cells" + (g.sizes[region] < kMinRegion ? " (small: islands?)" : "") + ", roomy candidates " + std::to_string (candidates.size());
}

bool cLandingChooser::valid (const cPosition& p) const
{
	if (map == nullptr || unitsData == nullptr) return false;
	if (!cActionInitNewGame::isValidLandingPosition (p, *map, fixedBridgehead, units, *unitsData)) return false;
	if (fixedBridgehead && !baseLeavesExit (*map, p)) return false;
	return true;
}

std::optional<cPosition> cLandingChooser::first()
{
	if (candidates.empty()) return std::nullopt;
	const std::size_t n = candidates.size();
	const std::size_t start = seed % n;
	for (std::size_t i = 0; i < n; ++i)
	{
		const cPosition& p = candidates[(start + i * 7919) % n]; // шаг — простое число: обход вразброс
		if (valid (p))
		{
			tried.assign (1, p);
			return p;
		}
	}
	// обход с шагом мог пропустить клетки (n кратно 7919) — подряд
	for (const auto& p : candidates)
		if (valid (p))
		{
			tried.assign (1, p);
			return p;
		}
	return std::nullopt;
}

std::optional<cPosition> cLandingChooser::next()
{
	auto p = pickFarthest();
	if (p) tried.push_back (*p);
	return p;
}

std::optional<cPosition> cLandingChooser::pickFarthest()
{
	if (candidates.empty()) return std::nullopt;
	std::vector<std::pair<double, std::size_t>> order;
	order.reserve (candidates.size());
	for (std::size_t i = 0; i < candidates.size(); ++i)
	{
		double d = std::numeric_limits<double>::max();
		for (const auto& t : tried)
			d = std::min (d, (candidates[i] - t).l2Norm());
		order.emplace_back (d, i);
	}
	std::sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
	std::vector<std::size_t> top;
	for (const auto& [d, i] : order)
	{
		if (!valid (candidates[i])) continue;
		top.push_back (i);
		if (top.size() >= kTopFarthest) break;
	}
	if (top.empty()) return std::nullopt;
	seed = seed * 1103515245u + 12345u;
	return candidates[top[(seed >> 16) % top.size()]];
}

} // namespace ai_client
