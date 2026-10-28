/***************************************************************************
 * ai_reorders.cpp — Р22: пересмотр приказов каждый ход. См. ai_reorders.h.
 ***************************************************************************/

#include "ai_reorders.h"

#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/vehicle.h"
#include "game/logic/movejob.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ai
{

namespace
{

// MAXR_REVIEW=now: причина приказа «тот же путь дальше»
constexpr const char* kContinue = "review: continue: ";

} // namespace

//------------------------------------------------------------------------------
const char* intentName (eIntent k)
{
	switch (k)
	{
		case eIntent::PerTurn: return "per turn";
		case eIntent::BuildSite: return "build site";
		case eIntent::Errand: return "errand";
		case eIntent::Evacuate: return "evacuate";
		case eIntent::Retreat: return "retreat";
		case eIntent::Refit: return "refit";
		case eIntent::Explore: return "explore";
		case eIntent::Approach: return "approach";
		default: return "unknown";
	}
}

//------------------------------------------------------------------------------
cOrderReview::cOrderReview()
{
	// опыт «вариант против варианта» (как выключатели Р7, Р17): MAXR_REVIEW=0 — как до Р22, =all — отпускать всех с путём, =now —
	// оставленные едут в заходе 1
	if (const char* env = std::getenv ("MAXR_REVIEW"); env != nullptr && env[0] != '\0')
	{
		if (env[0] == '0') mode_ = eMode::Off;
		else if (std::strcmp (env, "all") == 0) mode_ = eMode::All;
		else if (std::strcmp (env, "now") == 0) mode_ = eMode::Now;
	}
}

//------------------------------------------------------------------------------
// Вид приказа — по причине приказа Move (ai_rule_brain и модули пишут её всегда). Задуманное на ход — всё, что считается
// заново каждый ход и обрезано до хода (строй, разведка и резерв операций, сопровождение, посты, тревога базы, отход
// поддержки, приказы флота, авиации и коммандос); путь у такой машины остаётся только случайно — движок обошёл занятую клетку.
eIntent cOrderReview::classify (const std::string& r)
{
	auto starts = [&] (const char* p) { return r.rfind (p, 0) == 0; };
	if (r.empty()) return eIntent::Unknown;
	if (starts ("to build site")) return eIntent::BuildSite;
	if (starts ("to base for metal") || starts ("repairer to base for metal") || starts ("to connector path")) return eIntent::Errand;
	if (starts ("builder evacuates") || starts ("surveyor away from enemy") || starts ("surveyor: away from the enemy")
		|| starts ("surveyor: enemy may be near"))
		return eIntent::Evacuate;
	if (starts ("retreat")) return eIntent::Retreat; // к ремонтнику, за строй, к базе (и «(unsafe)»)
	if (starts ("to depot for refit") || starts ("scanner: to depot") || starts ("scanner: base is blind")) return eIntent::Refit;
	if (starts ("advance (scout)") || starts ("survey unexplored") || starts ("surveyor: ")) return eIntent::Explore;
	if (starts ("approach enemy")) return eIntent::Approach;
	return eIntent::PerTurn;
}

//------------------------------------------------------------------------------
void cOrderReview::note (const cModel& model, const std::vector<cOrder>& orders, int turn, int pass)
{
	for (const auto& o : orders)
	{
		if (o.type == eOrderType::Move)
		{
			if (o.reason.rfind (kContinue, 0) == 0) continue; // MAXR_REVIEW=now: тот же приказ дальше — намерение прежнее (и его ход)
			sIntent in;
			in.turn = turn;
			in.pass = pass;
			in.kind = classify (o.reason);
			in.reason = o.reason;
			in.dest = o.path.empty() ? o.position : o.path.back();
			in.target = o.unitId2;
			in.moveEnd = o.moveEnd;
			intents_[o.unitId] = std::move (in);
			continue;
		}
		// задание движения снимают стоп и начало стройки (cActionStop, cActionStartBuild), погрузка — машину внутрь; прочие приказы
		// (выстрел, ремонт, передача) путь не трогают, а у многих unitId — не машина (BuyUpgrades, ChangeResearch: 0)
		if ((o.type == eOrderType::StopWork || o.type == eOrderType::Build || o.type == eOrderType::BuildPath)
			&& model.getVehicleFromID (o.unitId) != nullptr)
			intents_.erase (o.unitId);
		if (o.type == eOrderType::Load && o.unitId2 != kNoUnit) intents_.erase (o.unitId2); // её грузят
	}
}

//------------------------------------------------------------------------------
void cOrderReview::beginTurn (const cModel& model, const cPlayer& player, int turn)
{
	turn_ = turn;
	released_.clear();
	decisions_.clear();
	for (auto it = intents_.begin(); it != intents_.end();)
	{
		const cVehicle* v = model.getVehicleFromID (it->first);
		if (v == nullptr || v->getOwner() != &player)
			it = intents_.erase (it);
		else
			++it;
	}
}

//------------------------------------------------------------------------------
const cOrderReview::sIntent* cOrderReview::intentOf (unsigned int id) const
{
	const auto it = intents_.find (id);
	return it == intents_.end() ? nullptr : &it->second;
}

//------------------------------------------------------------------------------
bool cOrderReview::carried (const cVehicle& v)
{
	const cMoveJob* job = v.getMoveJob();
	return job != nullptr && !job->isFinished() && !job->getPath().empty() && !v.isUnitMoving() && !v.isUnitLoaded();
}

//------------------------------------------------------------------------------
void cOrderReview::decide (const cVehicle& v, bool keep, const std::string& why)
{
	sDecision d;
	d.id = v.getId();
	d.type = v.getStaticUnitData().getDefaultName();
	if (const sIntent* in = intentOf (v.getId()))
	{
		d.old = in->reason;
		d.kind = intentName (in->kind);
		d.age = turn_ - in->turn;
		d.target = in->target;
		d.moveEnd = in->moveEnd;
	}
	else
		d.kind = intentName (eIntent::Unknown);
	if (const cMoveJob* job = v.getMoveJob())
		for (const auto& p : job->getPath())
		{
			++d.left;
			if (keep && mode_ == eMode::Now) d.path.push_back (p);
		}
	d.keep = keep;
	d.why = why;
	decisions_.push_back (std::move (d));
	++total_.carried;
	if (keep)
		++total_.kept;
	else
	{
		++total_.released;
		released_.insert (v.getId());
	}
}

//------------------------------------------------------------------------------
bool cOrderReview::moveBusy (const cVehicle& v) const
{
	return v.isUnitMoving() || (v.getMoveJob() != nullptr && !released (v.getId()));
}

//------------------------------------------------------------------------------
void cOrderReview::finish (std::vector<cOrder>& orders, int playerId)
{
	if (decisions_.empty()) return;
	auto stopOrder = [&] (unsigned int id, const std::string& why) {
		cOrder s;
		s.type = eOrderType::StopWork;
		s.playerId = playerId;
		s.unitId = id;
		s.level = "micro";
		s.reason = "review: stop the old path (" + why + ")";
		return s;
	};
	// первый приказ каждой отпущенной машины (её Move или другой приказ; погрузка — когда грузят её)
	std::map<unsigned int, size_t> first;
	for (size_t i = 0; i < orders.size(); ++i)
	{
		const cOrder& o = orders[i];
		// приказы игрока, а не юнита (unitId у них 0 — это не машина 0)
		if (o.type == eOrderType::EndTurn || o.type == eOrderType::ChangeResearch || o.type == eOrderType::BuyUpgrades) continue;
		auto mark = [&] (unsigned int id) {
			if (released (id) && first.count (id) == 0) first[id] = i;
		};
		mark (o.unitId);
		if (o.type == eOrderType::Load && o.unitId2 != kNoUnit) mark (o.unitId2);
	}
	std::vector<cOrder> out;
	out.reserve (orders.size() + decisions_.size());
	nlohmann::json units = nlohmann::json::array();
	int nCarried = 0, nKept = 0, nReleased = 0, nMoved = 0, nOther = 0, nStopped = 0;
	std::map<size_t, std::vector<cOrder>> before; // индекс приказа → стопы перед ним
	std::vector<cOrder> continues;                // MAXR_REVIEW=now: оставленные — тем же путём в заходе 1
	for (const auto& d : decisions_)
	{
		++nCarried;
		std::string now;
		if (d.keep)
		{
			++nKept;
			now = "continues";
			if (mode_ == eMode::Now && !d.path.empty()) // опыт: тот же путь — в заходе 1, а не в конце хода
			{
				cOrder m;
				m.type = eOrderType::Move;
				m.playerId = playerId;
				m.unitId = d.id;
				m.path = d.path;
				m.position = d.path.back();
				m.unitId2 = d.target;
				m.moveEnd = d.moveEnd;
				m.level = "micro";
				m.reason = std::string (kContinue) + d.old;
				continues.push_back (std::move (m));
				now = "continues now";
			}
		}
		else
		{
			++nReleased;
			const auto f = first.find (d.id);
			if (f == first.end())
			{
				out.push_back (stopOrder (d.id, d.why)); // дела нет — стоять, а не ехать по старому пути в конце хода
				++nStopped;
				++total_.stopped;
				now = "stop";
			}
			else if (orders[f->second].type == eOrderType::Move && orders[f->second].unitId == d.id)
			{
				++nMoved;
				++total_.moved;
				now = "Move: " + orders[f->second].reason; // новый путь заменяет ждущий (cModel::addMoveJob)
			}
			else
			{
				before[f->second].push_back (stopOrder (d.id, d.why)); // выстрел и погрузка задание движения не снимают
				++nOther;
				++total_.other;
				now = std::string (toString (orders[f->second].type)) + ": " + orders[f->second].reason;
			}
		}
		units.push_back ({d.id, d.type, d.old, d.kind, d.age, d.left, d.keep ? "keep" : "release", d.why, now});
	}
	for (size_t i = 0; i < orders.size(); ++i)
	{
		if (const auto b = before.find (i); b != before.end())
			for (auto& s : b->second) out.push_back (std::move (s));
		out.push_back (std::move (orders[i]));
	}
	for (auto& m : continues) out.push_back (std::move (m));
	orders = std::move (out);
	events_.push_back ({{"event", "review"}, {"level", "rule"}, {"carried", nCarried}, {"kept", nKept}, {"released", nReleased},
		{"moved", nMoved}, {"other", nOther}, {"stopped", nStopped},
		{"units", std::move (units)}}); // [id, тип, прежний приказ, вид, ходов приказу, клеток пути осталось, итог, почему, что теперь]
	// пересмотр — только заход 1: в заходах 2+ машина с путём занята, как до Р22 (отпущенная уже получила новый путь или стоп)
	decisions_.clear();
	released_.clear();
}

//------------------------------------------------------------------------------
std::vector<nlohmann::json> cOrderReview::takeEvents()
{
	auto e = std::move (events_);
	events_.clear();
	return e;
}

} // namespace ai
