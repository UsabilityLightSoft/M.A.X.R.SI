/***************************************************************************
 * ai_external_brain.cpp — внешний мозг (шаг 16, часть Б). Описание протокола —
 * ai_external_brain.h и nets/README.md.
 ***************************************************************************/

#include "ai_external_brain.h"

#include "ai_game_state.h"
#include "ai_http_client.h"
#include "ai_rule_brain.h"
#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"
#include "game/logic/turncounter.h"
#include "utility/log.h"

#include <algorithm>
#include <cstdlib>
#include <forward_list>
#include <set>
#include <utility>

namespace ai
{

namespace
{

std::shared_ptr<const cPlayer> sharedPlayer (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id)
			return p;
	return nullptr;
}

nlohmann::json posJ (const cPosition& p)
{
	return nlohmann::json::array ({p.x(), p.y()});
}

std::string typeText (const sTypeId& t)
{
	return std::to_string (t.firstPart) + "." + std::to_string (t.secondPart);
}

// приказ правил → JSON для внешней программы (что и кому правила хотят сделать)
nlohmann::json proposalJson (const cOrder& o)
{
	nlohmann::json j = {{"type", toString (o.type)}, {"unitId", o.unitId}, {"level", o.level}, {"reason", o.reason}};
	switch (o.type)
	{
		case eOrderType::Move:
			if (!o.path.empty()) j["to"] = posJ (o.path.back());
			j["pathLen"] = o.path.size();
			if (o.unitId2 != kNoUnit) // шаг 13: «нет цели» — kNoUnit (id 0 — настоящая машина)
			{
				j["unitId2"] = o.unitId2;
				j["moveEnd"] = toString (o.moveEnd);
			}
			break;
		case eOrderType::Attack:
			j["target"] = posJ (o.position);
			break;
		case eOrderType::Build:
		case eOrderType::BuildPath:
			j["building"] = typeText (o.buildingType);
			j["pos"] = posJ (o.position);
			if (o.type == eOrderType::BuildPath) j["pos2"] = posJ (o.position2);
			j["buildSpeed"] = o.buildSpeed;
			break;
		case eOrderType::ChangeBuildList:
		{
			nlohmann::json list = nlohmann::json::array();
			for (const auto& t : o.buildList) list.push_back (typeText (t));
			j["list"] = std::move (list);
			j["buildSpeed"] = o.buildSpeed;
			j["repeat"] = o.repeat;
			break;
		}
		case eOrderType::ChangeResearch:
			j["areas"] = o.researchAreas;
			break;
		case eOrderType::ActivateUnit:
		case eOrderType::FinishBuild:
			j["pos"] = posJ (o.position);
			if (o.unitId2 != kNoUnit) j["unitId2"] = o.unitId2;
			break;
		case eOrderType::Transfer:
			j["unitId2"] = o.unitId2;
			j["value"] = o.transferValue;
			j["resType"] = static_cast<int> (o.resType);
			break;
		case eOrderType::BuyUpgrades:
			j["unitType"] = typeText (o.unitType);
			j["steps"] = o.upgradeSteps;
			break;
		default:
			if (o.unitId2 != kNoUnit) j["unitId2"] = o.unitId2;
			break;
	}
	return j;
}

bool readPosJ (const nlohmann::json& j, const char* key, cPosition& out)
{
	const auto it = j.find (key);
	if (it == j.end()) return false;
	if (it->is_array() && it->size() == 2 && (*it)[0].is_number_integer() && (*it)[1].is_number_integer())
	{
		out = cPosition ((*it)[0].get<int>(), (*it)[1].get<int>());
		return true;
	}
	if (it->is_object() && it->contains ("x") && it->contains ("y") && (*it)["x"].is_number_integer() && (*it)["y"].is_number_integer())
	{
		out = cPosition ((*it)["x"].get<int>(), (*it)["y"].get<int>());
		return true;
	}
	return false;
}

bool readTypeJ (const nlohmann::json& v, sTypeId& out)
{
	if (!v.is_string()) return false;
	const std::string s = v.get<std::string>();
	const auto dot = s.find ('.');
	if (dot == std::string::npos) return false;
	try
	{
		out = sTypeId (std::stoi (s.substr (0, dot)), std::stoi (s.substr (dot + 1)));
	}
	catch (...)
	{
		return false;
	}
	return true;
}

// шаг 13, часть 2: def — если поля нет (или не id): для unitId2 — kNoUnit («нет цели»), 0 — машина с id 0
unsigned int idJ (const nlohmann::json& j, const char* key, unsigned int def = 0)
{
	const auto it = j.find (key);
	if (it == j.end() || !it->is_number_integer() || it->get<long long>() < 0) return def;
	return static_cast<unsigned int> (it->get<long long>());
}

int intJ (const nlohmann::json& j, const char* key, int def)
{
	const auto it = j.find (key);
	return (it != j.end() && it->is_number_integer()) ? it->get<int>() : def;
}

const std::pair<const char*, eOrderType> kTypes[] = {
	{"Move", eOrderType::Move}, {"Attack", eOrderType::Attack}, {"Build", eOrderType::Build},
	{"BuildPath", eOrderType::BuildPath}, {"ChangeBuildList", eOrderType::ChangeBuildList},
	{"ChangeResearch", eOrderType::ChangeResearch}, {"Repair", eOrderType::Repair}, {"Rearm", eOrderType::Rearm},
	{"Transfer", eOrderType::Transfer}, {"StartWork", eOrderType::StartWork}, {"StopWork", eOrderType::StopWork},
	{"ToggleSentry", eOrderType::ToggleSentry}, {"ToggleManualFire", eOrderType::ToggleManualFire},
	{"ToggleLayMines", eOrderType::ToggleLayMines}, {"ToggleCollectMines", eOrderType::ToggleCollectMines},
	{"Load", eOrderType::Load}, {"ActivateUnit", eOrderType::ActivateUnit}, {"FinishBuild", eOrderType::FinishBuild},
	{"Disable", eOrderType::Disable}, {"Steal", eOrderType::Steal}};

// JSON внешней программы → cOrder. false — приказ не разобран (why — почему).
bool convertJ (const nlohmann::json& j, int playerId, const cModel& model, const cMapView& view, cOrder& o, std::string& why)
{
	if (!j.is_object() || !j.contains ("type") || !j["type"].is_string())
	{
		why = "нет type";
		return false;
	}
	const std::string t = j["type"].get<std::string>();
	const auto found = std::find_if (std::begin (kTypes), std::end (kTypes), [&t] (const auto& kv) { return t == kv.first; });
	if (found == std::end (kTypes))
	{
		why = "тип не поддержан: " + t.substr (0, 40);
		return false;
	}
	o.type = found->second;
	o.playerId = playerId;
	o.level = "external";
	o.reason = j.contains ("reason") && j["reason"].is_string() ? "ext: " + j["reason"].get<std::string>().substr (0, 120) : "ext";
	o.unitId = idJ (j, "unitId");
	o.unitId2 = idJ (j, "unitId2", kNoUnit);
	switch (o.type)
	{
		case eOrderType::Move:
		{
			const cVehicle* v = model.getVehicleFromID (o.unitId);
			if (v == nullptr || v->getOwner() == nullptr || v->getOwner()->getId() != playerId)
			{
				why = "Move: не своя машина";
				return false;
			}
			if (const auto p = j.find ("path"); p != j.end() && p->is_array())
			{
				for (const auto& c : *p)
				{
					nlohmann::json wrap = {{"c", c}};
					cPosition cell;
					if (!readPosJ (wrap, "c", cell))
					{
						why = "Move: клетка пути";
						return false;
					}
					o.path.push_back (cell);
				}
			}
			else
			{
				cPosition to;
				if (!readPosJ (j, "to", to))
				{
					why = "Move: нет to";
					return false;
				}
				if (!model.getMap()->isValidPosition (to) || v->getPosition() == to)
				{
					why = "Move: клетка вне карты или та же";
					return false;
				}
				cPathCalculator pc (*v, view, to, false);
				for (const auto& c : pc.calcPath())
					o.path.push_back (c);
			}
			if (o.path.empty())
			{
				why = "Move: путь не найден";
				return false;
			}
			o.position = o.path.back();
			if (const unsigned int fire = idJ (j, "fire", kNoUnit); fire != kNoUnit)
			{
				o.unitId2 = fire;
				o.moveEnd = eMoveEnd::Attack;
			}
			else if (const auto me = j.find ("moveEnd"); me != j.end() && me->is_string())
				o.moveEnd = me->get<std::string>() == "load" ? eMoveEnd::Load : (me->get<std::string>() == "getIn" ? eMoveEnd::GetIn : eMoveEnd::Attack);
			return true;
		}
		case eOrderType::Attack:
			if (!readPosJ (j, "target", o.position))
			{
				why = "Attack: нет target";
				return false;
			}
			o.unitId2 = kNoUnit; // цель — клетка; id в журнале допишет движок
			return true;
		case eOrderType::Build:
		case eOrderType::BuildPath:
			if (!j.contains ("building") || !readTypeJ (j["building"], o.buildingType) || !readPosJ (j, "pos", o.position))
			{
				why = "Build: building, pos";
				return false;
			}
			if (o.type == eOrderType::BuildPath && !readPosJ (j, "pos2", o.position2))
			{
				why = "BuildPath: pos2";
				return false;
			}
			o.buildSpeed = intJ (j, "buildSpeed", 0);
			return true;
		case eOrderType::ChangeBuildList:
			if (!j.contains ("list") || !j["list"].is_array())
			{
				why = "ChangeBuildList: list";
				return false;
			}
			for (const auto& item : j["list"])
			{
				sTypeId id;
				if (!readTypeJ (item, id))
				{
					why = "ChangeBuildList: тип «a.b»";
					return false;
				}
				o.buildList.push_back (id);
			}
			o.buildSpeed = intJ (j, "buildSpeed", 0);
			o.repeat = j.contains ("repeat") && j["repeat"].is_boolean() && j["repeat"].get<bool>();
			return true;
		case eOrderType::ChangeResearch:
			if (!j.contains ("areas") || !j["areas"].is_array() || j["areas"].size() != o.researchAreas.size())
			{
				why = "ChangeResearch: areas[8]";
				return false;
			}
			for (std::size_t i = 0; i < o.researchAreas.size(); ++i)
				o.researchAreas[i] = j["areas"][i].is_number_integer() ? j["areas"][i].get<int>() : 0;
			return true;
		case eOrderType::ActivateUnit:
		case eOrderType::FinishBuild:
			if (!readPosJ (j, "pos", o.position))
			{
				why = "нет pos";
				return false;
			}
			return true;
		case eOrderType::Transfer:
			o.transferValue = intJ (j, "value", 0);
			o.resType = static_cast<eResourceType> (std::clamp (intJ (j, "resType", 0), 0, 3));
			return true;
		default:
			return true;
	}
}

} // namespace

