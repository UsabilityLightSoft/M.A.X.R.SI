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

// maxr_ai_client — клиент ИИИ (шаг 15 PLAN.md): входит в сетевую игру MAXR как обычный
// игрок (TCP/IP), сам проходит лобби, клан и высадку и играет мозгом ИИИ (rule, base2, llm)
// в режиме «Turn based». Хост — окно игры («TCP/IP Host») или выделенный сервер.
// Ход — в несколько заходов (шаг 11), если хост их понимает (проба в первом ходу), иначе один.
// Пишет в каталог партии: journal.jsonl, digest/, metrics.json, metrics_turns.jsonl (как
// ai_headless), journal_human.jsonl — действия других игроков теми же приказами, снимки
// digest/ на начале их ходов, client.json — итог, maxr.log и net.log.
// Запуск без аргументов — к 127.0.0.1:58600, мозг rule; --help — все ключи.

#include "SDLutility/sdlcomponent.h"
#include "SDLutility/sdlnetcomponent.h"
#include "ai_base2_rule_brain.h"
#include "ai_base_brain.h"
#include "ai_brain.h"
#include "ai_client_console.h"
#include "ai_client_landing.h"
#include "ai_client_observer.h"
#include "ai_decision_journal.h"
#include "ai_engine.h"
#include "ai_llm_advisor.h"
#include "ai_rule_brain.h"
#include "crashreporter/debug.h"
#include "defines.h"
#include "game/connectionmanager.h"
#include "game/data/gamesettings.h"
#include "game/data/map/map.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/player/playerbasicdata.h"
#include "game/data/report/savedreportchat.h"
#include "game/data/savegameinfo.h"
#include "game/data/units/landingunit.h"
#include "game/data/units/unitdata.h"
#include "game/logic/client.h"
#include "game/logic/gametimer.h"
#include "game/logic/landingpositionstate.h"
#include "game/logic/turncounter.h"
#include "game/networkaddress.h"
#include "game/protocol/lobbymessage.h"
#include "game/protocol/netmessage.h"
#include "game/startup/gamepreparation.h"
#include "game/startup/initplayerdata.h"
#include "game/startup/lobbyclient.h"
#include "maxrversion.h"
#include "resources/loaddata.h"
#include "resources/playercolor.h"
#include "settings.h"
#include "utility/log.h"
#include "utility/os.h"
#include "utility/string/utf-8.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

std::string gNetLogPath; // свой net.log (cLog открывает файл один раз за процесс — при --loop общий)

//------------------------------------------------------------------------------
struct sOptions
{
	std::string host = "127.0.0.1";
	int port = DEFAULTPORT;
	std::string brain = "rule";        // rule | base2 | base | llm
	std::string name;                  // пусто — «AI <мозг>»
	int clan = 0;                      // -1 — случайный
	std::string passes = "auto";       // auto — проба в первом ходу; 1..50 — как MAXR_PASSES
	std::string outDir;                // пусто — <папка exe>/ai_games/<дата-время>_<имя>
	int waitSec = 1800;                // сколько ждать, пока хост откроет игру
	std::uint32_t seed = 0;            // для выбора высадки и клана; 0 — от часов
	bool keepRunning = false;          // после партии — снова ждать игру (для ярлыка «одиночная»)
	// --setup: клиент сам настраивает игру (выделенный сервер): карта, «Turn based», без таймеров, старт
	bool setup = false;
	std::string map = "Mushroom.wrl";
	int players = 2;                   // --setup: стартовать, когда в лобби столько игроков и все готовы
	int victoryTurns = 0;              // --setup: победа по ходам (0 — до уничтожения)
	int quitAtTurn = 0;                // выйти из партии на этом ходу (проверки; 0 — играть до конца)
};

void printUsage()
{
	std::cout <<
		"maxr_ai_client — ИИИ входит в сетевую игру MAXR как игрок (нужен режим Turn based).\n"
		"  --host <ip>        адрес хоста (по умолчанию 127.0.0.1)\n"
		"  --port <n>         порт (58600)\n"
		"  --brain <b>        rule (умолчание) | base2 | base | llm\n"
		"  --name <имя>       имя в лобби (AI <мозг>)\n"
		"  --clan <0..7|random>  клан (0 — The Chosen)\n"
		"  --passes <auto|1..50> заходов за ход: auto — если хост понимает (проба), иначе 1\n"
		"  --out <каталог>    куда писать журналы партии\n"
		"  --wait <сек>       сколько ждать хоста (1800)\n"
		"  --seed <n>         сид выбора высадки\n"
		"  --loop             после партии снова ждать новую игру\n"
		"  --setup            настроить игру самому (выделенный сервер): --map, --players, --victory-turns\n"
		"  --quit-at-turn <n> выйти из партии на ходу n (для проверок; итог — по материалу, как ai_headless)\n";
}

bool parseArgs (int argc, char** argv, sOptions& o, std::string& err)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		const auto value = [&]() -> std::string {
			if (i + 1 >= argc)
			{
				err = "missing value after " + a;
				return {};
			}
			return argv[++i];
		};
		if (a == "--help" || a == "-h" || a == "/?") { printUsage(); std::exit (0); }
		else if (a == "--host") o.host = value();
		else if (a == "--port") o.port = std::atoi (value().c_str());
		else if (a == "--brain") o.brain = value();
		else if (a == "--name") o.name = value();
		else if (a == "--clan")
		{
			const std::string v = value();
			o.clan = (v == "random") ? -1 : std::atoi (v.c_str());
		}
		else if (a == "--passes") o.passes = value();
		else if (a == "--out") o.outDir = value();
		else if (a == "--wait") o.waitSec = std::atoi (value().c_str());
		else if (a == "--seed") o.seed = static_cast<std::uint32_t> (std::strtoul (value().c_str(), nullptr, 10));
		else if (a == "--loop") o.keepRunning = true;
		else if (a == "--setup") o.setup = true;
		else if (a == "--map") o.map = value();
		else if (a == "--players") o.players = std::atoi (value().c_str());
		else if (a == "--victory-turns") o.victoryTurns = std::atoi (value().c_str());
		else if (a == "--quit-at-turn") o.quitAtTurn = std::atoi (value().c_str());
		else
		{
			err = "unknown argument " + a;
			return false;
		}
		if (!err.empty()) return false;
	}
	if (o.brain != "rule" && o.brain != "base2" && o.brain != "base" && o.brain != "llm")
	{
		err = "unknown brain '" + o.brain + "' (rule, base2, base, llm)";
		return false;
	}
	if (o.passes != "auto")
	{
		const int n = std::atoi (o.passes.c_str());
		if (n < 1 || n > 50)
		{
			err = "--passes: auto or 1..50";
			return false;
		}
	}
	if (o.clan < -1 || o.clan > 7)
	{
		err = "--clan: 0..7 or random";
		return false;
	}
	if (o.port <= 0 || o.port > 65535)
	{
		err = "--port: 1..65535";
		return false;
	}
	if (o.name.empty()) o.name = "AI " + o.brain;
	return true;
}

