/***************************************************************************
 *      Mechanized Assault and Exploration Reloaded Projectfile            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

// ai_headless — план ИИ (PLAN_AI): партия 1v1 без GUI, игроками управляет
// cAiEngine (обвязка + «мозг»). Этап 2: дайджест/журнал/валидатор/метрики,
// мозг — заглушка. Этапы 3–4 подключают cRuleBrain, этап 5 — cLlmAdvisorBrain.
//
// argv[1] = maxTurns (default 10)
// argv[2] = карта (default "Delta.wrl")
// argv[3] = каталог вывода digest/journal/metrics (default "ai_out")
// argv[4] = сид RNG (uint32, decimal). Если не задан — берётся переменная
//           окружения MAXR_SEED (тот же формат). Если нет ни того, ни другого,
//           используется фиксированный дефолт kDefaultRandomSeed из random.h —
//           т.е. прогон всё равно детерминирован.
// argv[5] = сторож, мс без смены хода до выхода (default 900000)
// argv[6] = мозг: "rule", "base" или "llm" — обоим игрокам; "llm,rule", "rule,base" —
//           по игроку (игрок 0, игрок 1). Не задан — переменная MAXR_BRAIN, иначе "rule".
//           "base" — замороженная копия правил на 24.10.2026 (ai_base_brain.cpp), эталон
//           турниров шагов 7–12: «rule,base» мерит, насколько новые правила сильнее.
//           LLM-советник: MAXR_LLM_URL, MAXR_LLM_MODEL, MAXR_LLM_TIMEOUT_MS;
//           его журнал — <каталог вывода>/llm.jsonl, полные ответы сервера —
//           llm_replies.jsonl (первой строкой — параметры партии).
// MAXR_LLM_REPLAY=<llm_replies.jsonl | llm.jsonl> — повтор партии с LLM: советник
//           берёт ответы из записи, модель не зовётся (те же argv и MAXR_LLM_*).
// MAXR_FAST_TICK=0 — проигрывать ходы в реальном времени (по умолчанию быстрый такт).
// Партия кончается на ходе argv[1] или раньше — когда движок признал игрока
// побеждённым. Выход: digest/, journal.jsonl, metrics.json (исход и метрики D),
// metrics_turns.jsonl (ряд по ходам).

#include "SDLutility/sdlcomponent.h"
#include "SDLutility/sdlnetcomponent.h"
#include "ai_base_brain.h"
#include "ai_brain.h"
#include "ai_decision_journal.h"
#include "ai_engine.h"
#include "ai_llm_advisor.h"
#include "ai_rule_brain.h"
#include "crashreporter/debug.h"
#include "game/connectionmanager.h"
#include "game/data/gamesettings.h"
#include "game/data/map/map.h"
#include "game/data/player/clans.h"
#include "game/data/player/player.h"
#include "game/data/player/playerbasicdata.h"
#include "game/data/units/landingunit.h"
#include "game/data/units/unitdata.h"
#include "game/logic/action/actioninitnewgame.h"
#include "game/logic/client.h"
#include "game/logic/server.h"
#include "game/logic/turncounter.h"
#include "game/startup/gamepreparation.h"
#include "game/startup/initplayerdata.h"
#include "game/startup/lobbypreparationdata.h"
#include "maxrversion.h"
#include "resources/loaddata.h"
#include "settings.h"
#include "utility/log.h"
#include "utility/random.h"
#include "utility/string/utf-8.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

//------------------------------------------------------------------------------
// разобрать выбор мозга: «rule»/«base»/«llm» — обоим игрокам, «llm,rule» — по игроку
static bool parseBrains (const std::string& spec, int nPlayers, std::vector<std::string>& out, std::string& err)
{
	std::vector<std::string> items;
	std::size_t start = 0;
	for (;;)
	{
		const auto comma = spec.find (',', start);
		std::string item = spec.substr (start, comma == std::string::npos ? std::string::npos : comma - start);
		item.erase (0, item.find_first_not_of (" \t"));
		item.erase (item.find_last_not_of (" \t") + 1);
		items.push_back (item);
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
	if (items.size() == 1) items.assign (nPlayers, items.front());
	if (static_cast<int> (items.size()) != nPlayers)
	{
		err = "expected 1 or " + std::to_string (nPlayers) + " comma-separated brains, got " + std::to_string (items.size());
		return false;
	}
	for (const auto& item : items)
	{
		if (item != "rule" && item != "base" && item != "llm")
		{
			err = "unknown brain '" + item + "' (allowed: rule, base, llm)";
			return false;
		}
	}
	out = std::move (items);
	return true;
}


//------------------------------------------------------------------------------
// Высадка (PLAN.md §7 п. 13, 17). Обе стороны — в одной области суши, по которой
// ходит танк: не вода, не берег, не блок, 8 соседей, как в cPathCalculator. Танк,
// шахта и генератор на берег не встают, морского транспорта у ИИИ нет — из разных
// областей армии друг до друга не дойдут (старая высадка на Delta: два материка,
// ничьи без потерь). Точка годна, если стартовые генератор и шахта встают на месте,
// юниты высадки помещаются рядом (isValidLandingPosition) и здания базы не отрезают
// от области ни одной клетки рядом с базой (карман — Delta, угол (0,0)).
// Старый выбор (первая клетка от своего угла + сдвиг шахты движком) остаётся, если он
// годен для обоих: так Mushroom и Lava играются как прежде. Иначе — в самой большой
// области берутся «просторные» годные точки (суши в радиусе площадок шахт правил не
// меньше kRoomShare от лучшей: ресурсы движок сыплет по карте равномерно, и у базы с
// меньшим простором меньше площадок под шахты — на Delta у дальней пары без этого было
// 457 клеток суши против 643), и из них — самая дальняя по пути танка пара.
// MAXR_LANDING="x0,y0;x1,y1" — точки вручную (для опытов), проверка годности та же.
namespace
{
constexpr int kMinLandingRegion = 300;  // клеток в области на двоих; меньше (Three Isles: 185) — играть негде
constexpr int kMinLandingDistance = 20; // шагов танка между базами — не ближе
constexpr int kRoomRadius = 18;         // радиус «простора» = kMaxSiteRadius правил (площадки шахт)
constexpr double kRoomShare = 0.8;      // просторная точка — не меньше этой доли от лучшей

bool isTankGround (const cStaticMap& map, const cPosition& p)
{
	return map.isValidPosition (p) && !map.isWater (p) && !map.isCoast (p) && !map.isBlocked (p);
}

// области суши танка: номер области клетки (−1 — не суша) и размеры
struct sLandGrid
{
	int w = 0;
	int h = 0;
	std::vector<int> region;
	std::vector<int> sizes;
	mutable std::vector<int> stamp; // метки обхода baseLeavesExit
	mutable int stampValue = 0;

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
	g.stamp.assign (g.w * g.h, 0);
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

// клетки стартовых зданий (как getInitialBuildingPositions в actioninitnewgame.cpp)
std::array<cPosition, 5> startBaseCells (const cPosition& l)
{
	return {l + cPosition (-1, 0), l + cPosition (0, -1), l + cPosition (1, -1), l + cPosition (1, 0), l};
}

bool startMineFits (const cStaticMap& map, const cUnitsData& unitsData, const cPosition& l)
{
	const auto& mine = unitsData.getStaticUnitData (unitsData.getMineID());
	const auto& generator = unitsData.getStaticUnitData (unitsData.getSmallGeneratorID());
	return map.possiblePlace (generator, l + cPosition (-1, 0)) && map.possiblePlace (mine, l + cPosition (0, -1));
}

// куда движок сдвинет высадку ради шахты (копия cActionInitNewGame::findPositionForStartMine)
bool shiftForStartMine (const cStaticMap& map, const cUnitsData& unitsData, cPosition& pos)
{
	for (int radius = 0; radius < 3; ++radius)
		for (int offY = -radius; offY <= radius; ++offY)
			for (int offX = -radius; offX <= radius; ++offX)
			{
				const cPosition place = pos + cPosition (offX, offY);
				if (startMineFits (map, unitsData, place))
				{
					pos = place;
					return true;
				}
			}
	return false;
}

// путь танка (шагов) от from до клеток её области; −1 — не дойти
std::vector<int> tankDistances (const sLandGrid& g, const cPosition& from)
{
	std::vector<int> dist (g.w * g.h, -1);
	const int r = g.at (from);
	if (r < 0) return dist;
	std::vector<int> queue (1, from.y() * g.w + from.x());
	dist[queue.front()] = 0;
	for (std::size_t qi = 0; qi < queue.size(); ++qi)
	{
		const int cx = queue[qi] % g.w;
		const int cy = queue[qi] / g.w;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition n (cx + dx, cy + dy);
				if (g.at (n) != r) continue;
				const int o = n.y() * g.w + n.x();
				if (dist[o] != -1) continue;
				dist[o] = dist[queue[qi]] + 1;
				queue.push_back (o);
			}
	}
	return dist;
}

// здания базы в l не отрезают от области ни одной её клетки: всё отрезанное касается
// базы, значит лежит не дальше 2 клеток от l, — эти клетки должны остаться связаны
bool baseLeavesExit (const sLandGrid& g, const cPosition& l)
{
	const int r = g.at (l);
	if (r < 0) return false;
	const auto base = startBaseCells (l);
	const auto isBase = [&base] (const cPosition& p) { return std::find (base.begin(), base.end(), p) != base.end(); };
	std::size_t nearCount = 0;
	cPosition first;
	for (int dy = -2; dy <= 2; ++dy)
		for (int dx = -2; dx <= 2; ++dx)
		{
			const cPosition p = l + cPosition (dx, dy);
			if (g.at (p) != r || isBase (p)) continue;
			if (nearCount++ == 0) first = p;
		}
	if (nearCount == 0) return false;
	++g.stampValue;
	std::size_t found = 0;
	const auto visit = [&] (const cPosition& p) {
		g.stamp[p.y() * g.w + p.x()] = g.stampValue;
		if (std::max (std::abs (p.x() - l.x()), std::abs (p.y() - l.y())) <= 2) ++found;
	};
	std::vector<cPosition> queue (1, first);
	visit (first);
	for (std::size_t qi = 0; qi < queue.size() && found < nearCount; ++qi)
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const cPosition n = queue[qi] + cPosition (dx, dy);
				if (g.at (n) != r || isBase (n) || g.stamp[n.y() * g.w + n.x()] == g.stampValue) continue;
				visit (n);
				queue.push_back (n);
			}
	return found == nearCount;
}

// клеток суши (танку, любой области) в квадрате радиуса kRoomRadius вокруг каждой клетки
std::vector<int> landRoom (const cStaticMap& map)
{
	const int w = map.getSize().x();
	const int h = map.getSize().y();
	std::vector<int> sum ((w + 1) * (h + 1), 0); // префиксные суммы
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			sum[(y + 1) * (w + 1) + x + 1] = sum[y * (w + 1) + x + 1] + sum[(y + 1) * (w + 1) + x] - sum[y * (w + 1) + x]
				+ (isTankGround (map, cPosition (x, y)) ? 1 : 0);
	std::vector<int> room (w * h, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			const int x0 = std::max (0, x - kRoomRadius);
			const int y0 = std::max (0, y - kRoomRadius);
			const int x1 = std::min (w - 1, x + kRoomRadius) + 1;
			const int y1 = std::min (h - 1, y + kRoomRadius) + 1;
			room[y * w + x] = sum[y1 * (w + 1) + x1] - sum[y0 * (w + 1) + x1] - sum[y1 * (w + 1) + x0] + sum[y0 * (w + 1) + x0];
		}
	return room;
}

struct sLandingPlan
{
	std::array<cPosition, 2> send{};    // что отдать initNewGame
	std::array<cPosition, 2> landing{}; // где встанет база
	std::string how;                    // "corner" — старый выбор, "roomy" — дальняя просторная пара, "env" — MAXR_LANDING
	std::string error;                  // не пусто — высадка невозможна
	int regionSize = 0;
	int distance = 0;
	std::array<int, 2> room{};          // простор у баз (клеток суши в радиусе kRoomRadius)
};

std::string posText (const cPosition& p)
{
	return "(" + std::to_string (p.x()) + "," + std::to_string (p.y()) + ")";
}

sLandingPlan planLanding (const cStaticMap& map, const cUnitsData& unitsData, bool fixedBridgehead,
                          const std::array<std::vector<sLandingUnit>, 2>& units)
{
	const sLandGrid g = buildLandGrid (map);
	const std::vector<int> room = landRoom (map);
	const cPosition mapSize = map.getSize();
	const auto good = [&] (const cPosition& l, int player) {
		return g.at (l) >= 0 && g.sizes[g.at (l)] >= kMinLandingRegion && startMineFits (map, unitsData, l)
			&& cActionInitNewGame::isValidLandingPosition (l, map, fixedBridgehead, units[player], unitsData)
			&& baseLeavesExit (g, l);
	};
	const auto roomAt = [&] (const cPosition& l) { return room[l.y() * g.w + l.x()]; };
	sLandingPlan plan;

	// 0. точки вручную: MAXR_LANDING="x0,y0;x1,y1"
	if (const char* env = std::getenv ("MAXR_LANDING"); env != nullptr && *env != 0)
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		if (std::sscanf (env, "%d,%d;%d,%d", &x0, &y0, &x1, &y1) != 4)
		{
			plan.error = std::string ("MAXR_LANDING='") + env + "': expected x0,y0;x1,y1";
			return plan;
		}
		plan.landing = {cPosition (x0, y0), cPosition (x1, y1)};
		plan.send = plan.landing;
		for (int i = 0; i < 2; ++i)
			if (!map.isValidPosition (plan.landing[i]) || !good (plan.landing[i], i))
			{
				plan.error = "MAXR_LANDING: p" + std::to_string (i) + "=" + posText (plan.landing[i])
					+ " is not a valid landing place (start mine, units, exit, land region)";
				return plan;
			}
		if (g.at (plan.landing[0]) != g.at (plan.landing[1]))
		{
			plan.error = "MAXR_LANDING: the points are in different land regions";
			return plan;
		}
		plan.how = "env";
		plan.regionSize = g.sizes[g.at (plan.landing[0])];
		plan.distance = tankDistances (g, plan.landing[0])[plan.landing[1].y() * g.w + plan.landing[1].x()];
		plan.room = {roomAt (plan.landing[0]), roomAt (plan.landing[1])};
		return plan;
	}

	// 1. старый выбор: первая клетка под конструктор от своего угла (игрок 0 — левый
	// верхний, игрок 1 — правый нижний), движок сдвигает её ради шахты
	{
		const auto& constructorData = unitsData.getStaticUnitData (unitsData.getConstructorID());
		bool ok = true;
		for (int i = 0; i < 2 && ok; ++i)
		{
			const int step = (i == 0) ? 1 : -1;
			const int startX = (i == 0) ? 1 : mapSize.x() - 2;
			const int startY = (i == 0) ? 1 : mapSize.y() - 2;
			cPosition pos (startX, startY);
			bool found = false;
			for (int dx = 0; dx < 30 && !found; ++dx)
				for (int dy = 0; dy < 30 && !found; ++dy)
				{
					const cPosition candidate (std::clamp (startX + dx * step, 0, mapSize.x() - 1),
						std::clamp (startY + dy * step, 0, mapSize.y() - 1));
					if (map.possiblePlace (constructorData, candidate))
					{
						pos = candidate;
						found = true;
					}
				}
			plan.send[i] = pos;
			plan.landing[i] = pos;
			ok = found && shiftForStartMine (map, unitsData, plan.landing[i]) && good (plan.landing[i], i);
		}
		if (ok && g.at (plan.landing[0]) == g.at (plan.landing[1]))
		{
			plan.distance = tankDistances (g, plan.landing[0])[plan.landing[1].y() * g.w + plan.landing[1].x()];
			if (plan.distance >= kMinLandingDistance)
			{
				plan.how = "corner";
				plan.regionSize = g.sizes[g.at (plan.landing[0])];
				plan.room = {roomAt (plan.landing[0]), roomAt (plan.landing[1])};
				return plan;
			}
		}
	}

	// 2. самая дальняя пара просторных годных точек в самой большой области
	if (g.sizes.empty())
	{
		plan.error = "no land for tanks on the map";
		return plan;
	}
	const int region = static_cast<int> (std::max_element (g.sizes.begin(), g.sizes.end()) - g.sizes.begin());
	plan.regionSize = g.sizes[region];
	if (g.sizes[region] < kMinLandingRegion)
	{
		plan.error = "largest land region reachable by tanks has " + std::to_string (g.sizes[region]) + " cells, need "
			+ std::to_string (kMinLandingRegion) + " for both players (islands: the AI has no sea or air transport)";
		return plan;
	}
	std::vector<cPosition> valid;
	for (int o = 0; o < g.w * g.h; ++o)
	{
		const cPosition l (o % g.w, o / g.w);
		if (g.region[o] == region && good (l, 0) && good (l, 1)) valid.push_back (l);
	}
	if (valid.size() < 2)
	{
		plan.error = "only " + std::to_string (valid.size()) + " valid landing places in the largest land region";
		return plan;
	}
	{
		int best = 0;
		for (const auto& l : valid) best = std::max (best, roomAt (l));
		std::vector<cPosition> roomy;
		for (const auto& l : valid)
			if (roomAt (l) >= kRoomShare * best) roomy.push_back (l);
		valid = std::move (roomy);
	}
	// дальняя пара — два прохода «самая дальняя от»; при равенстве — первая по порядку клеток
	const auto farthest = [&] (const cPosition& from) {
		const auto dist = tankDistances (g, from);
		cPosition best = valid.front();
		int bestDist = -1;
		for (const auto& l : valid)
		{
			const int d = dist[l.y() * g.w + l.x()];
			if (d > bestDist)
			{
				bestDist = d;
				best = l;
			}
		}
		return std::make_pair (best, bestDist);
	};
	const cPosition a = farthest (valid.front()).first;
	const auto [b, distance] = farthest (a);
	if (distance < kMinLandingDistance)
	{
		plan.error = "roomy landing places in the largest land region are at most " + std::to_string (distance)
			+ " tank steps apart, need " + std::to_string (kMinLandingDistance);
		return plan;
	}
	const bool aFirst = a.y() * g.w + a.x() < b.y() * g.w + b.x();
	plan.landing = {aFirst ? a : b, aFirst ? b : a};
	plan.send = plan.landing; // шахта встаёт на месте — движок точку не сдвинет
	plan.distance = distance;
	plan.room = {roomAt (plan.landing[0]), roomAt (plan.landing[1])};
	plan.how = "roomy";
	return plan;
}
} // namespace

//------------------------------------------------------------------------------
int main (int argc, char** argv)
try
{
	// SDL_main в GUI-сборках делает это сам; у консольного раннера своего
	// main нет — без setlocale libstdc++ конвертирует wide-пути через локаль
	// «C», и кириллица в пути падает с «Cannot convert character sequence».
	setlocale (LC_ALL, "");
	std::locale::global (std::locale (""));

	const int maxTurns = argc > 1 ? std::atoi (argv[1]) : 10;
	const std::string mapFile = argc > 2 ? argv[2] : "Delta.wrl";
	const std::string outDir = argc > 3 ? argv[3] : "ai_out";
	// сторож: сколько мс без смены хода терпим до выхода. Дефолт 900000 (15 мин):
	// на больших армиях проигрывание хода идёт в real-time (~9000 тиков × 10 мс ≈
	// 90 с на ход), и 90-секундный лимит обрывал партию на ходе ~127.
	const uint32_t watchdogMs = argc > 5 ? static_cast<uint32_t> (std::strtoul (argv[5], nullptr, 10)) : 900000u;

	// --- мозги игроков: argv[6] > MAXR_BRAIN > rule ---
	constexpr int kPlayers = 2;
	std::vector<std::string> brains;
	{
		std::string spec = "rule";
		std::string source = "default";
		if (argc > 6 && argv[6][0] != '\0')
		{
			spec = argv[6];
			source = "argv[6]";
		}
		else if (const char* envBrain = std::getenv ("MAXR_BRAIN"); envBrain != nullptr && envBrain[0] != '\0')
		{
			spec = envBrain;
			source = "MAXR_BRAIN";
		}
		std::string err;
		if (!parseBrains (spec, kPlayers, brains, err))
		{
			std::cout << "[ai_headless] ERROR: brain '" << spec << "' from " << source << ": " << err
				<< ". Usage: rule | base | llm | rule,base | llm,rule" << std::endl;
			return -1;
		}
		std::cout << "[ai_headless] brains (" << source << "):";
		for (int i = 0; i < kPlayers; ++i)
			std::cout << " p" << i << "=" << brains[i];
		std::cout << std::endl;
	}
	const bool anyLlm = std::find (brains.begin(), brains.end(), "llm") != brains.end();
	const ai::sLlmConfig llmConfig = ai::sLlmConfig::fromEnv();
	if (anyLlm)
	{
		std::cout << "[ai_headless] llm: url=" << llmConfig.url
			<< " model=" << (llmConfig.model.empty() ? "(not set)" : llmConfig.model)
			<< " timeoutMs=" << llmConfig.timeoutMs << std::endl;
		if (!llmConfig.note.empty())
			std::cout << "[ai_headless] llm: WARNING " << llmConfig.note << std::endl;
	}

	// --- сид RNG: argv[4] > MAXR_SEED > фиксированный дефолт (kDefaultRandomSeed) ---
	// Вызываем ДО LoadData/сервера, чтобы ВСЕ случайные числа (в т.ч. при
	// загрузке данных) шли из одного пересиженного генератора.
	std::string seedText = "default";
	{
		bool haveSeed = false;
		uint32_t seed = 0;
		if (argc > 4)
		{
			seed = static_cast<uint32_t> (std::strtoul (argv[4], nullptr, 10));
			haveSeed = true;
		}
		else if (const char* envSeed = std::getenv ("MAXR_SEED"))
		{
			seed = static_cast<uint32_t> (std::strtoul (envSeed, nullptr, 10));
			haveSeed = true;
		}
		if (haveSeed)
		{
			seedText = std::to_string (seed);
			setRandomSeed (seed);
			std::cout << "[ai_headless] RNG seed: " << seed << " (explicit)" << std::endl;
		}
		else
		{
			std::cout << "[ai_headless] RNG seed: default kDefaultRandomSeed (fixed, "
				"reproducible)" << std::endl;
		}
	}

	std::string brainsText;
	for (int i = 0; i < kPlayers; ++i)
		brainsText += (i == 0 ? "" : ",") + brains[i];
	// параметры партии — первой строкой llm_replies.jsonl: по ним партию повторяют
	const nlohmann::json gameInfo = {{"event", "game"}, {"turns", maxTurns}, {"map", mapFile}, {"seed", seedText},
		{"watchdogMs", watchdogMs}, {"brains", brainsText}};
	std::shared_ptr<ai::cLlmReplay> llmReplay;
	if (anyLlm && !llmConfig.replay.empty())
	{
		llmReplay = std::make_shared<ai::cLlmReplay>();
		std::string err;
		if (!llmReplay->load (llmConfig.replay, err))
		{
			std::cout << "[ai_headless] ERROR: MAXR_LLM_REPLAY: " << err << std::endl;
			return -1;
		}
		std::cout << "[ai_headless] llm: REPLAY from " << llmConfig.replay << ": " << llmReplay->size()
			<< " recorded calls, the model is not called" << std::endl;
		std::string diff;
		for (const char* key : {"turns", "map", "seed", "brains"})
			if (llmReplay->getGame().is_object() && llmReplay->getGame().contains (key) && llmReplay->getGame()[key] != gameInfo[key])
				diff += std::string (diff.empty() ? "" : ", ") + key + "=" + gameInfo[key].dump() + " (recorded " + llmReplay->getGame()[key].dump() + ")";
		if (!diff.empty())
			std::cout << "[ai_headless] llm: REPLAY WARNING: game differs from the recording: " << diff << std::endl;
	}

	std::cout << "[ai_headless] step 0: argv0=" << (argc > 0 ? argv[0] : "?") << std::endl;

	// Логи движка — в каталог партии, до cSettings: иначе все партии пишут один
	// <Документы>/maxr/maxr.log (открывается с обнулением — параллельные партии турнира
	// затирают друг друга), а net-лог (с debug — 7–14 МБ на партию) копится в
	// <Документы>/maxr/log_files. cLog::setLogPath уже открытый файл не переоткрывает,
	// поэтому пути из cSettings::setPaths для headless не действуют.
	const std::string engineLogPath = outDir + "/maxr.log";
	const std::string netLogPath = outDir + "/net.log";
	{
		std::error_code ec;
		std::filesystem::create_directories (outDir, ec);
		Log.setLogPath (engineLogPath);
		NetLog.setLogPath (netLogPath);
	}

	try
	{
		cSettings& settings = cSettings::getInstance();
		if (!settings.isInitialized())
		{
			Log.error ("ai_headless: Settings not initialized!");
			return -1;
		}
		// headless-прогоны: автосейв каждый ход пишет слот 10 и гоняет
		// save+reload-самопроверку (шум в логе, лишний ввод-вывод, источник гонки).
		settings.setAutosave (false);
	}
	catch (const std::exception& ex)
	{
		std::cout << "[ai_headless] settings FAILED: " << ex.what() << std::endl;
		return -1;
	}
	std::cout << "[ai_headless] step 1: settings OK" << std::endl;
	logMAXRVersion();
	CR_INIT_CRASHREPORTING();
	std::cout << "[ai_headless] step 2: version logged" << std::endl;

	auto sdlComponent = std::make_shared<SDLComponent> (false);
	auto sdlNetComponent = std::make_shared<SDLNetComponent> (sdlComponent);
	std::cout << "[ai_headless] step 3: SDL init OK" << std::endl;

	if (LoadData (false) == eLoadingState::Error)
	{
		Log.error ("ai_headless: Error while loading data!");
		return -1;
	}
	std::cout << "[ai_headless] step 4: LoadData OK" << std::endl;

	// --- game settings: пошаговый режим, победа по ходам, без таймера хода ---
	auto gameSettings = std::make_shared<cGameSettings>();
	gameSettings->gameType = eGameSettingsGameType::Turns;
	gameSettings->victoryConditionType = eGameSettingsVictoryCondition::Turns;
	gameSettings->victoryTurns = maxTurns + 1;
	gameSettings->turnEndDeadlineActive = false;

	// --- карта ---
	auto staticMap = std::make_shared<cStaticMap>();
	if (!staticMap->loadMap (mapFile))
	{
		Log.error ("ai_headless: Map not found: " + mapFile);
		return -1;
	}
	std::cout << "[ai_headless] step 5: map loaded OK" << std::endl;

	// --- высадка: юниты (бесплатный стартовый набор) и точки — до старта сервера,
	// чтобы на негодной карте падать сразу, а не ждать сторожа ---
	// Стартовый набор: игра отдаёт его бесплатно по computeInitialLandingUnits;
	// без юнитов экономика стартовой шахты не устаивается и ход не завершается.
	std::array<std::vector<sLandingUnit>, 2> landingUnits;
	for (int i = 0; i < 2; ++i)
		for (const auto& [unitId, cargo] : computeInitialLandingUnits (i, *gameSettings, UnitsDataGlobal))
			landingUnits[i].push_back ({unitId, static_cast<unsigned int> (cargo)});
	const sLandingPlan landing = planLanding (*staticMap, UnitsDataGlobal,
		gameSettings->bridgeheadType == eGameSettingsBridgeheadType::Definite, landingUnits);
	if (!landing.error.empty())
	{
		std::cout << "[ai_headless] ERROR: map " << mapFile << " is not playable: " << landing.error << std::endl;
		Log.error ("ai_headless: map " + mapFile + " is not playable: " + landing.error);
		return -1;
	}
	std::cout << "[ai_headless] landing (" << landing.how << "): p0=" << posText (landing.landing[0])
		<< " p1=" << posText (landing.landing[1]) << ", tank path " << landing.distance << " steps, land region "
		<< landing.regionSize << " cells, room " << landing.room[0] << "/" << landing.room[1] << std::endl;

	// --- игроки ---
	std::vector<cPlayerBasicData> playersBasicData;
	for (int i = 0; i < 2; ++i)
	{
		auto player = cPlayerBasicData::fromSettings();
		player.setNr (i);
		player.setName ("AI_" + std::to_string (i + 1));
		playersBasicData.push_back (std::move (player));
	}

	// --- сервер + клиенты (разводка как в hotseat, без GUI) ---
	auto connectionManager = std::make_shared<cConnectionManager>();
	auto unitsData = std::make_shared<const cUnitsData> (UnitsDataGlobal);
	auto clanData = std::make_shared<const cClanData> (ClanDataGlobal);

	auto server = std::make_unique<cServer> (connectionManager);
	server->setPreparationData ({unitsData, clanData, gameSettings, staticMap});
	server->setPlayers (playersBasicData);

	std::vector<std::shared_ptr<cClient>> clients (2);
	for (int i = 0; i < 2; ++i)
	{
		clients[i] = std::make_shared<cClient> (connectionManager);
		clients[i]->setPreparationData ({unitsData, clanData, gameSettings, staticMap});
		clients[i]->setPlayers (playersBasicData, i);
	}

	connectionManager->setLocalServer (server.get());
	std::vector<INetMessageReceiver*> localClients = {clients[0].get(), clients[1].get()};
	connectionManager->setLocalClients (std::move (localClients));

	std::cout << "[ai_headless] step 6: wiring OK" << std::endl;

	// --- детерминированный такт (headless): сервер НЕ двигает модель, пока
	// активный игрок не сдал ход; после сдачи ход проигрывается детерминированно.
	// Клиенту запрещаем уходить в WaitForServer (иначе endTurn блокируется).
	server->setDeterministicTick (true);
	for (auto& client : clients)
		client->setDeterministicTick (true);

	// Зависший конец хода (машина на ходу стала стрелком реактивного огня: задания
	// движения и атаки по очереди поворачивают её навсегда) развязывается одинаково
	// во всех моделях; обычные концы хода не меняются (см. cModel::setResolveStalledTurnEnd).
	server->setResolveStalledTurnEnd (true);
	for (auto& client : clients)
		client->setResolveStalledTurnEnd (true);

	// --- быстрый такт (headless): сданный ход проигрывается так быстро, как
	// успевают клиенты, а не по 10 мс реального времени на тик. Результат тот же:
	// модель всё равно стоит, пока игрок думает, и тики считаются те же.
	// MAXR_FAST_TICK=0 — прежний режим реального времени (для сверки).
	bool fastTick = true;
	if (const char* envFast = std::getenv ("MAXR_FAST_TICK"); envFast != nullptr && std::string (envFast) == "0")
		fastTick = false;
	server->setFastTick (fastTick);
	for (auto& client : clients)
		client->setFastTick (fastTick);
	std::cout << "[ai_headless] tick: deterministic, " << (fastTick ? "fast" : "real-time")
		<< "; logs: " << engineLogPath << ", " << netLogPath
		<< " (settings net log name, unused here: " << utf8::to_string (cSettings::getInstance().getNetLogPath().filename()) << ")" << std::endl;

	server->start();
	std::cout << "[ai_headless] step 7: server started" << std::endl;

	// --- высадка игроков (точки выбраны после загрузки карты, planLanding) ---
	for (int i = 0; i < 2; ++i)
	{
		sInitPlayerData initPlayerData;
		initPlayerData.clan = i;
		initPlayerData.landingPosition = landing.send[i];
		initPlayerData.landingUnits = landingUnits[i];
		clients[i]->initNewGame (initPlayerData);
	}
	std::cout << "[ai_headless] step 8: initNewGame sent" << std::endl;

	// --- движок ИИ: мозг каждого игрока — по выбору (rule по умолчанию) ---
	ai::cAiEngine engine (outDir);
	std::shared_ptr<ai::cDecisionJournal> llmJournal; // один llm.jsonl на всех LLM-игроков
	std::shared_ptr<ai::cDecisionJournal> llmReplies; // полные ответы сервера (llm_replies.jsonl)
	if (anyLlm)
	{
		llmJournal = std::make_shared<ai::cDecisionJournal> (outDir + "/llm.jsonl");
		if (!llmJournal->open())
			Log.error ("ai_headless: cannot open " + llmJournal->getPath());
		llmReplies = std::make_shared<ai::cDecisionJournal> (outDir + "/llm_replies.jsonl");
		if (!llmReplies->open())
			Log.error ("ai_headless: cannot open " + llmReplies->getPath());
		llmReplies->log (gameInfo);
	}
	for (int i = 0; i < kPlayers; ++i)
	{
		if (brains[i] == "llm")
			engine.addPlayer (i, clients[i].get(), std::make_shared<ai::cLlmAdvisorBrain> (llmJournal, llmConfig, llmReplies, llmReplay));
		else if (brains[i] == "base")
			engine.addPlayer (i, clients[i].get(), std::make_shared<ai::cBaseRuleBrain>());
		else
			engine.addPlayer (i, clients[i].get(), std::make_shared<ai::cRuleBrain>());
	}

	Log.info ("ai_headless: game started, maxTurns=" + std::to_string (maxTurns));

	// Первый ход ИИ — только когда высадка ВСЕХ игроков дошла до ВСЕХ клиентских
	// моделей: иначе дайджест хода 1 зависел бы от того, успел ли клиент принять
	// высадку соперника (гонка потоков сервера и драйвера).
	{
		const auto allLanded = [&]() {
			for (const auto& client : clients)
				for (const auto& player : client->getModel().getPlayerList())
					if (player->getLandingPos() == cPosition (-1, -1)) return false;
			return true;
		};
		// высадка доходит за миллисекунды; не дошла — движок её отклонил (net.log:
		// «couldn't place player start mine», «Landing failed»), ждать сторожа незачем
		const uint32_t landingTimeoutMs = std::min<uint32_t> (watchdogMs, 60000u);
		const uint32_t landingStart = SDL_GetTicks();
		while (!allLanded())
		{
			if (SDL_GetTicks() - landingStart > landingTimeoutMs)
			{
				std::cout << "[ai_headless] ERROR: landing not applied in " << landingTimeoutMs
					<< " ms (rejected by the engine? see " << netLogPath << ")" << std::endl;
				Log.error ("ai_headless: landing not applied in " + std::to_string (landingTimeoutMs) + "ms");
				server->stop();
				return -1;
			}
			SDL_Delay (1);
			for (auto& client : clients)
				client->run();
		}
	}
	std::cout << "[ai_headless] step 9: driver loop start" << std::endl;

	int lastTurn = 0;
	uint32_t lastProgressTick = SDL_GetTicks();
	ai::eGameEnd gameEnd = ai::eGameEnd::TurnLimit;
	while (true)
	{
		// клиентские таймеры ВПЕРЁД: модель синхронизируется, движок читает свежее.
		const unsigned int gameTimeBefore = clients[0]->getModel().getGameTime();
		for (auto& client : clients)
			client->run();
		// быстрый такт: пока ход проигрывается, не спим — сервер ждёт подтверждений клиентов
		const bool advanced = clients[0]->getModel().getGameTime() != gameTimeBefore;

		const int turn = clients[0]->getModel().getTurnCounter()->getTurn();
		if (turn != lastTurn)
		{
			Log.info ("ai_headless: turn " + std::to_string (turn));
			lastTurn = turn;
			lastProgressTick = SDL_GetTicks();
		}
		// Движок на начале круга признал игрока побеждённым (нет юнитов, способных
		// стрелять или строить) — партия решена. Дальше она и не идёт: EndTurn
		// побеждённого движок игнорирует.
		if (engine.isAnyPlayerDefeated())
		{
			gameEnd = ai::eGameEnd::Elimination;
			break;
		}
		if (turn >= maxTurns)
			break;

		// сторож: нет прогресса — выходим с диагностикой (лимит настраивается argv[5])
		if (SDL_GetTicks() - lastProgressTick > watchdogMs)
		{
			Log.error ("ai_headless: WATCHDOG: no progress for " + std::to_string (watchdogMs) + "ms at turn " + std::to_string (turn));
			gameEnd = ai::eGameEnd::Watchdog;
			break;
		}

		engine.run();
		if (!fastTick || !advanced)
			SDL_Delay (2);
	}

	const ai::sGameResult result = engine.finalize (gameEnd, lastTurn);
	std::cout << "[ai_headless] game end: " << result.endReason << " at turn " << result.endTurn
		<< ", winner " << result.winner << " (" << result.winReason << ")" << std::endl;
	if (llmJournal) llmJournal->close();
	if (llmReplies) llmReplies->close();
	server->stop();
	Log.info ("ai_headless: DONE. Turns played: " + std::to_string (lastTurn));
	return 0;
}
catch (const std::exception& ex)
{
	Log.error (std::string ("ai_headless: ") + ex.what());
	return -1;
}