//------------------------------------------------------------------------------
sExternalConfig sExternalConfig::fromEnv()
{
	sExternalConfig c;
	if (const char* v = std::getenv ("MAXR_EXT_URL"); v != nullptr && v[0] != '\0') c.url = v;
	if (const char* v = std::getenv ("MAXR_EXT_MODE"); v != nullptr && v[0] != '\0') c.mode = v;
	if (const char* v = std::getenv ("MAXR_EXT_TIMEOUT_MS"); v != nullptr && v[0] != '\0') c.timeoutMs = std::max (1000, std::atoi (v));
	if (c.mode != "full") c.mode = "assist";
	return c;
}

cExternalBrain::cExternalBrain (sExternalConfig config) :
	config_ (std::move (config))
{
	if (config_.mode == "assist") rule_ = std::make_unique<cRuleBrain>();
}

cExternalBrain::~cExternalBrain() = default;

std::vector<cOrder> cExternalBrain::decideTurn (int playerId, const cModel& model)
{
	std::vector<cOrder> proposal;
	bool ruleWants = false;
	if (rule_)
	{
		proposal = rule_->decideTurn (playerId, model);
		ruleWants = rule_->wantsAnotherPass();
	}
	return decide (playerId, model, 1, std::move (proposal), ruleWants);
}

