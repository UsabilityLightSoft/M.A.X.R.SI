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
// мозг — заглушка. Этапы 3–4 подключают cRuleBrain.
//
// argv[1] = maxTurns (default 10)
// argv[2] = карта (default "Delta.wrl")
// argv[3] = каталог вывода digest/journal/metrics (default "ai_out")
// argv[4] = сид RNG (uint32, decimal). Если не задан — берётся переменная
//           окружения MAXR_SEED (тот же формат). Если нет ни того, ни другого,
//           используется фиксированный дефолт kDefaultRandomSeed из random.h —
//           т.е. прогон всё равно детерминирован.

#include "SDLutility/sdlcomponent.h"
#include "SDLutility/sdlnetcomponent.h"
#include "ai_brain.h"
#include "ai_engine.h"
#include "ai_rule_brain.h"
#include "crashreporter/debug.h"
#include "game/connectionmanager.h"
#include "game/data/gamesettings.h"
#include "game/data/map/map.h"
#include "game/data/player/clans.h"
#include "game/data/player/playerbasicdata.h"
#include "game/data/units/landingunit.h"
#include "game/data/units/unitdata.h"
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

#include <SDL.h>

#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

	// --- сид RNG: argv[4] > MAXR_SEED > фиксированный дефолт (kDefaultRandomSeed) ---
	// Вызываем ДО LoadData/сервера, чтобы ВСЕ случайные числа (в т.ч. при
	// загрузке данных) шли из одного пересиженного генератора.
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
			setRandomSeed (seed);
			std::cout << "[ai_headless] RNG seed: " << seed << " (explicit)" << std::endl;
		}
		else
		{
			std::cout << "[ai_headless] RNG seed: default kDefaultRandomSeed (fixed, "
				"reproducible)" << std::endl;
		}
	}

	std::cout << "[ai_headless] step 0: argv0=" << (argc > 0 ? argv[0] : "?") << std::endl;

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

	server->start();
	std::cout << "[ai_headless] step 7: server started" << std::endl;

	// --- высадка игроков ---
	const cPosition mapSize = staticMap->getSize();
	for (int i = 0; i < 2; ++i)
	{
		sInitPlayerData initPlayerData;
		initPlayerData.clan = i;

		const int step = (i == 0) ? 1 : -1;
		const int startX = (i == 0) ? 1 : mapSize.x() - 2;
		const int startY = (i == 0) ? 1 : mapSize.y() - 2;

		cPosition pos (startX, startY);
		bool found = false;
		// Конструктор высаживается 1x1; ищем СУШУ (не воду/блок), а не просто
		// клетку в границах — иначе на картах с водой в углах стартовая шахта
		// не встанет («couldn't place player start mine»).
		const auto& constructorData = unitsData->getStaticUnitData (unitsData->getConstructorID());
		for (int dx = 0; dx < 30 && !found; ++dx)
		{
			for (int dy = 0; dy < 30 && !found; ++dy)
			{
				cPosition candidate (std::clamp (startX + dx * step, 0, mapSize.x() - 1),
					std::clamp (startY + dy * step, 0, mapSize.y() - 1));
				if (staticMap->possiblePlace (constructorData, candidate))
				{
					pos = candidate;
					found = true;
				}
			}
		}
		initPlayerData.landingPosition = pos;

		// Стартовый набор: игра отдаёт его бесплатно по computeInitialLandingUnits;
		// без юнитов экономика стартовой шахты не устаивается и ход не завершается.
		const auto initialUnits = computeInitialLandingUnits (initPlayerData.clan, *gameSettings, *unitsData);
		initPlayerData.landingUnits.clear();
		for (const auto& [unitId, cargo] : initialUnits)
			initPlayerData.landingUnits.push_back ({unitId, cargo});

		clients[i]->initNewGame (initPlayerData);
	}
	std::cout << "[ai_headless] step 8: initNewGame sent" << std::endl;

	// --- движок ИИ (оба игрока — алгоритмический мозг) ---
	ai::cAiEngine engine (outDir);
	for (int i = 0; i < 2; ++i)
		engine.addPlayer (i, clients[i].get(), std::make_shared<ai::cRuleBrain>());

	Log.info ("ai_headless: game started, maxTurns=" + std::to_string (maxTurns));
	std::cout << "[ai_headless] step 9: driver loop start" << std::endl;

	int lastTurn = 0;
	uint32_t lastProgressTick = SDL_GetTicks();
	while (true)
	{
		// клиентские таймеры ВПЕРЁД: модель синхронизируется, движок читает свежее.
		for (auto& client : clients)
			client->run();

		const int turn = clients[0]->getModel().getTurnCounter()->getTurn();
		if (turn != lastTurn)
		{
			Log.info ("ai_headless: turn " + std::to_string (turn));
			lastTurn = turn;
			lastProgressTick = SDL_GetTicks();
		}
		if (turn >= maxTurns)
			break;

		// сторож: нет прогресса — выходим с диагностикой (лимит настраивается argv[5])
		if (SDL_GetTicks() - lastProgressTick > watchdogMs)
		{
			Log.error ("ai_headless: WATCHDOG: no progress for " + std::to_string (watchdogMs) + "ms at turn " + std::to_string (turn));
			break;
		}

		engine.run();
		SDL_Delay (2);
	}

	engine.finalize();
	server->stop();
	Log.info ("ai_headless: DONE. Turns played: " + std::to_string (lastTurn));
	return 0;
}
catch (const std::exception& ex)
{
	Log.error (std::string ("ai_headless: ") + ex.what());
	return -1;
}
