/***************************************************************************
 * ai_eco.cpp — эко-сферы и победа по очкам (Р7). Описание — в заголовке.
 ***************************************************************************/

#include "ai_eco.h"

#include "game/data/gamesettings.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/unitdata.h"

#include <algorithm>
#include <cstdlib>

namespace ai
{

namespace
{

constexpr int kMaxSpheres = 4;        // «по ходам» — сфер не больше
constexpr int kMaxSpheresPoints = 6;  // «по очкам» — сферы и есть цель
constexpr int kLateMin = 60;          // «предел близко»: осталось ходов не больше max (kLateMin, предел / kLateDiv)
constexpr int kLateDiv = 4;
constexpr int kStallTurn = 100;       // застой — не раньше этого хода…
constexpr int kStallWindow = 40;      // …и за столько ходов своих потерь не больше kStallLosses
constexpr int kStallLosses = 1;
constexpr int kStallIncome = 30;      // …и добыча металла не ниже (сфера при застое — из лишнего)
constexpr int kTwoSpheresIncome = 30; // добыча металла, с которой «предел близко» — 2 сферы
constexpr int kStatusEvery = 20;

} // namespace

const sID cEcoPlan::kIdEcoSphere (1, 35);

//------------------------------------------------------------------------------
void cEcoPlan::event (const char* what, nlohmann::json extra)
{
	nlohmann::json e = {{"event", "eco"}, {"level", "strategy"}, {"what", what}, {"turn", turn_}};
	if (extra.is_object())
		for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
	events_.push_back (std::move (e));
}

std::vector<nlohmann::json> cEcoPlan::takeEvents()
{
	std::vector<nlohmann::json> res;
	res.swap (events_);
	return res;
}

const char* cEcoPlan::victoryName() const
{
	return victory_ == 1 ? "points" : (victory_ == 2 ? "death" : "turns");
}

nlohmann::json cEcoPlan::toJson() const
{
	return {{"victory", victoryName()}, {"wanted", wanted_}, {"urgent", urgent_}, {"spheres", counts_.own}, {"built", counts_.ownBuilt},
		{"enemySpheres", counts_.enemy}, {"score", counts_.ownScore}, {"enemyScore", counts_.enemyScore},
		{"turnsLeft", counts_.turnsLeft}, {"stall", stall_}, {"lost", counts_.lostSpheres}, {"why", why_}};
}

//------------------------------------------------------------------------------
void cEcoPlan::update (const cModel& model, const cPlayer& player, int income, int turn)
{
	turn_ = turn;
	const auto settings = model.getGameSettings();
	victory_ = 0;
	int victoryTurns = 0;
	int victoryPoints = 0;
	if (settings)
	{
		switch (settings->victoryConditionType)
		{
			case eGameSettingsVictoryCondition::Points: victory_ = 1; break;
			case eGameSettingsVictoryCondition::Death: victory_ = 2; break;
			case eGameSettingsVictoryCondition::Turns: victory_ = 0; break;
		}
		victoryTurns = static_cast<int> (settings->victoryTurns);
		victoryPoints = static_cast<int> (settings->victoryPoints);
	}

	// очки и сферы сторон — окно отчётов (видно каждому игроку)
	const int lostSpheres = counts_.lostSpheres;
	counts_ = {};
	counts_.lostSpheres = lostSpheres;
	counts_.own = player.getNumEcoSpheres();
	counts_.ownScore = player.getScore();
	for (const auto& p : model.getPlayerList())
	{
		if (p->getId() == player.getId() || p->isDefeated) continue;
		counts_.enemy += p->getNumEcoSpheres();
		counts_.enemyScore = std::max (counts_.enemyScore, p->getScore());
	}
	counts_.turnsLeft = victory_ == 0 && victoryTurns > 0 ? victoryTurns - turn : -1;

	// свои сферы: построенные; пропавшие — в журнал (очки, что она принесла, потеряны)
	std::vector<unsigned int> now;
	for (const auto& b : player.getBuildings())
		if (b->getStaticUnitData().ID == kIdEcoSphere)
		{
			now.push_back (b->getId());
			++counts_.ownBuilt;
		}
	for (unsigned int id : spheres_)
		if (std::find (now.begin(), now.end(), id) == now.end())
		{
			++counts_.lostSpheres;
			event ("sphere lost", {{"unitId", id}, {"score", counts_.ownScore}, {"why", "destroyed: the points it earned are gone"}});
		}
	for (unsigned int id : now)
		if (std::find (spheres_.begin(), spheres_.end(), id) == spheres_.end())
			event ("sphere built", {{"unitId", id}, {"spheres", counts_.ownBuilt}});
	spheres_ = std::move (now);

	// застой: с хода kStallTurn за kStallWindow ходов своих потерь не больше kStallLosses
	const auto& stat = player.getGameOverStat();
	losses_[turn] = static_cast<int> (stat.lostVehiclesCount + stat.lostBuildingsCount);
	for (auto it = losses_.begin(); it != losses_.end() && it->first < turn - kStallWindow - 5;) it = losses_.erase (it);
	stall_ = false;
	if (turn >= kStallTurn)
	{
		const auto old = losses_.lower_bound (turn - kStallWindow);
		if (old != losses_.end() && old->first <= turn - kStallWindow + 2)
			stall_ = losses_[turn] - old->second <= kStallLosses;
	}

	int want = 0;
	bool urgent = false;
	std::string why;
	if (victory_ == 1)
	{
		want = std::clamp (2 + income / 15, 2, kMaxSpheresPoints);
		urgent = true;
		why = "victory by points (" + std::to_string (victoryPoints) + "): eco-spheres are the goal";
	}
	else if (victory_ == 2)
		why = "victory by elimination: points do not decide";
	else
	{
		const int lateAt = std::max (kLateMin, victoryTurns / kLateDiv);
		if (counts_.enemy > 0 || counts_.enemyScore > 0)
		{
			want = counts_.enemy + 1;
			urgent = true;
			why = "the enemy builds eco-spheres (" + std::to_string (counts_.enemy) + "): one more than he has";
		}
		if (counts_.turnsLeft >= 0 && counts_.turnsLeft <= lateAt)
		{
			const int w = income >= kTwoSpheresIncome ? 2 : 1;
			if (w > want)
			{
				want = w;
				why = std::string (why.empty() ? "" : why + "; ") + "turn limit near (<= " + std::to_string (lateAt)
					+ " turns left): a draw turns into a win by points";
			}
			urgent = true;
		}
		// застой — сфера из лишнего: экономика выросла (Gen-часть r7d: сфера с хода ~118 на добыче ниже отнимала конструктор и
		// энергию у фабрик — по материалу D 0.538 против 0.586 без сфер)
		if (stall_ && want < 1 && income >= kStallIncome)
		{
			want = 1;
			why = "the game is stalled (own losses <= " + std::to_string (kStallLosses) + " in " + std::to_string (kStallWindow)
				+ " turns): points decide at the turn limit";
		}
		want = std::min (want, kMaxSpheres);
	}
	// опыт «вариант против варианта»: MAXR_ECO=0 — сфер не строить (кроме «по очкам»)
	if (const char* off = std::getenv ("MAXR_ECO"); off != nullptr && off[0] == '0' && victory_ != 1)
	{
		want = 0;
		urgent = false;
		why = "MAXR_ECO=0";
	}
	// построенные не бросаем: держим (их очки пропадут только при уничтожении)
	wanted_ = std::max (want, want > 0 ? counts_.ownBuilt : 0);
	urgent_ = urgent && wanted_ > counts_.ownBuilt;
	pressing_ = urgent && wanted_ > 0;
	why_ = why.empty() ? std::string ("no need: the game is not stalled, the turn limit is far, the enemy has no eco-spheres") : why;
	if (wanted_ != lastWanted_ || (why_ != lastWhy_ && wanted_ > 0))
	{
		event ("program", toJson());
		lastWanted_ = wanted_;
		lastWhy_ = why_;
	}
	else if (turn % kStatusEvery == 0 && (counts_.ownBuilt > 0 || counts_.enemy > 0 || wanted_ > 0))
		event ("status", toJson());
}

} // namespace ai