std::string timeStamp (const char* format)
{
	const std::time_t t = std::time (nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s (&tm, &t);
#else
	localtime_r (&t, &tm);
#endif
	char buf[64];
	std::strftime (buf, sizeof (buf), format, &tm);
	return buf;
}

std::string safeName (const std::string& s)
{
	std::string out;
	for (const unsigned char c : s)
		out += (std::isalnum (c) || c == '-' || c == '_') ? static_cast<char> (c) : '_';
	return out.empty() ? std::string ("ai") : out;
}

void say (const std::string& text)
{
	std::cout << "[" << timeStamp ("%H:%M:%S") << "] " << text << std::endl;
	Log.info ("ai_client: " + text);
}

bool sameColor (const cRgbColor& a, const cRgbColor& b)
{
	return a.toLab().deltaE (b.toLab()) < 10.; // как lobbyutils.cpp
}

const char* landingStateText (eLandingPositionState s)
{
	switch (s)
	{
		case eLandingPositionState::Clear: return "Clear";
		case eLandingPositionState::Warning: return "Warning";
		case eLandingPositionState::TooClose: return "TooClose";
		case eLandingPositionState::Confirmed: return "Confirmed";
		default: return "Unknown";
	}
}

std::string gameTypeText (eGameSettingsGameType t)
{
	switch (t)
	{
		case eGameSettingsGameType::Turns: return "Turn based";
		case eGameSettingsGameType::HotSeat: return "Hot Seat";
		default: return "Simultaneous";
	}
}

//------------------------------------------------------------------------------
// Клиент ИИИ: лобби → подготовка (клан, высадка) → партия → итоги
class cAiClient
{
public:
	cAiClient (sOptions options, std::string outDir, std::atomic<bool>& stop) :
		opt (std::move (options)),
		outDir (std::move (outDir)),
		stop (stop)
	{}

	~cAiClient();
	int run();

private:
	enum class eState { Connecting, Lobby, Preparation, Game, Done };
	enum class eProbe { Pending, Running, Done };

	void connectSignals();
	void updateIdentity();
	void setupLobby();
	void startPreparation();
	void chooseLanding (eLandingPositionState);
	void sendLanding (const cPosition&);
	void landingStep();
	void pumpMessages();
	bool forwardHeld();
	void checkSync();
	void startGameFromLobby();
	void startGame (std::shared_ptr<cClient>, bool newGame, bool tapped);
	void gameStep();
	void createEngine (bool passesSupported);
	bool myTurnReady() const;
	void probeStep();
	void finishGame (const std::string& reason);
	void chat (const std::string& text);

	sOptions opt;
	std::string outDir;
	std::atomic<bool>& stop;

	std::shared_ptr<cConnectionManager> connectionManager;
	std::unique_ptr<cLobbyClient> lobby;
	eState state = eState::Connecting;
	Clock::time_point stateSince = Clock::now();
	Clock::time_point nextConnect = Clock::now();
	Clock::time_point waitStart = Clock::now();
	int connectAttempts = 0;
	bool connectPending = false;

	// лобби
	std::vector<cPlayerBasicData> lobbyPlayers;
	std::shared_ptr<cGameSettings> lobbySettings;
	std::shared_ptr<cStaticMap> lobbyMap;
	bool savedGameSelected = false;
	std::string lastWarnedSettings;
	bool greeted = false;
	int setupStage = 0; // --setup: 0 — карта, 1 — настройки, 2 — ждать игроков, 3 — старт запрошен
	Clock::time_point setupAskedAt;

	// подготовка
	sInitPlayerData initPlayerData;
	ai_client::cLandingChooser landing;
	int landingTries = 0;
	cPosition landingPos;
	std::string landingState = "Unknown";
	bool landingOk = false;            // последнее известное состояние своей точки — Clear / Confirmed
	Clock::time_point landingSentAt;   // когда отправлена точка (состояние приходит, только если сменилось)

	// партия
	std::shared_ptr<cClient> client;
	ai_client::cActionTap tap;
	std::unique_ptr<ai_client::cObserver> observer;
	std::unique_ptr<ai::cAiEngine> engine;
	std::shared_ptr<ai::cDecisionJournal> llmJournal;
	std::shared_ptr<ai::cDecisionJournal> llmReplies;
	int playerId = -1;
	eProbe probe = eProbe::Done;
	unsigned int probeSerial = 0;
	Clock::time_point probeStart;
	int probeTries = 0;
	std::string passesInfo;
	int maxPasses = 1;
	bool gameOver = false;
	std::string endReason;
	int lastPrintedTurn = -1;
	int resyncs = 0;
	int freezes = 0;
	bool wasFrozen = false;
	bool frozenSaid = false;
	Clock::time_point frozenSince;
	bool saidHello = false;
	int winnerId = -1;
	std::string winnerName;
	int lastTurn = 0;
	Clock::time_point gameStart;
	bool tapped = false; // сообщения сервера идут через кран (иначе — переподключение: клиент лобби)
	std::unique_ptr<cNetMessage> heldMessage; // ждёт, пока ход его игрока начнётся в модели (forwardHeld)
	bool holding = false;
	Clock::time_point heldSince;
	int heldTimeouts = 0;
	// рассинхрон с сервером (checkSync): строки «OUT OF SYNC» в своём net.log
	std::uintmax_t netLogOffset = 0;
	Clock::time_point lastLogScan;
	int desyncs = 0;
	int resyncRequests = 0;
	Clock::time_point lastResyncRequest;

	// последними: отключаются первыми, пока сигналы лобби и модели ещё живы
	cSignalConnectionManager signals;
	cSignalConnectionManager gameSignals;
};

//------------------------------------------------------------------------------
cAiClient::~cAiClient()
{
	gameSignals.disconnectAll();
	signals.disconnectAll();
	// сеть не должна писать в разрушенный кран
	if (connectionManager) connectionManager->setLocalClient (nullptr, -1);
}

//------------------------------------------------------------------------------
void cAiClient::connectSignals()
{
	signals.connect (lobby->onLocalPlayerConnected, [this]() {
		connectPending = false;
		state = eState::Lobby;
		stateSince = Clock::now();
		greeted = false;
		setupStage = 0;
		say ("подключён к " + opt.host + ":" + std::to_string (opt.port) + " как игрок " + std::to_string (lobby->getLocalPlayer().getNr()));
	});
	signals.connect (lobby->onConnectionFailed, [this] (eDeclineConnectionReason reason) {
		connectPending = false;
		++connectAttempts;
		if (reason == eDeclineConnectionReason::NotPartOfTheGame)
			say ("хост: идёт партия, в которой меня нет — жду новую");
		else if (reason == eDeclineConnectionReason::AlreadyConnected)
			say ("хост: игрок с этим именем уже подключён");
		nextConnect = Clock::now() + std::chrono::seconds (2);
		if (connectAttempts == 1 || connectAttempts % 30 == 0)
			say ("хост " + opt.host + ":" + std::to_string (opt.port) + " не отвечает — жду, пока откроют игру (TCP/IP Host)");
	});
	signals.connect (lobby->onConnectionClosed, [this]() {
		connectPending = false;
		if (state == eState::Game || state == eState::Done) return; // в партии — через cClient::connectionToServerLost
		say ("хост закрыл лобби — жду новую игру");
		state = eState::Connecting;
		nextConnect = Clock::now() + std::chrono::seconds (2);
		waitStart = Clock::now();
	});
	signals.connect (lobby->onDifferentVersion, [] (std::string_view version, std::string_view revision) {
		say ("у хоста другая версия: " + std::string (version) + " " + std::string (revision) + " (своя " PACKAGE_VERSION " " PACKAGE_REV ")");
	});
	signals.connect (lobby->onPlayersList, [this] (const cPlayerBasicData&, const std::vector<cPlayerBasicData>& players) {
		lobbyPlayers = players;
		updateIdentity();
	});
	// сигналы зовутся изнутри changeLocalPlayerProperties — не звать updateIdentity рекурсивно:
	// исправит следующий список игроков от сервера
	signals.connect (lobby->onDuplicatedPlayerName, []() { Log.info ("ai_client: duplicated player name"); });
	signals.connect (lobby->onDuplicatedPlayerColor, []() { Log.info ("ai_client: duplicated player color"); });
	signals.connect (lobby->onOptionsChanged, [this] (std::shared_ptr<cGameSettings> settings, std::shared_ptr<cStaticMap> map, const cSaveGameInfo& save) {
		lobbySettings = settings;
		lobbyMap = map;
		savedGameSelected = save.number >= 0;
		updateIdentity();
	});
	signals.connect (lobby->onNoMapNoReady, [this] (const std::filesystem::path& map) {
		say ("нет карты " + utf8::to_string (map) + " — не готов");
	});
	signals.connect (lobby->onMapDownloadRequest, [this] (const std::filesystem::path& map) {
		say ("карты " + utf8::to_string (map) + " нет — качаю у хоста");
		chat (opt.name + ": no map " + utf8::to_string (map) + ", downloading. Select the map again when done.");
	});
	signals.connect (lobby->onDownloadMapFinished, [this] (std::shared_ptr<cStaticMap>) {
		say ("карта скачана — попросите хоста выбрать её ещё раз");
	});
	signals.connect (lobby->onChatMessage, [] (const std::string& player, const std::string& message) {
		say ("чат: " + player + ": " + message);
	});
	signals.connect (lobby->onCannotEndLobby, [this] (bool missingSettings, const std::vector<cPlayerBasicData>& notReady, bool, const std::vector<cPlayerBasicData>&) {
		std::string who;
		for (const auto& p : notReady) who += (who.empty() ? "" : ", ") + p.getName();
		say (std::string ("старт не принят: ") + (missingSettings ? "нет настроек или карты; " : "") + (who.empty() ? "" : "не готовы: " + who));
		if (setupStage == 3) setupStage = 2;
	});
	signals.connect (lobby->onStartGamePreparation, [this]() { startPreparation(); });
	signals.connect (lobby->onPlayerAbortGamePreparation, [this] (const std::string& player) {
		say (player + " вышел из подготовки — назад в лобби");
		state = eState::Lobby;
		stateSince = Clock::now();
		if (setupStage == 3) setupStage = 2;
	});
	signals.connect (lobby->onLandingDone, [this] (eLandingPositionState s) { chooseLanding (s); });
	// START_GAME клиент разбирает сам (startGameFromLobby): лобби не получает его и своего cClient не создаёт.
	// Переподключение к идущей партии (GAME_ALREADY_RUNNING) делает лобби — его cClient, без крана.
	signals.connect (lobby->onReconnectGame, [this] (std::shared_ptr<cClient> c) { startGame (c, false, false); });
	signals.connect (lobby->onDisconnectNotInSavedGame, []() { say ("хост загрузил сохранение без меня — отключён"); });
}

//------------------------------------------------------------------------------
void cAiClient::chat (const std::string& text)
{
	if (state == eState::Game && client)
	{
		if (const cPlayer* me = client->getModel().getPlayer (playerId))
			client->report (std::make_unique<cSavedReportChat> (*me, std::string (text)));
	}
	else if (lobby && lobby->isConnectedToServer() && lobby->getLocalPlayer().getNr() >= 0)
		lobby->sendChatMessage (std::string (text));
}

//------------------------------------------------------------------------------
// имя и цвет — не занятые другими; готов, когда есть карта и настройки, режим «Turn based»
void cAiClient::updateIdentity()
{
	if (state != eState::Lobby) return;
	const int me = lobby->getLocalPlayer().getNr();
	const auto others = [&] (const auto& pred) {
		return std::any_of (lobbyPlayers.begin(), lobbyPlayers.end(), [&] (const cPlayerBasicData& p) { return p.getNr() != me && pred (p); });
	};

	std::string name = opt.name;
	for (int k = 2; others ([&] (const cPlayerBasicData& p) { return p.getName() == name; }) && k < 20; ++k)
		name = opt.name + " " + std::to_string (k);

	cRgbColor color = lobby->getLocalPlayer().getColor();
	if (lobby->getLocalPlayer().getName() != name || others ([&] (const cPlayerBasicData& p) { return sameColor (p.getColor(), color); }))
	{
		// свободный цвет из стандартных: с конца списка (хост обычно — красный, первый), а свой
		// начальный сдвиг по номеру игрока — чтобы два клиента ИИИ не выбрали один и тот же
		const int n = static_cast<int> (cPlayerColor::predefinedColorsCount);
		for (int k = 0; k < n; ++k)
		{
			const int i = ((n - 1 - std::max (0, me) - k) % n + n) % n;
			const cRgbColor c = cPlayerColor::predefinedColors[i];
			if (!others ([&] (const cPlayerBasicData& p) { return sameColor (p.getColor(), c); }))
			{
				color = c;
				break;
			}
		}
	}

	const bool haveMap = lobbyMap && lobbyMap->isValid() && lobbyMap->getSize().x() > 0;
	const bool turnBased = lobbySettings && lobbySettings->gameType == eGameSettingsGameType::Turns;
	const bool ready = (haveMap && turnBased) || savedGameSelected;

	if (lobbySettings && !turnBased && !savedGameSelected)
	{
		const std::string key = gameTypeText (lobbySettings->gameType);
		if (key != lastWarnedSettings)
		{
			lastWarnedSettings = key;
			say ("тип игры «" + key + "» — ИИИ играет только в «Turn based»: не готов");
			chat (name + ": I play only Game Type 'Turn based' (now: " + key + "). Options -> Game Type -> Turn based.");
		}
	}
	else
		lastWarnedSettings.clear();

	const auto& local = lobby->getLocalPlayer();
	if (local.getName() != name || !sameColor (local.getColor(), color) || local.isReady() != ready)
	{
		const bool wasReady = local.isReady();
		lobby->changeLocalPlayerProperties (std::string (name), color, ready);
		if (ready && !wasReady && lobbyMap) say ("готов: карта " + utf8::to_string (lobbyMap->getFilename()) + ", Turn based");
	}
	if (!greeted && me >= 0)
	{
		greeted = true;
		chat (name + " (brain " + opt.brain + ") joined. I play Game Type 'Turn based' only.");
	}
}

//------------------------------------------------------------------------------
// --setup: выделенный сервер — клиент сам выбирает карту и настройки и стартует
void cAiClient::setupLobby()
{
	if (!opt.setup || state != eState::Lobby) return;
	if (setupStage == 0)
	{
		lobby->selectMapFilename (opt.map);
		setupStage = 1;
		say ("настройка: карта " + opt.map);
		return;
	}
	if (setupStage == 1)
	{
		if (!lobbyMap || utf8::to_string (lobbyMap->getFilename()) != opt.map) return; // ждём, пока карта дойдёт
		cGameSettings s;
		s.gameType = eGameSettingsGameType::Turns;
		s.turnLimitActive = false;
		s.turnEndDeadlineActive = false;
		if (opt.victoryTurns > 0)
		{
			s.victoryConditionType = eGameSettingsVictoryCondition::Turns;
			s.victoryTurns = static_cast<unsigned int> (opt.victoryTurns);
		}
		lobby->selectGameSettings (s);
		setupStage = 2;
		say ("настройка: Turn based, без таймеров, победа " + (opt.victoryTurns > 0 ? "по " + std::to_string (opt.victoryTurns) + " ходам" : std::string ("до уничтожения")));
		return;
	}
	if (setupStage == 2)
	{
		const bool allReady = static_cast<int> (lobbyPlayers.size()) >= opt.players
			&& std::all_of (lobbyPlayers.begin(), lobbyPlayers.end(), [] (const cPlayerBasicData& p) { return p.isReady(); });
		if (!allReady || !lobbySettings || lobbySettings->gameType != eGameSettingsGameType::Turns) return;
		if (Clock::now() - setupAskedAt < std::chrono::seconds (3)) return;
		setupAskedAt = Clock::now();
		lobby->askToFinishLobby();
		setupStage = 3;
		say ("настройка: все готовы (" + std::to_string (lobbyPlayers.size()) + ") — старт");
	}
}

//------------------------------------------------------------------------------
void cAiClient::startPreparation()
{
	const auto& prep = lobby->getLobbyPreparationData();
	if (!prep.gameSettings || !prep.staticMap || !prep.unitsData)
	{
		say ("подготовка: нет настроек, карты или данных юнитов — жду");
		return;
	}
	state = eState::Preparation;
	stateSince = Clock::now();
	const auto& settings = *prep.gameSettings;

	std::uint32_t seed = opt.seed != 0 ? opt.seed : static_cast<std::uint32_t> (std::time (nullptr)) ^ static_cast<std::uint32_t> (lobby->getLocalPlayer().getNr() * 7919);
	initPlayerData = sInitPlayerData();
	if (settings.clansEnabled)
		initPlayerData.clan = opt.clan >= 0 ? opt.clan : static_cast<int> (seed % 8);
	else
		initPlayerData.clan = -1;
	// бесплатный стартовый набор (как ai_headless); кредиты остаются на улучшения за золото
	for (const auto& [unitId, cargo] : computeInitialLandingUnits (initPlayerData.clan, settings, *prep.unitsData))
		initPlayerData.landingUnits.push_back ({unitId, static_cast<unsigned int> (cargo)});

	const bool fixed = settings.bridgeheadType == eGameSettingsBridgeheadType::Definite;
	landing.init (*prep.staticMap, *prep.unitsData, fixed, initPlayerData.landingUnits, seed);
	landingTries = 0;
	say ("подготовка: клан " + std::to_string (initPlayerData.clan) + ", юнитов высадки " + std::to_string (initPlayerData.landingUnits.size())
		+ "; высадка: " + landing.describe());

	lobby->enterLandingSelection();
	const auto p = landing.first();
	if (!p)
	{
		say ("ОШИБКА: на карте нет годной точки высадки");
		chat (opt.name + ": no valid landing place on this map.");
		return;
	}
	say ("высадка: (" + std::to_string (p->x()) + "," + std::to_string (p->y()) + ")");
	sendLanding (*p);
}

//------------------------------------------------------------------------------
void cAiClient::sendLanding (const cPosition& p)
{
	landingPos = p;
	initPlayerData.landingPosition = p;
	landingOk = false;
	landingSentAt = Clock::now();
	lobby->selectLandingPosition (p);
}

//------------------------------------------------------------------------------
// Состояние своей точки сервер присылает, только когда оно сменилось: если ответа нет (точка осталась
// TooClose или Warning, хотя сосед уже ушёл, — «лучшее» состояние другому игроку сервер не ставит),
// через 2 с выбрать новую точку.
void cAiClient::landingStep()
{
	if (state != eState::Preparation || landingOk || landingTries > 40) return;
	if (Clock::now() - landingSentAt < std::chrono::seconds (2)) return;
	chooseLanding (eLandingPositionState::TooClose);
}

//------------------------------------------------------------------------------
void cAiClient::chooseLanding (eLandingPositionState s)
{
	landingState = landingStateText (s);
	if (s != eLandingPositionState::Warning && s != eLandingPositionState::TooClose)
	{
		landingOk = true;
		say (std::string ("высадка: ") + landingState);
		return;
	}
	++landingTries;
	if (landingTries > 40)
	{
		say ("высадка: не нашёл места вдали от других за 40 попыток — жду");
		return;
	}
	if (landingTries > 12 && s == eLandingPositionState::Warning)
	{
		// не нашлось места вдали от всех: Warning — подтвердить ту же точку (движок разрешает), TooClose — дальше искать
		say (std::string ("высадка: ") + landingState + " и после 12 попыток — подтверждаю (" + std::to_string (landingPos.x()) + "," + std::to_string (landingPos.y()) + ")");
		sendLanding (landingPos);
		return;
	}
	const auto p = landing.next();
	if (!p) return;
	say (std::string ("высадка: ") + landingState + " — рядом другой игрок, новая точка (" + std::to_string (p->x()) + "," + std::to_string (p->y()) + ")");
	sendLanding (*p);
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Сообщения сервера — через кран (он — приёмник connectionManager с самого начала: cClient не умеет
// popMessage, и подменить приёмник после старта партии нельзя). В лобби — лобби-клиенту, в партии —
// наблюдателю (действия других игроков в журнал, модель ещё до них) и затем cClient.
void cAiClient::pumpMessages()
{
	if (heldMessage && !forwardHeld()) return;
	while (auto message = tap.popMessage())
	{
		if (state == eState::Game && client && tapped)
		{
			heldMessage = std::move (message);
			if (!forwardHeld()) return; // ход чужого игрока ещё не начался в модели — ждать
			continue;
		}
		if (message->getType() == eNetMessageType::MULTIPLAYER_LOBBY
			&& static_cast<const cMultiplayerLobbyMessage&> (*message).getType() == cMultiplayerLobbyMessage::eMessageType::MU_MSG_START_GAME)
		{
			lobby->run(); // сначала — всё, что пришло до старта (список игроков, номера)
			startGameFromLobby();
			continue;
		}
		lobby->pushMessage (std::move (message));
	}
}

//------------------------------------------------------------------------------
// Сообщение сервера — наблюдателю и клиенту. Действие чужого игрока в «Turn based» сервер исполнил в его
// ход; пока модель клиента туда не дошла (смена хода и действие пришли одной пачкой), сообщение ждёт:
// иначе в журнал ушёл бы прежний номер хода, а снимок начала его хода — после его действий.
// Страховка — 3 с: дальше отдать как есть.
bool cAiClient::forwardHeld()
{
	const cModel& model = client->getModel();
	if (observer && !gameOver && observer->mustWait (*heldMessage, model))
	{
		if (!holding)
		{
			holding = true;
			heldSince = Clock::now();
		}
		if (Clock::now() - heldSince < std::chrono::seconds (3)) return false;
		Log.warn ("ai_client: action of player " + std::to_string (heldMessage->playerNr) + " held 3 s, the turn did not start in the model — forwarded");
		++heldTimeouts;
	}
	holding = false;
	if (observer && !gameOver)
	{
		if (heldMessage->getType() == eNetMessageType::ACTION && heldMessage->playerNr != playerId)
			observer->tick (model, heldMessage->playerNr); // снимок начала его хода — до его первого действия
		observer->onMessage (*heldMessage, model);
	}
	client->pushMessage (std::move (heldMessage));
	return true;
}

//------------------------------------------------------------------------------
// Рассинхрон: модель клиента разошлась с моделью сервера (хост другой сборки — у нашей движок с правками,
// у авторов без них). Клиент движка сверяет контрольную сумму каждый такт, но только пишет в net.log
// «OUT OF SYNC» (сумма — внутри cGameTimerClient), а сервер авторов сам модель не присылает — партия
// расходится и встаёт (EndTurn отбрасывается: «another players turn»). Клиент ИИИ раз в секунду читает
// новые строки своего net.log и при «OUT OF SYNC» просит сервер прислать модель (REQUEST_RESYNC_MODEL,
// как чат-команда «/resync» окна игры) — не чаще раза в 30 с: у хоста авторов после пересинхронизации
// модели снова расходятся (движок форка играет иначе), просьба держит в согласии ход и очередь.
void cAiClient::checkSync()
{
	if (gNetLogPath.empty() || Clock::now() - lastLogScan < std::chrono::seconds (1)) return;
	lastLogScan = Clock::now();
	std::error_code ec;
	const auto size = std::filesystem::file_size (gNetLogPath, ec);
	if (ec) return;
	if (size < netLogOffset) netLogOffset = 0;
	if (size == netLogOffset) return;
	std::uintmax_t from = netLogOffset;
	if (size - from > (64u << 20)) from = size - (1u << 20); // отладочный лог вырос скачком — хватит хвоста
	std::ifstream f (gNetLogPath, std::ios::binary);
	f.seekg (static_cast<std::streamoff> (from));
	std::string chunk (static_cast<std::size_t> (size - from), '\0');
	f.read (chunk.data(), static_cast<std::streamsize> (chunk.size()));
	chunk.resize (static_cast<std::size_t> (f.gcount()));
	netLogOffset = from + chunk.size();
	int found = 0;
	for (std::size_t pos = chunk.find ("OUT OF SYNC"); pos != std::string::npos; pos = chunk.find ("OUT OF SYNC", pos + 1))
		++found;
	if (found == 0) return;
	desyncs += found;
	if (resyncRequests > 0 && Clock::now() - lastResyncRequest < std::chrono::seconds (30)) return;
	lastResyncRequest = Clock::now();
	++resyncRequests;
	say ("модель разошлась с сервером (" + std::to_string (desyncs) + " тактов «OUT OF SYNC») — прошу пересинхронизацию");
	cNetMessageRequestResync request (playerId);
	request.From (playerId);
	connectionManager->sendToServer (request);
}

//------------------------------------------------------------------------------
// копия cLobbyClient::handleNetMessage_MU_MSG_START_GAME, но приёмник сообщений остаётся краном
void cAiClient::startGameFromLobby()
{
	const auto t0 = Clock::now();
	const auto ms = [&t0]() { return std::to_string (std::chrono::duration_cast<std::chrono::milliseconds> (Clock::now() - t0).count()); };
	Log.info ("ai_client: START_GAME received");
	auto c = std::make_shared<cClient> (connectionManager);
	c->setPlayers (lobbyPlayers, lobby->getLocalPlayer().getNr());
	if (savedGameSelected)
	{
		c->setMap (lobby->getLobbyPreparationData().staticMap);
		startGame (c, false, true);
	}
	else
	{
		c->setPreparationData (lobby->getLobbyPreparationData());
		Log.info ("ai_client: client created in " + ms() + " ms");
		startGame (c, true, true);
	}
	Log.info ("ai_client: game set up in " + ms() + " ms");
}

//------------------------------------------------------------------------------
void cAiClient::startGame (std::shared_ptr<cClient> c, bool newGame, bool tapped_)
{
	client = c;
	tapped = tapped_;
	playerId = client->getActivePlayer().getId();
	state = eState::Game;
	stateSince = Clock::now();
	gameStart = Clock::now();
	gameOver = false;

	{
		std::error_code ec;
		const auto size = gNetLogPath.empty() ? 0 : std::filesystem::file_size (gNetLogPath, ec);
		netLogOffset = ec ? 0 : size;
	}
	// действия других игроков — в журнал (только через кран)
	if (tapped) observer = std::make_unique<ai_client::cObserver> (outDir, playerId);

	gameSignals.connect (client->connectionToServerLost, [this]() {
		if (!gameOver) finishGame ("connectionLost");
	});
	gameSignals.connect (client->resynced, [this]() {
		++resyncs;
		say ("модель пересинхронизирована сервером (" + std::to_string (resyncs) + ")");
	});
	const cModel& model = client->getModel();
	gameSignals.connect (model.playerHasWon, [this] (const cPlayer& winner) {
		winnerId = winner.getId();
		winnerName = winner.getName();
		say ("победил " + winnerName);
		if (!gameOver) finishGame (winner.getId() == playerId ? "won" : "lost");
	});
	gameSignals.connect (model.suddenDeathMode, []() { say ("ничья по очкам — «внезапная смерть»: игра идёт до перевеса"); });
	gameSignals.connect (model.playerHasLost, [this] (const cPlayer& loser) {
		say ("проиграл " + loser.getName());
		if (loser.getId() == playerId && !gameOver) finishGame ("defeated");
	});

	if (newGame) client->initNewGame (initPlayerData);

	// заходы: числом — сразу; auto — проба в первом своём ходу (хост авторов AiPass не понимает)
	if (opt.passes == "auto")
	{
		probe = eProbe::Pending;
		createEngine (true);
	}
	else
	{
		probe = eProbe::Done;
		ai_client::setEnv ("MAXR_PASSES", opt.passes.c_str());
		createEngine (true);
		passesInfo = "set " + opt.passes;
	}
	say (std::string (newGame ? "партия началась" : "партия продолжена (сохранение / переподключение)") + ": игрок " + std::to_string (playerId)
		+ ", мозг " + opt.brain + ", журналы — " + outDir);
}

//------------------------------------------------------------------------------
void cAiClient::createEngine (bool passesSupported)
{
	if (!passesSupported) ai_client::setEnv ("MAXR_PASSES", "1");
	engine.reset(); // журнал прежнего движка закрыт до открытия нового (дописывается)
	engine = std::make_unique<ai::cAiEngine> (outDir);
	maxPasses = engine->getMaxPasses();

	std::shared_ptr<ai::cAiBrain> brain;
	if (opt.brain == "base2")
		brain = std::make_shared<ai::base2::cRuleBrain>();
	else if (opt.brain == "base")
		brain = std::make_shared<ai::cBaseRuleBrain>();
	else if (opt.brain == "llm")
	{
		const ai::sLlmConfig config = ai::sLlmConfig::fromEnv();
		if (!llmJournal)
		{
			llmJournal = std::make_shared<ai::cDecisionJournal> (outDir + "/llm.jsonl");
			llmJournal->open();
			llmReplies = std::make_shared<ai::cDecisionJournal> (outDir + "/llm_replies.jsonl");
			llmReplies->open();
			say ("советник: " + config.url + ", модель " + (config.model.empty() ? std::string ("(не задана)") : config.model));
		}
		brain = std::make_shared<ai::cLlmAdvisorBrain> (llmJournal, config, llmReplies);
	}
	else
		brain = std::make_shared<ai::cRuleBrain>();
	engine->addPlayer (playerId, client.get(), brain);
}

//------------------------------------------------------------------------------
bool cAiClient::myTurnReady() const
{
	const cModel& model = client->getModel();
	const cPlayer* active = model.getActiveTurnPlayer();
	return active != nullptr && active->getId() == playerId && active->getLandingPos() != cPosition (-1, -1)
		&& !active->getHasFinishedTurn() && model.isTurnStartPlayedOut();
}

//------------------------------------------------------------------------------
// Проба заходов: в первом своём ходу — пустой AiPass. Хост нашей сборки его исполняет и рассылает
// (номер захода в модели растёт), хост авторов не может разобрать новое действие и молча бросает.
void cAiClient::probeStep()
{
	const cModel& model = client->getModel();
	const bool frozen = client->getFreezeModes().isFrozen();
	if (probe == eProbe::Pending)
	{
		if (frozen) return;
		probeSerial = model.getAiPassSerial();
		probeStart = Clock::now();
		++probeTries;
		client->aiPass();
		probe = eProbe::Running;
		return;
	}
	// Running
	if (frozen)
	{
		// в заморозке сервер бросает действия — проба не годится, повторить
		probe = eProbe::Pending;
		return;
	}
	if (model.getAiPassSerial() != probeSerial && !model.isAiPassActive())
	{
		probe = eProbe::Done;
		passesInfo = "auto: host understands AiPass, up to " + std::to_string (maxPasses) + " passes";
		say ("хост понимает заходы хода — до " + std::to_string (maxPasses) + " за ход");
		return;
	}
	if (Clock::now() - probeStart > std::chrono::seconds (6))
	{
		probe = eProbe::Done;
		createEngine (false);
		passesInfo = "auto: host ignored AiPass (authors' build?) — 1 pass";
		say ("хост не понимает заходы хода (сборка авторов?) — один заход за ход (MAXR_PASSES=1)");
	}
}

//------------------------------------------------------------------------------
void cAiClient::gameStep()
{
	client->run(); // такт модели, синхронизация с сервером
	if (gameOver) return;
	checkSync();

	const cModel& model = client->getModel();
	// заморозка на смене хода — обычное дело (доли секунды); говорить — о долгой
	const bool frozen = client->getFreezeModes().isFrozen();
	if (frozen != wasFrozen)
	{
		wasFrozen = frozen;
		frozenSince = Clock::now();
		if (!frozen && frozenSaid)
		{
			frozenSaid = false;
			say ("игра снова идёт");
		}
	}
	if (frozen && !frozenSaid && Clock::now() - frozenSince > std::chrono::seconds (5))
	{
		frozenSaid = true;
		++freezes;
		say ("игра заморожена дольше 5 с (ждём игрока или сервер)");
	}

	if (observer) observer->tick (model);

	const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	lastTurn = turn;
	if (opt.quitAtTurn > 0 && turn >= opt.quitAtTurn && model.isTurnStartPlayedOut())
	{
		finishGame ("turnLimit");
		return;
	}
	if (const cPlayer* active = model.getActiveTurnPlayer(); active != nullptr && turn != lastPrintedTurn && model.isTurnStartPlayedOut())
	{
		if (active->getId() == playerId)
		{
			lastPrintedTurn = turn;
			say ("ход " + std::to_string (turn) + ": думаю");
		}
	}

	// движок ИИИ — когда высадка всех игроков дошла до модели (как ai_headless: снимок метрик «до первого хода»)
	const auto& list = model.getPlayerList();
	if (!std::all_of (list.begin(), list.end(), [] (const auto& p) { return p->getLandingPos() != cPosition (-1, -1); })) return;

	if (probe != eProbe::Done && myTurnReady())
	{
		probeStep();
		return;
	}
	if (myTurnReady() && !saidHello)
	{
		saidHello = true;
		chat (lobby->getLocalPlayer().getName() + ": good luck! (brain " + opt.brain + ")");
	}
	engine->run();
}

//------------------------------------------------------------------------------
void cAiClient::finishGame (const std::string& reason)
{
	if (gameOver) return;
	gameOver = true;
	endReason = reason;
	const cModel& model = client->getModel();
	const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : lastTurn;
	const bool anyDefeated = std::any_of (model.getPlayerList().begin(), model.getPlayerList().end(), [] (const auto& p) { return p->isDefeated; });
	ai::sGameResult result;
	if (engine) result = engine->finalize (anyDefeated ? ai::eGameEnd::Elimination : ai::eGameEnd::TurnLimit, turn);
	if (observer) observer->close();

	nlohmann::json j;
	j["event"] = "client";
	j["name"] = lobby->getLocalPlayer().getName();
	j["player"] = playerId;
	j["brain"] = opt.brain;
	j["host"] = opt.host + ":" + std::to_string (opt.port);
	j["version"] = std::string (PACKAGE_VERSION " " PACKAGE_REV);
	j["map"] = model.getMap() ? utf8::to_string (model.getMap()->getFilename()) : std::string();
	j["clan"] = initPlayerData.clan;
	j["landing"] = {{"x", landingPos.x()}, {"y", landingPos.y()}, {"state", landingState}, {"tries", landingTries}};
	j["passes"] = passesInfo;
	j["maxPasses"] = maxPasses;
	j["end"] = reason;
	j["turn"] = turn;
	// свой проигрыш приходит раньше победы соперника (тот же тик) — победитель тогда из итога движка
	if (winnerId < 0 && result.winner >= 0)
	{
		winnerId = result.winner;
		if (const cPlayer* w = model.getPlayer (winnerId)) winnerName = w->getName();
	}
	j["winner"] = winnerId;
	j["winnerName"] = winnerName;
	j["engineResult"] = {{"winner", result.winner}, {"winReason", result.winReason}, {"endReason", result.endReason}};
	j["resyncs"] = resyncs;
	j["heldTimeouts"] = heldTimeouts;
	j["outOfSyncTicks"] = desyncs;
	j["resyncRequests"] = resyncRequests;
	j["freezes"] = freezes;
	j["seconds"] = std::chrono::duration_cast<std::chrono::seconds> (Clock::now() - gameStart).count();
	if (const auto settings = model.getGameSettings())
		j["gameType"] = gameTypeText (settings->gameType);
	nlohmann::json ps = nlohmann::json::array();
	for (const auto& p : model.getPlayerList())
		ps.push_back ({{"id", p->getId()}, {"name", p->getName()}, {"defeated", p->isDefeated}, {"score", p->getScore()},
			{"vehicles", p->getVehicles().size()}, {"buildings", p->getBuildings().size()}});
	j["players"] = std::move (ps);
	if (observer) j["observed"] = observer->summary();
	std::ofstream f (outDir + "/client.json");
	f << j.dump (2) << "\n";

	say ("партия окончена (" + reason + ") на ходу " + std::to_string (turn) + "; итоги — " + outDir);
	if (reason == "won" || reason == "lost" || reason == "defeated")
		chat (lobby->getLocalPlayer().getName() + ": gg");
}

//------------------------------------------------------------------------------
int cAiClient::run()
{
	connectionManager = std::make_shared<cConnectionManager>();
	auto player = cPlayerBasicData::fromSettings();
	player.setName (std::string (opt.name));
	player.setColor (cPlayerColor::predefinedColors[cPlayerColor::predefinedColorsCount - 1]);
	player.setReady (false);
	lobby = std::make_unique<cLobbyClient> (connectionManager, player);
	connectionManager->setLocalClient (&tap, -1); // кран — вместо лобби-клиента (сообщения раздаёт pumpMessages)
	connectSignals();
	waitStart = Clock::now();

	say ("клиент ИИИ: мозг " + opt.brain + ", ищу хост " + opt.host + ":" + std::to_string (opt.port) + " (" PACKAGE_VERSION " " PACKAGE_REV ")");
	while (true)
	{
		if (stop.load())
		{
			say ("остановлен");
			if (state == eState::Game && !gameOver) finishGame ("stopped");
			break;
		}
		pumpMessages();
		switch (state)
		{
			case eState::Connecting:
				if (!connectPending && !lobby->isConnectedToServer() && Clock::now() >= nextConnect)
				{
					if (opt.waitSec > 0 && Clock::now() - waitStart > std::chrono::seconds (opt.waitSec))
					{
						say ("хост так и не открыл игру за " + std::to_string (opt.waitSec) + " с — выход");
						return 2;
					}
					connectPending = true;
					lobby->connectToServer ({opt.host, static_cast<std::uint16_t> (opt.port)});
				}
				lobby->run();
				break;
			case eState::Lobby:
			case eState::Preparation:
				lobby->run();
				setupLobby();
				landingStep();
				break;
			case eState::Game:
				gameStep();
				break;
			case eState::Done:
				break;
		}
		if (state == eState::Game && gameOver)
		{
			// партия окончена: отключиться, таймер клиента — остановить до разрушения (гонка SDL, шаг 11 §9 п. 4)
			connectionManager->disconnectAll();
			client->getGameTimer()->stop();
			SDL_Delay (3 * GAME_TICK_TIME);
			if (!opt.keepRunning || endReason == "stopped") break;
			return 3; // --loop: начать заново — делает main (новый клиент и каталог)
		}
		SDL_Delay (state == eState::Game ? 2 : 10);
	}
	if (state == eState::Game && client)
	{
		connectionManager->disconnectAll();
		client->getGameTimer()->stop();
		SDL_Delay (3 * GAME_TICK_TIME);
	}
	return 0;
}

} // namespace

//------------------------------------------------------------------------------
int main (int argc, char** argv)
try
{
	setlocale (LC_ALL, "");
	try
	{
		std::locale::global (std::locale (""));
	}
	catch (const std::exception&)
	{
		// LANG из git-bash ломает std::locale("") — остаёмся на «C» (пути — ASCII)
	}
	static std::atomic<bool> stop{false};
	static std::atomic<bool> done{false};
	ai_client::installConsole (&stop, &done);

	sOptions opt;
	std::string err;
	if (!parseArgs (argc, argv, opt, err))
	{
		std::cout << "ОШИБКА: " << err << "\n";
		printUsage();
		return 1;
	}

	const std::filesystem::path exeDir = os::getCurrentExeDir();
	bool firstGame = true;
	int code = 0;
	do
	{
		std::string outDir = opt.outDir;
		if (outDir.empty() || !firstGame)
			outDir = utf8::to_string (exeDir / "ai_games" / (timeStamp ("%Y-%m-%d_%H-%M-%S") + "_" + safeName (opt.name)));
		std::error_code ec;
		std::filesystem::create_directories (outDir, ec);
		if (firstGame)
		{
			// логи движка — в каталог партии, до cSettings (иначе — portable\maxr.log, общий с окном игры)
			Log.setLogPath (outDir + "/maxr.log");
			NetLog.setLogPath (outDir + "/net.log");
			gNetLogPath = outDir + "/net.log";
			if (!cSettings::getInstance().isInitialized())
			{
				std::cout << "ОШИБКА: настройки игры не прочитаны (maxr.json)\n";
				return 1;
			}
			logMAXRVersion();
			CR_INIT_CRASHREPORTING();
		}
		static std::shared_ptr<SDLComponent> sdl;
		static std::shared_ptr<SDLNetComponent> sdlNet;
		if (firstGame)
		{
			sdl = std::make_shared<SDLComponent> (false);
			sdlNet = std::make_shared<SDLNetComponent> (sdl);
			if (LoadData (false) == eLoadingState::Error)
			{
				std::cout << "ОШИБКА: данные игры не загружены (запускать из папки игры)\n";
				return 1;
			}
		}
		cAiClient aiClient (opt, outDir, stop);
		code = aiClient.run();
		firstGame = false;
	} while (code == 3 && !stop.load());
	done.store (true);
	return code == 3 ? 0 : code;
}
catch (const std::exception& ex)
{
	Log.error (std::string ("ai_client: ") + ex.what());
	std::cout << "ОШИБКА: " << ex.what() << std::endl;
	return 1;
}