std::vector<cOrder> cExternalBrain::decidePass (int playerId, const cModel& model, int pass)
{
	std::vector<cOrder> proposal;
	bool ruleWants = false;
	if (rule_)
	{
		proposal = rule_->decidePass (playerId, model, pass);
		ruleWants = rule_->wantsAnotherPass();
	}
	return decide (playerId, model, pass, std::move (proposal), ruleWants);
}

void cExternalBrain::noteDisabled (const std::vector<unsigned int>& ids)
{
	if (rule_) rule_->noteDisabled (ids);
}

std::vector<nlohmann::json> cExternalBrain::takeJournalEvents()
{
	return std::exchange (events_, {});
}

std::vector<cOrder> cExternalBrain::decide (int playerId, const cModel& model, int pass, std::vector<cOrder> proposal, bool ruleWants)
{
	if (rule_)
		for (auto& ev : rule_->takeJournalEvents())
			events_.push_back (std::move (ev));

	nlohmann::json req;
	req["player"] = playerId;
	req["turn"] = model.getTurnCounter()->getTurn();
	req["pass"] = pass;
	req["mode"] = config_.mode;
	req["digest"] = buildGameStateDigest (model, playerId);
	req["digest"]["visibility"] = buildVisibility (model, playerId);
	nlohmann::json prop = nlohmann::json::array();
	for (const auto& o : proposal) prop.push_back (proposalJson (o));
	req["proposal"] = std::move (prop);
	req["ruleWantsAnotherPass"] = ruleWants;
	req["events"] = events_; // события правил этого захода (hold, world, операции)

	++requests_;
	const auto res = httpPostJson (config_.url, req.dump(), config_.timeoutMs);
	nlohmann::json note = {{"event", "external"}, {"level", "external"}, {"ms", res.elapsedMs}, {"proposal", proposal.size()}};
	nlohmann::json reply;
	if (res.ok && res.status == 200)
	{
		try
		{
			reply = nlohmann::json::parse (res.body);
		}
		catch (const std::exception& e)
		{
			note["error"] = std::string ("JSON: ") + e.what();
		}
	}
	else
		note["error"] = res.ok ? "HTTP " + std::to_string (res.status) : res.error;
	if (note.contains ("error") || !reply.is_object())
	{
		++failures_;
		if (!note.contains ("error")) note["error"] = "ответ — не объект";
		note["failures"] = failures_;
		Log.warn ("ai_external: " + note["error"].get<std::string>());
		events_.push_back (std::move (note));
		wantPass_ = ruleWants;
		return rule_ ? proposal : std::vector<cOrder>{};
	}

	const cMapView view (model.getMap(), sharedPlayer (model, playerId));
	std::vector<cOrder> own;
	std::set<unsigned int> taken;
	nlohmann::json bad = nlohmann::json::array();
	if (const auto h = reply.find ("hold"); h != reply.end() && h->is_array())
		for (const auto& id : *h)
			if (id.is_number_integer() && id.get<long long>() >= 0) taken.insert (static_cast<unsigned int> (id.get<long long>()));
	if (const auto os = reply.find ("orders"); os != reply.end() && os->is_array())
	{
		for (const auto& j : *os)
		{
			if (j.is_object() && j.value ("type", std::string()) == "Hold")
			{
				taken.insert (idJ (j, "unitId"));
				continue;
			}
			cOrder o;
			std::string why;
			if (convertJ (j, playerId, model, view, o, why))
			{
				if (o.unitId != 0 || o.type != eOrderType::ChangeResearch) taken.insert (o.unitId);
				own.push_back (std::move (o));
			}
			else if (bad.size() < 20)
				bad.push_back (why);
		}
	}
	const bool replace = config_.mode == "full" || reply.value ("replace", false);
	std::vector<cOrder> out;
	std::size_t kept = 0;
	if (!replace)
		for (auto& o : proposal)
			if (taken.count (o.unitId) == 0 || (o.type == eOrderType::ChangeResearch && o.unitId == 0))
			{
				out.push_back (std::move (o));
				++kept;
			}
	for (auto& o : own) out.push_back (std::move (o));
	if (const auto w = reply.find ("wantAnotherPass"); w != reply.end() && w->is_boolean())
		wantPass_ = w->get<bool>();
	else
		wantPass_ = ruleWants;
	note["orders"] = own.size();
	note["kept"] = kept;
	note["taken"] = taken.size();
	if (!bad.empty()) note["unparsed"] = std::move (bad);
	events_.push_back (std::move (note));
	return out;
}

} // namespace ai
