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

#ifndef game_logic_gametimerH
#define game_logic_gametimerH

#include <SDL.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

class cClient;
class cServer;
class cNetMessageSyncServer;
class cNetMessageSyncClient;
class cModel;
class cPlayer;

#define GAME_TICK_TIME 10 /** Number of milliseconds of one game time tick */
#define MAX_CLIENT_LAG 15 /** Maximum ticks, the clients time is allowed to be behind
										    the server time */
#define PAUSE_GAME_TIMEOUT 200 /** Number of ticks, after which the server pauses the game,
										    when no sync message from a client is received */
#define MAX_SERVER_EVENT_COUNTER 15 /** When the server is under heavy load, ticks are discarded,
											when there are more than MAX_SERVER_EVENT_COUNTER timer events pending */
#define MAX_WAITING_FOR_SERVER 50 /** The client will display the "Waiting for Server message" when no sync messages are
											received for MAX_WAITING_FOR_SERVER ticks */
#define FAST_TICK_WINDOW 100 /** Headless fast tick: maximum ticks, the server may run ahead of the slowest client.
										Must stay below PAUSE_GAME_TIMEOUT, otherwise the clients are marked as not responding */

struct sGameTimerClientDebugData
{
	bool crcOK;
	float timeBuffer;
	float ticksPerFrame;
	float queueSize;
	float eventCounter;
	float ping;
};

class cGameTimer
{
	friend class cDebugOutputWidget;

protected:
	static Uint32 gameTimerCallback (Uint32 interval, void* arg);

	cGameTimer();

	/** SDL_Timer that controls the game time */
	SDL_TimerID timerID;
	std::mutex mutex;
	unsigned int eventCounter;

	void timerCallback();

	void pushEvent();
	bool popEvent();

public:
	~cGameTimer();

	unsigned int maxEventQueueSize;

	void start();
	void stop();
};

class cGameTimerServer : public cGameTimer
{
	friend class cDebugOutputWidget;

private:
	void checkPlayersResponding (const std::vector<std::shared_ptr<cPlayer>>& playerList, cServer&);
	void advanceGameTime (cModel&, cServer&);
	void runFast (cModel&, cServer&);
	/** headless deterministic tick: the model stands still, while this is true */
	static bool isPlayerDeciding (const cModel&);
	std::map<int, sGameTimerClientDebugData> clientDebugData;
	std::map<int, unsigned int> receivedTime; // time that the client has reported to server in last sync message
	unsigned int sentGameTime = 0; // time that has server has reported to clients in last sync message

public:
	cGameTimerServer() = default;
	void run (cModel&, cServer& server);
	void handleSyncMessage (const cNetMessageSyncClient&, unsigned int gameTime);
	void setPlayerNumbers (const std::vector<std::shared_ptr<cPlayer>>& playerList);

	/**
	* Headless-only deterministic tick: when true, the server does NOT advance
	* the model (no unit movement/attacks) while a player's turn is active and
	* the player has not finished. Once the player finishes, the turn is played
	* out deterministically. During a pass of the turn (cActionAiPass, see
	* cModel::startAiPass) the model runs, until the orders of the pass are played out;
	* at the start of a turn it runs, until the reaction fire triggered by the turn start
	* has hit (cModel::isTurnStartPlayedOut). Set via cServer::setDeterministicTick.
	*/
	bool deterministicTick = false;

	/**
	* Headless-only fast tick: when true, the server ignores the wall-clock timer
	* events and advances the model as fast as the clients acknowledge the game
	* time (at most FAST_TICK_WINDOW ticks ahead of the slowest client). Pausing
	* (stopped timer) is still respected. Meant to be used together with
	* deterministicTick. Set via cServer::setFastTick.
	*/
	bool fastTick = false;
};

class cGameTimerClient : public cGameTimer
{
	friend class cDebugOutputWidget;

private:
	unsigned int receivedTime = 0; // gametime of the latest sync message in the netmessage queue
	unsigned int remoteChecksum = 0; // received checksum from server. After running the jobs for the next gametime, the clientmodel should have the same checksum!
	unsigned int timeSinceLastSyncMessage = 0; // when no sync message is received for a certain time, user gets message "waiting for server"

	bool syncMessageReceived = false; // The gametime can only be increased, after the sync message for the next gametime from the server has been received.
		// After the sync message has been received, all following netmessages belong to the next gametime step. So handling of
		// messages is stopped, until client reached the next gametime.

	unsigned int localChecksum = 0; // saved local checksum for debug view
	unsigned int debugRemoteChecksum = 0; // saved data for debug view only
	unsigned int ping = 0; // saved data for debug view only

	void checkServerResponding (cClient&);
	void runFast (cClient&, cModel&);

public:
	cGameTimerClient() = default;

	void setReceivedTime (unsigned int time);
	unsigned int getReceivedTime();

	void run (cClient&, cModel&);

	void sendSyncMessage (const cClient&, unsigned int gameTime, unsigned int tickPerFrame, unsigned int timeBuffer);

	void handleSyncMessage (const cNetMessageSyncServer&, unsigned int gameTime);

	/**
	* Headless-only deterministic tick: when true, the client never enters the
	* WaitForServer freeze mode (the server freezes game time during a player's
	* turn, so sync messages are legitimately absent while the AI decides).
	* Otherwise cClient::endTurn would be blocked by the freeze. Set via
	* cClient::setDeterministicTick.
	*/
	bool deterministicTick = false;

	/**
	* Headless-only fast tick: when true, the client does not wait for its
	* wall-clock timer events, but processes every sync message received from
	* the server so far (the server paces the game, see
	* cGameTimerServer::fastTick). Set via cClient::setFastTick.
	*/
	bool fastTick = false;
};

#endif // game_logic_gametimerH
