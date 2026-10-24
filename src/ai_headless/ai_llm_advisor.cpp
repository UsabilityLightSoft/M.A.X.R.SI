/***************************************************************************
 * ai_llm_advisor.cpp — LLM-советник: запрос плана, разбор, директивы,
 * повтор с текстом ошибки, предохранитель, слияние с правилами.
 ***************************************************************************/

#include "ai_llm_advisor.h"

#include "ai_decision_journal.h"
#include "ai_game_state.h"
#include "ai_http_client.h"
#include "ai_llm_directives.h"
#include "ai_order_validator.h"
#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/unit.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"
#include "game/logic/turncounter.h"
#include "game/logic/upgradecalculator.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <forward_list>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <utility>

namespace ai
{

namespace
{

constexpr std::size_t kContentLogBytes = 500;  // сколько content писать в llm.jsonl
constexpr std::size_t kShortLogBytes = 300;    // strategy, исходный приказ
constexpr std::size_t kReasonBytes = 200;      // reason приказа в журнале
constexpr std::size_t kMaxLlmOrders = 100;     // больше приказов из одного плана не разбираем
constexpr std::size_t kRetryEchoBytes = 4000;  // ответ модели, возвращаемый ей при повторе
constexpr std::size_t kRetryProblems = 6;      // сколько причин отказа показать модели
constexpr int kMinRetryMs = 3000;              // меньше осталось от бюджета хода — не повторяем
constexpr int kBreakerFailures = 3;            // транспортных отказов подряд → предохранитель
constexpr int kBreakerSkipTurns = 5;           // столько своих ходов не звать модель

bool g_modelWarned = false;       // «MAXR_LLM_MODEL не задана» — одна запись на процесс
bool g_schemaDisabled = false;    // сервер отверг response_format — до конца партии без схемы
bool g_reasoningDisabled = false; // сервер отверг reasoning_effort — до конца партии без него
bool g_switchesPrinted = false;   // строка выключателей в stdout — одна на процесс

// обрезать до maxBytes, не разрывая символ UTF-8
std::string clipUtf8 (const std::string& s, std::size_t maxBytes)
{
	if (s.size() <= maxBytes) return s;
	std::size_t cut = maxBytes;
	while (cut > 0 && (static_cast<unsigned char> (s[cut]) & 0xC0) == 0x80) --cut;
	return s.substr (0, cut) + "…";
}

std::string lowerAscii (std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z') c = static_cast<char> (c - 'A' + 'a');
	return s;
}

// имена областей исследований — как в digest; индекс = cResearch::eResearchArea
constexpr const char* kResearchNames[] = {"attack", "shots", "range", "armor", "hitpoints", "speed", "scan", "cost"};
static_assert (std::size (kResearchNames) == static_cast<std::size_t> (cResearch::kNrResearchAreas), "области исследований разошлись с cResearch");

// --- системный промпт ---------------------------------------------------------

// форма ответа: только включённые директивы и orders
std::string replyForm (const sLlmSwitches& sw)
{
	std::string s = "{\"strategy\":\"one short sentence\"";
	if (sw.anyDirective())
	{
		std::string d;
		if (sw.production) d += "\"production\":{...}";
		if (sw.research) d += std::string (d.empty() ? "" : ",") + "\"research\":{...}";
		if (sw.army) d += std::string (d.empty() ? "" : ",") + "\"army\":{...}";
		s += ",\"directives\":{" + d + "}";
	}
	if (sw.orders) s += ",\"orders\":[]";
	return s + "}";
}

std::string percentText (double share)
{
	return std::to_string (static_cast<int> (share * 100.0 + 0.5)) + "%";
}

// выключенных директив и orders в промпте нет: модель о них не знает
std::string systemPrompt (int playerId, const sLlmSwitches& sw)
{
	const std::string p = std::to_string (playerId);
	std::string s = "You are the strategic advisor of AI player " + p + " in M.A.X.R. (turn-based strategy, remake of M.A.X.).\n"
		"A built-in rule AI plays every turn (economy, construction, combat micro). You steer it with "
		+ std::string (sw.anyDirective() ? "a few high-level directives" : "rare direct orders for single units") + ".\n"
		"The user message is JSON: the game state seen by player " + p + " (fog of war applies), plus \"catalog\" "
		"(vehicles our factories can build: cost and combat stats) and \"summary\" (our base, factories and queues, army, nearest visible enemies).\n"
		"Reply with ONLY one JSON object, no prose, no markdown:\n" + replyForm (sw) + "\n";
	if (sw.anyDirective())
	{
		s += "Directives (each is optional; omit one to leave that area to the rule AI):\n";
		if (sw.production)
		{
			s += "- production: {\"<catalog id>\": weight 1-10, ...} - our factories build these vehicle types in a repeating queue "
				"proportional to the weights, e.g. {\"0.32\":3,\"0.21\":1}. Only ids from catalog. The rule AI still orders constructors when the economy needs them.\n";
			s += "  Catalog cost is metal per vehicle: cheap mass-produced vehicles are strong too";
			if (sw.prodBaseShare > 0.0)
				s += " (the rule AI's base vehicle " + llmBaseVehicleId() + " always keeps at least " + percentText (sw.prodBaseShare)
					+ (sw.prodShareByMetal ? " of each queue's metal)" : " of each queue)");
			s += ".\n";
		}
		if (sw.research)
			s += "- research: {\"attack|shots|range|armor|hitpoints|speed|scan|cost\": weight 1-10} - working research centers are split by these weights.\n";
		if (sw.army)
			s += "- army: {\"mode\":\"attack|gather|defend|explore\",\"target\":{\"x\":int,\"y\":int}} - free combat units that are not fighting: "
				"attack = advance toward target, gather = assemble near target, defend = return to our base, explore = scout (rule AI). "
				"Units in combat always follow the rule AI. target is required for attack and gather and must be inside mapSize.\n";
		s += "Directives stay in effect only while you send them: send every turn the ones you want now.\n";
	}
	if (sw.orders)
	{
		s += std::string ("orders (") + (sw.anyDirective() ? "optional, rarely needed" : "optional") + ", at most 5): direct orders for single special units, objects with \"type\" and fields"
			+ (sw.army ? " (do not use orders to move the army: use the army directive)" : "") + ":\n"
			"Move: unitId, to (cell); Attack: unitId, target (cell); Build: unitId (constructor), building (typeId), pos; "
			"ChangeBuildList: unitId (factory), list (typeIds), repeat; Repair, Rearm, Load: unitId, targetId; "
			"Transfer: unitId, targetId, value, res (metal|oil|gold); StartWork, StopWork, ToggleSentry: unitId.\n"
			"unitId must be our own unit (owner == " + p + "); ";
	}
	s += "cells are {\"x\":int,\"y\":int}; type ids are \"a.b\" (digest typeId \"00 32\" is \"0.32\").";
	if (sw.orders) s += " Never send EndTurn or SelfDestroy.";
	return s;
}

// сообщение пользователя при повторе: что не так и что ждём
std::string retryMessage (const std::string& why, const std::vector<std::string>& problems, const sLlmSwitches& sw)
{
	std::string s = "Your previous reply was not usable: " + why + ".";
	if (!problems.empty())
	{
		s += " Problems:";
		for (std::size_t i = 0; i < problems.size() && i < kRetryProblems; ++i)
			s += (i == 0 ? " " : "; ") + clipUtf8 (problems[i], 200);
		s += ".";
	}
	s += " Reply again with ONLY one JSON object of the form " + replyForm (sw) + ":";
	std::string rules;
	if (sw.production) rules += " production keys only from \"catalog\",";
	if (sw.army) rules += " army target inside mapSize,";
	if (sw.orders) rules += " unitId only of our own units,";
	if (!rules.empty()) rules.back() = '.';
	return s + (rules.empty() ? std::string (" nothing else.") : rules);
}

// --- разбор ответа -------------------------------------------------------------

// вырезать <think>…</think> и ```-ограждения, взять объект от первой '{' до последней '}'
bool extractPlanText (const std::string& content, std::string& out, std::string& why)
{
	std::string s = content;
	for (;;)
	{
		const auto b = s.find ("<think>");
		if (b == std::string::npos) break;
		const auto e = s.find ("</think>", b);
		if (e == std::string::npos)
		{
			s.erase (b); // незакрытое рассуждение — до конца
			break;
		}
		s.erase (b, e + 8 - b);
	}
	// шаблон чата может отдать только закрывающий тег — всё до него рассуждение
	const auto lone = s.rfind ("</think>");
	if (lone != std::string::npos) s.erase (0, lone + 8);

	// ```json … ``` — берём содержимое первого ограждения, если в нём объект
	const auto fence = s.find ("```");
	if (fence != std::string::npos)
	{
		const auto lineEnd = s.find ('\n', fence);
		if (lineEnd != std::string::npos)
		{
			const auto close = s.find ("```", lineEnd);
			const std::string inner = s.substr (lineEnd + 1, close == std::string::npos ? std::string::npos : close - lineEnd - 1);
			if (inner.find ('{') != std::string::npos) s = inner;
		}
	}

	const auto first = s.find ('{');
	const auto last = s.rfind ('}');
	if (first == std::string::npos || last == std::string::npos || last < first)
	{
		why = "в ответе нет JSON-объекта";
		return false;
	}
	out = s.substr (first, last - first + 1);
	return true;
}

// --- чтение полей приказа -------------------------------------------------------

// целое значение v (what — имя поля для причины)
bool readIntValue (const nlohmann::json& v, const std::string& what, int& out, std::string& why)
{
	if (!v.is_number_integer())
	{
		why = "неверный тип поля " + what + ": нужно целое число";
		return false;
	}
	if (v.is_number_unsigned() ? v.get<unsigned long long>() > static_cast<unsigned long long> (INT_MAX)
	                           : (v.get<long long>() < INT_MIN || v.get<long long>() > INT_MAX))
	{
		why = "поле " + what + ": число вне диапазона";
		return false;
	}
	out = static_cast<int> (v.get<long long>());
	return true;
}

bool readInt (const nlohmann::json& j, const char* key, int& out, std::string& why, bool required = true)
{
	const auto it = j.find (key);
	if (it == j.end() || it->is_null())
	{
		if (!required) return true;
		why = std::string ("нет обязательного поля ") + key;
		return false;
	}
	return readIntValue (*it, key, out, why);
}

bool readId (const nlohmann::json& j, const char* key, unsigned int& out, std::string& why)
{
	int v = 0;
	if (!readInt (j, key, v, why)) return false;
	if (v < 0)
	{
		why = std::string ("поле ") + key + ": id не может быть отрицательным";
		return false;
	}
	out = static_cast<unsigned int> (v);
	return true;
}

bool readBool (const nlohmann::json& j, const char* key, bool& out, std::string& why)
{
	const auto it = j.find (key);
	if (it == j.end() || it->is_null()) return true; // необязательное
	if (!it->is_boolean())
	{
		why = std::string ("неверный тип поля ") + key + ": нужно true/false";
		return false;
	}
	out = it->get<bool>();
	return true;
}

bool readPos (const nlohmann::json& j, const char* key, cPosition& out, std::string& why)
{
	const auto it = j.find (key);
	if (it == j.end() || it->is_null())
	{
		why = std::string ("нет обязательного поля ") + key;
		return false;
	}
	if (!it->is_object())
	{
		why = std::string ("неверный тип поля ") + key + ": нужен объект {\"x\":int,\"y\":int}";
		return false;
	}
	int x = 0;
	int y = 0;
	if (!readInt (*it, "x", x, why) || !readInt (*it, "y", y, why))
	{
		why = std::string ("поле ") + key + ": " + why;
		return false;
	}
	out = cPosition (x, y);
	return true;
}

bool readTypeIdValue (const nlohmann::json& v, const std::string& what, sTypeId& out, std::string& why)
{
	if (!v.is_string())
	{
		why = "неверный тип поля " + what + ": нужна строка «a.b»";
		return false;
	}
	if (!llmParseTypeId (v.get<std::string>(), out))
	{
		why = what + ": sID не в формате «a.b»: «" + clipUtf8 (v.get<std::string>(), 40) + "»";
		return false;
	}
	return true;
}

bool readTypeId (const nlohmann::json& j, const char* key, sTypeId& out, std::string& why)
{
	const auto it = j.find (key);
	if (it == j.end() || it->is_null())
	{
		why = std::string ("нет обязательного поля ") + key;
		return false;
	}
	return readTypeIdValue (*it, key, out, why);
}

// --- типы приказов ----------------------------------------------------------------

bool orderTypeFromString (const std::string& s, eOrderType& out)
{
	for (int t = static_cast<int> (eOrderType::EndTurn); t <= static_cast<int> (eOrderType::FinishBuild); ++t)
	{
		const auto type = static_cast<eOrderType> (t);
		if (s == toString (type))
		{
			out = type;
			return true;
		}
	}
	return false;
}

// путь как в rule brain: forward_list (front = следующий шаг) → вектор без старта
std::vector<cPosition> pathToVector (const std::forward_list<cPosition>& path)
{
	std::vector<cPosition> res;
	for (const auto& p : path)
		res.push_back (p);
	return res;
}

std::shared_ptr<const cPlayer> sharedPlayerOf (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id)
			return p;
	return nullptr;
}

// JSON-приказ → cOrder. playerId — всегда свой; поле игрока из JSON игнорируется.
bool convertOrder (const nlohmann::json& j, int playerId, const cModel& model, const cMapView& view, cOrder& o, std::string& why)
{
	if (!j.is_object())
	{
		why = "приказ — не JSON-объект";
		return false;
	}
	const auto typeIt = j.find ("type");
	if (typeIt == j.end() || !typeIt->is_string())
	{
		why = "нет поля type (строка)";
		return false;
	}
	const std::string typeText = typeIt->get<std::string>();
	if (!orderTypeFromString (typeText, o.type))
	{
		why = "неизвестный тип приказа «" + clipUtf8 (typeText, 40) + "»";
		return false;
	}
	if (o.type == eOrderType::EndTurn)
	{
		why = "EndTurn: ход завершает движок, не советник";
		return false;
	}
	if (o.type == eOrderType::SelfDestroy)
	{
		why = "SelfDestroy: советнику запрещено";
		return false;
	}

	o.playerId = playerId;
	o.level = "llm";
	std::string reason;
	if (const auto r = j.find ("reason"); r != j.end() && r->is_string()) reason = clipUtf8 (r->get<std::string>(), kReasonBytes);
	o.reason = reason.empty() ? "llm" : "llm: " + reason;

	const std::string t = toString (o.type);
	auto failed = [&why, &t]() {
		why = t + ": " + why;
		return false;
	};

	if (o.type != eOrderType::ChangeResearch && !readId (j, "unitId", o.unitId, why)) return failed();

	switch (o.type)
	{
		case eOrderType::Move:
		{
			cPosition to;
			if (!readPos (j, "to", to, why)) return failed();
			o.position = to; // чтобы журнал показывал цель
			const cVehicle* v = model.getVehicleFromID (o.unitId);
			// чужой/несуществующий юнит — путь не считаем, причину даст валидатор
			if (v == nullptr || v->getOwner() == nullptr || v->getOwner()->getId() != playerId) return true;
			if (!model.getMap()->isValidPosition (to))
			{
				why = "Move: клетка вне карты";
				return false;
			}
			if (v->getPosition() == to)
			{
				why = "Move: юнит уже в этой клетке";
				return false;
			}
			cPathCalculator pc (*v, view, to, false);
			o.path = pathToVector (pc.calcPath());
			if (o.path.empty())
			{
				why = "Move: путь не найден";
				return false;
			}
			return true;
		}
		case eOrderType::Attack:
			if (!readPos (j, "target", o.position, why)) return failed();
			return true;
		case eOrderType::Build:
		case eOrderType::BuildPath:
			if (!readTypeId (j, "building", o.buildingType, why)) return failed();
			if (!readPos (j, "pos", o.position, why)) return failed();
			if (o.type == eOrderType::BuildPath && !readPos (j, "pos2", o.position2, why)) return failed();
			if (!readInt (j, "buildSpeed", o.buildSpeed, why, false)) return failed();
			return true;
		case eOrderType::ChangeBuildList:
		{
			const auto list = j.find ("list");
			if (list == j.end() || list->is_null())
			{
				why = "нет обязательного поля list";
				return failed();
			}
			if (!list->is_array())
			{
				why = "неверный тип поля list: нужен массив «a.b»";
				return failed();
			}
			for (const auto& item : *list)
			{
				sTypeId id;
				if (!readTypeIdValue (item, "list[]", id, why)) return failed();
				o.buildList.push_back (id);
			}
			if (!readBool (j, "repeat", o.repeat, why)) return failed();
			if (!readInt (j, "buildSpeed", o.buildSpeed, why, false)) return failed();
			return true;
		}
		case eOrderType::ChangeResearch:
		{
			const auto areas = j.find ("areas");
			if (areas == j.end() || areas->is_null())
			{
				why = "нет обязательного поля areas";
				return failed();
			}
			if (!areas->is_object())
			{
				why = "неверный тип поля areas: нужен объект {область: центров}";
				return failed();
			}
			o.researchAreas.fill (0);
			for (const auto& [name, value] : areas->items())
			{
				std::size_t idx = std::size (kResearchNames);
				for (std::size_t i = 0; i < std::size (kResearchNames); ++i)
					if (name == kResearchNames[i]) idx = i;
				if (idx == std::size (kResearchNames))
				{
					why = "неизвестная область «" + clipUtf8 (name, 40) + "»";
					return failed();
				}
				if (!readIntValue (value, "areas." + name, o.researchAreas[idx], why)) return failed();
			}
			return true;
		}
		case eOrderType::Repair:
		case eOrderType::Rearm:
		case eOrderType::Load:
			if (!readId (j, "targetId", o.unitId2, why)) return failed();
			return true;
		case eOrderType::Transfer:
		{
			if (!readId (j, "targetId", o.unitId2, why)) return failed();
			if (!readInt (j, "value", o.transferValue, why)) return failed();
			const auto res = j.find ("res");
			if (res == j.end() || res->is_null())
			{
				why = "нет обязательного поля res";
				return failed();
			}
			const std::string r = res->is_string() ? res->get<std::string>() : std::string();
			if (r == "metal") o.resType = eResourceType::Metal;
			else if (r == "oil") o.resType = eResourceType::Oil;
			else if (r == "gold") o.resType = eResourceType::Gold;
			else
			{
				why = "поле res: metal, oil или gold";
				return failed();
			}
			return true;
		}
		case eOrderType::ActivateUnit:
			if (!readId (j, "targetId", o.unitId2, why)) return failed();
			if (!readPos (j, "pos", o.position, why)) return failed();
			return true;
		case eOrderType::FinishBuild:
			if (!readPos (j, "pos", o.position, why)) return failed();
			return true;
		case eOrderType::StartWork:
		case eOrderType::StopWork:
		case eOrderType::ToggleSentry:
		case eOrderType::ToggleManualFire:
		case eOrderType::ToggleLayMines:
		case eOrderType::ToggleCollectMines:
			return true;
		default:
			why = "тип приказа не поддержан советником";
			return failed();
	}
}

// --- оценка плана -----------------------------------------------------------------

struct sPlanEval
{
	std::vector<cOrder> accepted;      // явные приказы плана, прошедшие проверку
	std::set<std::string> keys;        // их ключи конфликта
	int rejected = 0;                  // явных приказов отклонено
	sDirectivesResult directives;      // перевод директив
	std::vector<cOrder> directiveOrders; // приказы директив, прошедшие validateOrder
	int directiveOrdersRejected = 0;
	int ordersIgnored = 0;             // orders выключены (MAXR_LLM_ORDERS=0), а пришли
	std::vector<std::string> problems; // для повтора
	bool hasDirectives = false;
	bool hasOrders = false;
};

// разобрать приказы и директивы плана; события — в журнал
template <typename LogFn>
sPlanEval evaluatePlan (const nlohmann::json& plan, int playerId, const cModel& model, const std::vector<cOrder>& ruleOrders,
	const sLlmSwitches& sw, LogFn&& log)
{
	sPlanEval ev;
	const auto& map = model.getMap();
	const cMapView view (map, sharedPlayerOf (model, playerId));

	// --- явные приказы ---
	if (const auto ordersIt = plan.find ("orders"); ordersIt != plan.end() && !ordersIt->is_null()
		&& !(ordersIt->is_array() && ordersIt->empty()) && !sw.orders)
	{
		// orders выключены: в промпте и схеме их нет; пришли всё же — игнорируем
		ev.ordersIgnored = ordersIt->is_array() ? static_cast<int> (ordersIt->size()) : 1;
		log ("orders_ignored", nlohmann::json{{"count", ev.ordersIgnored}, {"input", clipUtf8 (ordersIt->dump (-1, ' ', false, nlohmann::json::error_handler_t::replace), kShortLogBytes)},
			{"reason", "orders выключены (MAXR_LLM_ORDERS=0) — решают правила"}});
	}
	else if (const auto ordersIt = plan.find ("orders"); ordersIt != plan.end() && !ordersIt->is_null())
	{
		if (!ordersIt->is_array())
		{
			ev.problems.push_back ("orders — не массив");
			log ("order_rejected", nlohmann::json{{"index", -1}, {"order", clipUtf8 (ordersIt->dump(), kShortLogBytes)}, {"reason", "orders — не массив"}});
			++ev.rejected;
		}
		else
		{
			const auto& planOrders = *ordersIt;
			ev.hasOrders = !planOrders.empty();
			const std::size_t total = std::min (planOrders.size(), kMaxLlmOrders);
			if (planOrders.size() > kMaxLlmOrders)
				log ("orders_truncated", nlohmann::json{{"given", planOrders.size()}, {"limit", kMaxLlmOrders}});
			for (std::size_t i = 0; i < total; ++i)
			{
				const auto& j = planOrders[i];
				cOrder o;
				std::string reason;
				bool ok = convertOrder (j, playerId, model, view, o, reason);
				if (ok)
				{
					const auto v = validateOrder (model, o);
					if (!v.ok)
					{
						ok = false;
						reason = v.reason;
					}
				}
				const std::string key = ok ? llmConflictKey (o) : std::string();
				if (ok && ev.keys.count (key) != 0)
				{
					ok = false;
					reason = "повтор: в плане уже есть приказ с ключом " + key;
				}
				if (!ok)
				{
					++ev.rejected;
					ev.problems.push_back ("orders[" + std::to_string (i) + "]: " + reason);
					log ("order_rejected", nlohmann::json{{"index", i}, {"order", clipUtf8 (j.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace), kShortLogBytes)}, {"reason", reason}});
					continue;
				}
				ev.keys.insert (key);
				nlohmann::json info = {{"index", i}, {"type", toString (o.type)}, {"unitId", o.unitId}, {"key", key}};
				if (o.type == eOrderType::Move)
				{
					info["to"] = {{"x", o.position.x()}, {"y", o.position.y()}};
					info["pathLen"] = o.path.size();
				}
				log ("order_accepted", std::move (info));
				ev.accepted.push_back (std::move (o));
			}
		}
	}

	// --- директивы ---
	if (const auto dirIt = plan.find ("directives"); dirIt != plan.end() && !dirIt->is_null())
	{
		ev.hasDirectives = !(dirIt->is_object() && dirIt->empty());
		ev.directives = translateLlmDirectives (*dirIt, playerId, model, ruleOrders, ev.keys, sw);
		for (auto& e : ev.directives.events)
			log ("directive", std::move (e));
		ev.directives.events.clear();
		for (const auto& p : ev.directives.problems) ev.problems.push_back (p);
		for (auto& o : ev.directives.orders)
		{
			const auto v = validateOrder (model, o);
			if (!v.ok)
			{
				++ev.directiveOrdersRejected;
				log ("directive_order_rejected", nlohmann::json{{"type", toString (o.type)}, {"unitId", o.unitId}, {"reason", v.reason}, {"orderReason", o.reason}});
				continue;
			}
			ev.directiveOrders.push_back (std::move (o));
		}
		ev.directives.orders.clear();
	}
	return ev;
}

// значение переменной окружения: целое в [lo, hi]
void envInt (const char* name, int lo, int hi, int& out, std::string& note)
{
	const char* v = std::getenv (name);
	if (v == nullptr || *v == '\0') return;
	char* end = nullptr;
	const long n = std::strtol (v, &end, 10);
	if (end != v && *end == '\0' && n >= lo && n <= hi)
		out = static_cast<int> (n);
	else
		note += std::string (note.empty() ? "" : "; ") + name + "='" + v + "' is invalid (" + std::to_string (lo) + ".."
			+ std::to_string (hi) + "), using " + std::to_string (out);
}

// тело ответа из события response llm.jsonl: советник берёт из него только
// content, finish_reason, usage и длину рассуждения
std::string replyBodyFromLog (const nlohmann::json& e)
{
	nlohmann::json message = {{"role", "assistant"}, {"content", e.value ("content", std::string())}};
	if (const long n = e.value ("reasoningChars", 0L); n > 0)
		message["reasoning_content"] = std::string (static_cast<std::size_t> (n), 'r');
	nlohmann::json choice = {{"index", 0}, {"message", std::move (message)}};
	if (const std::string f = e.value ("finishReason", std::string()); !f.empty()) choice["finish_reason"] = f;
	nlohmann::json body = {{"choices", nlohmann::json::array ({std::move (choice)})}};
	if (const auto u = e.find ("usage"); u != e.end() && u->is_object())
	{
		nlohmann::json usage = {{"prompt_tokens", u->value ("prompt", 0)}, {"completion_tokens", u->value ("completion", 0)}};
		if (u->contains ("reasoning")) usage["completion_tokens_details"] = {{"reasoning_tokens", u->value ("reasoning", 0)}};
		body["usage"] = std::move (usage);
	}
	return body.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

//------------------------------------------------------------------------------
std::string llmRequestHash (const std::string& body)
{
	std::uint64_t h = 14695981039346656037ULL;
	for (const unsigned char c : body)
	{
		h ^= c;
		h *= 1099511628211ULL;
	}
	char buf[17];
	std::snprintf (buf, sizeof (buf), "%016llx", static_cast<unsigned long long> (h));
	return buf;
}

//------------------------------------------------------------------------------
bool cLlmReplay::load (const std::string& path, std::string& error)
{
	path_ = path;
	records_.clear();
	configs_.clear();
	game_ = nlohmann::json();
	std::ifstream in (path, std::ios::binary);
	if (!in.is_open())
	{
		error = "cannot open " + path;
		return false;
	}
	// llm.jsonl прежних прогонов: вызов — по событиям request игрока в ходе,
	// его исход — первое следующее событие response / error / *_disabled
	struct sPending
	{
		int call = 0;
		long timeoutMs = 0;
		long long chars = -1;
		bool open = false;
	};
	std::map<std::pair<int, int>, sPending> pending;
	const auto budgetOf = [this] (int player, const char* key, long fallback) {
		const auto it = configs_.find (player);
		return it == configs_.end() ? fallback : it->second.value (key, fallback);
	};
	std::string line;
	while (std::getline (in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const auto e = nlohmann::json::parse (line, nullptr, false);
		if (e.is_discarded() || !e.is_object()) continue; // последняя строка может быть оборвана аварией
		try
		{
			const int player = e.value ("player", -1);
			const int turn = e.value ("turn", -1);
			const std::string event = e.value ("event", std::string());
			if (e.contains ("call")) // llm_replies.jsonl
			{
				sRecord r;
				r.skipped = e.value ("skipped", false);
				r.ok = e.value ("ok", false);
				r.status = e.value ("status", 0);
				r.body = e.value ("body", std::string());
				r.error = e.value ("error", std::string());
				r.ms = e.value ("ms", 0L);
				r.elapsedMs = e.value ("elapsedMs", 0L);
				r.requestHash = e.value ("requestHash", std::string());
				r.requestChars = e.value ("requestChars", -1LL);
				records_[{player, turn, e.value ("call", 0)}] = std::move (r);
				continue;
			}
			if (event == "game")
			{
				game_ = e;
				continue;
			}
			if (event == "config")
			{
				configs_[player] = e;
				continue;
			}
			// --- llm.jsonl ---
			auto& p = pending[{player, turn}];
			if (event == "request")
			{
				++p.call;
				p.timeoutMs = e.value ("timeoutMs", 0L);
				p.chars = e.value ("bodyChars", -1LL);
				p.open = true;
				continue;
			}
			if (event == "retry_skipped")
			{
				sRecord r;
				r.skipped = true;
				r.elapsedMs = budgetOf (player, "turnBudgetMs", 60000) - e.value ("budgetLeftMs", 0L);
				records_[{player, turn, p.call + 1}] = std::move (r);
				continue;
			}
			const std::string stage = e.value ("stage", std::string());
			const bool outcome = event == "response" || event == "schema_disabled" || event == "reasoning_disabled"
				|| (event == "error" && (stage == "http" || stage == "status" || stage == "body" || stage == "content"));
			if (!outcome || !p.open) continue;
			p.open = false;
			sRecord r;
			r.requestChars = p.chars;
			r.ms = e.value ("ms", 0L);
			// сколько бюджета прошло: таймаут запроса меньше настроенного — его урезал бюджет
			const long timeout = budgetOf (player, "timeoutMs", 30000);
			r.elapsedMs = p.timeoutMs > 0 && p.timeoutMs < timeout ? budgetOf (player, "turnBudgetMs", 60000) - p.timeoutMs : 0;
			if (event == "response")
			{
				r.ok = true;
				r.status = e.value ("status", 200);
				r.body = replyBodyFromLog (e);
				r.clipped = e.value ("contentChars", 0LL) != static_cast<long long> (e.value ("content", std::string()).size());
			}
			else if (event == "error" && stage == "http")
				r.error = e.value ("error", std::string());
			else
			{
				r.ok = true;
				r.status = e.value ("status", 200);
				r.body = e.value ("body", std::string());
				r.clipped = r.body.size() > kShortLogBytes; // тело в llm.jsonl обрезано до kShortLogBytes (+ «…»)
			}
			records_[{player, turn, p.call}] = std::move (r);
		}
		catch (const std::exception&)
		{
			continue; // поле не того типа — строку пропускаем
		}
	}
	if (records_.empty())
	{
		error = "no recorded replies in " + path;
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------
const cLlmReplay::sRecord* cLlmReplay::find (int playerId, int turn, int call) const
{
	const auto it = records_.find ({playerId, turn, call});
	return it == records_.end() ? nullptr : &it->second;
}

//------------------------------------------------------------------------------
const nlohmann::json* cLlmReplay::getConfig (int playerId) const
{
	const auto it = configs_.find (playerId);
	return it == configs_.end() ? nullptr : &it->second;
}

//------------------------------------------------------------------------------
sLlmConfig sLlmConfig::fromEnv()
{
	sLlmConfig c;
	if (const char* v = std::getenv ("MAXR_LLM_URL"); v != nullptr && *v != '\0') c.url = v;
	if (const char* v = std::getenv ("MAXR_LLM_MODEL"); v != nullptr && *v != '\0') c.model = v;
	envInt ("MAXR_LLM_TIMEOUT_MS", 1, 3600000, c.timeoutMs, c.note);
	envInt ("MAXR_LLM_TURN_BUDGET_MS", 1, 7200000, c.turnBudgetMs, c.note);
	envInt ("MAXR_LLM_MAX_TOKENS", 0, 1000000, c.maxTokens, c.note);
	envInt ("MAXR_LLM_RETRIES", 0, 3, c.retries, c.note);
	if (const char* v = std::getenv ("MAXR_LLM_SCHEMA"); v != nullptr && *v != '\0')
	{
		const std::string s = v;
		if (s == "0") c.schema = false;
		else if (s == "1") c.schema = true;
		else c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_SCHEMA='" + s + "' is invalid (0|1), using 1";
	}
	if (const char* v = std::getenv ("MAXR_LLM_REASONING"); v != nullptr && *v != '\0')
	{
		const std::string s = lowerAscii (v);
		if (s == "none" || s == "low" || s == "medium" || s == "high" || s == "default") c.reasoning = s;
		else c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_REASONING='" + v + "' is invalid (none|low|medium|high|default), using none";
	}
	// --- выключатели ---
	if (const char* v = std::getenv ("MAXR_LLM_DIRECTIVES"); v != nullptr && *v != '\0')
	{
		sLlmSwitches sw = c.switches;
		sw.production = sw.research = sw.army = false;
		bool ok = true;
		bool none = false;
		std::size_t items = 0;
		const std::string list = lowerAscii (v);
		for (std::size_t pos = 0; pos <= list.size();)
		{
			const std::size_t comma = std::min (list.find (',', pos), list.size());
			std::string item = list.substr (pos, comma - pos);
			item.erase (0, item.find_first_not_of (" \t"));
			item.erase (item.find_last_not_of (" \t") + 1);
			pos = comma + 1;
			if (item.empty()) continue;
			++items;
			if (item == "production") sw.production = true;
			else if (item == "research") sw.research = true;
			else if (item == "army") sw.army = true;
			else if (item == "none") none = true;
			else if (item == "all") sw.production = sw.research = sw.army = true;
			else ok = false;
		}
		if (none && items > 1) ok = false; // «none» — только в одиночку
		if (ok && items > 0) c.switches = sw;
		else c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_DIRECTIVES='" + v
			+ "' is invalid (comma list of production,research,army | all | none), using production,research";
	}
	if (const char* v = std::getenv ("MAXR_LLM_ORDERS"); v != nullptr && *v != '\0')
	{
		const std::string s = v;
		if (s == "0") c.switches.orders = false;
		else if (s == "1") c.switches.orders = true;
		else c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_ORDERS='" + s + "' is invalid (0|1), using 0";
	}
	if (const char* v = std::getenv ("MAXR_LLM_PROD_BASE_SHARE"); v != nullptr && *v != '\0')
	{
		const std::string s = v;
		double d = 0.0;
		const auto [ptr, ec] = std::from_chars (s.data(), s.data() + s.size(), d);
		if (ec == std::errc() && ptr == s.data() + s.size() && d >= 0.0 && d <= 1.0) c.switches.prodBaseShare = d;
		else c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_PROD_BASE_SHARE='" + s + "' is invalid (0..1), using 0.8";
	}
	if (const char* v = std::getenv ("MAXR_LLM_PROD_SHARE_BY"); v != nullptr && *v != '\0')
	{
		const std::string s = v;
		if (s == "metal") c.switches.prodShareByMetal = true;
		else if (s == "count") c.switches.prodShareByMetal = false;
		else c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_PROD_SHARE_BY='" + s + "' is invalid (count|metal), using metal";
	}
	if (c.turnBudgetMs < c.timeoutMs)
		c.note += std::string (c.note.empty() ? "" : "; ") + "MAXR_LLM_TURN_BUDGET_MS < MAXR_LLM_TIMEOUT_MS: a request gets at most the turn budget";
	if (const char* v = std::getenv ("MAXR_LLM_REPLAY"); v != nullptr && *v != '\0') c.replay = v;
	return c;
}

//------------------------------------------------------------------------------
nlohmann::json sLlmConfig::toJson() const
{
	// replay сюда не входит: llm.jsonl повтора совпадает с исходным
	nlohmann::json cfg = {
		{"url", url},
		{"model", model.empty() ? nlohmann::json() : nlohmann::json (model)},
		{"timeoutMs", timeoutMs},
		{"turnBudgetMs", turnBudgetMs},
		{"maxTokens", maxTokens},
		{"schema", schema},
		{"reasoning", reasoning},
		{"retries", retries},
		{"breaker", {{"failures", kBreakerFailures}, {"skipTurns", kBreakerSkipTurns}}},
		{"directives", switches.directivesText()},
		{"orders", switches.orders},
		{"prodBaseShare", switches.prodBaseShare},
		{"baseVehicle", llmBaseVehicleId()},
	};
	cfg["prodShareBy"] = switches.prodShareByMetal ? "metal" : "count";
	return cfg;
}

//------------------------------------------------------------------------------
cLlmAdvisorBrain::cLlmAdvisorBrain (std::shared_ptr<cDecisionJournal> journal, sLlmConfig config,
                                    std::shared_ptr<cDecisionJournal> replies, std::shared_ptr<const cLlmReplay> replay) :
	journal_ (std::move (journal)),
	replies_ (std::move (replies)),
	replay_ (std::move (replay)),
	config_ (std::move (config))
{
	// выключатели — строкой в stdout (один раз на процесс, до начала партии)
	if (!g_switchesPrinted)
	{
		g_switchesPrinted = true;
		const auto& sw = config_.switches;
		char share[16];
		std::snprintf (share, sizeof (share), "%.2f", sw.prodBaseShare);
		std::cout << "[ai_headless] llm: directives=" << sw.directivesText() << " orders=" << (sw.orders ? 1 : 0)
			<< " prodBaseShare=" << share << " prodShareBy=" << (sw.prodShareByMetal ? "metal" : "count")
			<< (sw.anything() ? "" : " (all off: the model is not called, rules play)") << std::endl;
	}
}

//------------------------------------------------------------------------------
void cLlmAdvisorBrain::logEvent (const char* event, int playerId, int turn, nlohmann::json fields)
{
	if (!journal_) return;
	fields["event"] = event;
	fields["turn"] = turn;
	fields["player"] = playerId;
	try
	{
		journal_->log (fields);
	}
	catch (const std::exception&)
	{
		// битый UTF-8 в строке ответа: пишем с заменой символов
		try
		{
			journal_->log (nlohmann::json::parse (fields.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace)));
		}
		catch (...)
		{
		}
	}
}

//------------------------------------------------------------------------------
void cLlmAdvisorBrain::logReply (nlohmann::json entry)
{
	if (!replies_) return;
	try
	{
		replies_->log (entry);
	}
	catch (const std::exception&)
	{
		// битый UTF-8 в теле ответа: пишем с заменой символов (повтор этого ответа может разойтись)
		try
		{
			replies_->log (nlohmann::json::parse (entry.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace)));
		}
		catch (...)
		{
		}
	}
}

//------------------------------------------------------------------------------
std::vector<cOrder> cLlmAdvisorBrain::fallback (int playerId, int turn, const std::string& reason, const std::vector<cOrder>& ruleOrders)
{
	logEvent ("fallback", playerId, turn, {{"reason", reason}, {"ruleOrders", ruleOrders.size()}});
	return ruleOrders;
}

//------------------------------------------------------------------------------
void cLlmAdvisorBrain::transportFailure (int playerId, int turn, const std::string& what)
{
	++transportFailures_;
	if (transportFailures_ < kBreakerFailures) return;
	logEvent ("breaker_open", playerId, turn, {{"failures", transportFailures_}, {"skipTurns", kBreakerSkipTurns}, {"last", what}});
	transportFailures_ = 0;
	skipTurnsLeft_ = kBreakerSkipTurns;
}

//------------------------------------------------------------------------------
std::vector<cOrder> cLlmAdvisorBrain::decideTurn (int playerId, const cModel& model)
{
	// правила считают ход всегда: и для гибрида, и для fallback
	const std::vector<cOrder> ruleOrders = rule_.decideTurn (playerId, model);
	const int turn = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;

	try
	{
		return adviseOrFallback (playerId, turn, model, ruleOrders);
	}
	catch (const std::exception& ex)
	{
		return fallback (playerId, turn, std::string ("исключение: ") + ex.what(), ruleOrders);
	}
	catch (...)
	{
		return fallback (playerId, turn, "неизвестное исключение", ruleOrders);
	}
}

//------------------------------------------------------------------------------
std::vector<cOrder> cLlmAdvisorBrain::adviseOrFallback (int playerId, int turn, const cModel& model, const std::vector<cOrder>& ruleOrders)
{
	if (!configLogged_)
	{
		configLogged_ = true;
		const nlohmann::json cfg = config_.toJson();
		nlohmann::json replyCfg = cfg;
		replyCfg["event"] = "config";
		replyCfg["player"] = playerId;
		replyCfg["turn"] = turn;
		logReply (replyCfg);
		logEvent ("config", playerId, turn, cfg);
		// повтор с другими настройками советника разойдётся с записью
		if (const nlohmann::json* recorded = replay_ ? replay_->getConfig (playerId) : nullptr)
		{
			std::string diff;
			for (const auto& [key, value] : cfg.items())
				if (!recorded->contains (key) || (*recorded)[key] != value)
					diff += (diff.empty() ? "" : ", ") + key + "=" + value.dump() + " (recorded " + (recorded->contains (key) ? (*recorded)[key].dump() : std::string ("none")) + ")";
			if (!diff.empty())
				std::cout << "[ai_headless] llm: REPLAY WARNING player " << playerId << ": advisor settings differ from the recording: " << diff << std::endl;
		}
	}

	// --- всё выключено: модель не зовём, ход — правилами ---
	if (!config_.switches.anything())
	{
		if (!disabledLogged_)
		{
			disabledLogged_ = true;
			logEvent ("disabled", playerId, turn, {{"reason", "MAXR_LLM_DIRECTIVES=none и MAXR_LLM_ORDERS=0: модель не зовётся, все ходы — правилами"},
				{"ruleOrders", ruleOrders.size()}});
		}
		return ruleOrders;
	}
	if (config_.model.empty() && !g_modelWarned)
	{
		g_modelWarned = true;
		logEvent ("warning", playerId, turn, {{"message", "MAXR_LLM_MODEL не задана — поле model в запросе не отправляется"}});
	}

	// --- предохранитель: модель недоступна — несколько ходов не зовём ---
	if (skipTurnsLeft_ > 0)
	{
		--skipTurnsLeft_;
		logEvent ("skipped", playerId, turn, {{"reason", "предохранитель: " + std::to_string (kBreakerFailures) + " транспортных отказа подряд"},
			{"turnsLeft", skipTurnsLeft_}, {"ruleOrders", ruleOrders.size()}});
		return ruleOrders;
	}

	// --- сообщение пользователя: digest + каталог + сводка ---
	nlohmann::json userJson = buildGameStateDigest (model, playerId);
	const std::size_t digestChars = userJson.dump().size();
	const nlohmann::json catalog = buildLlmCatalog (model, playerId);
	const nlohmann::json summary = buildLlmSummary (model, playerId);
	userJson["catalog"] = catalog;
	userJson["summary"] = summary;
	const std::string user = userJson.dump();
	const std::string system = systemPrompt (playerId, config_.switches);
	const nlohmann::ordered_json schema = buildLlmPlanSchema (catalog, config_.switches);

	// ordered_json: порядок полей схемы важен для вывода модели
	nlohmann::ordered_json messages = nlohmann::ordered_json::array ({
		{{"role", "system"}, {"content", system}},
		{{"role", "user"}, {"content", user}},
	});

	using clock = std::chrono::steady_clock;
	const auto turnStart = clock::now();
	int attempt = 1;
	int resends = 0; // повторы того же запроса после отказа сервера от схемы/reasoning (не считаются попыткой)
	int call = 0;    // HTTP-обмен хода (llm_replies.jsonl, повтор)
	for (;;)
	{
		++call;
		// повтор: и ответ, и сколько бюджета хода прошло до вызова — из записи
		const cLlmReplay::sRecord* recorded = replay_ ? replay_->find (playerId, turn, call) : nullptr;
		const long elapsed = replay_ ? (recorded ? recorded->elapsedMs : 0L)
		                             : static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (clock::now() - turnStart).count());
		const long left = config_.turnBudgetMs - elapsed;
		if (attempt > 1 && left < kMinRetryMs)
		{
			logReply ({{"player", playerId}, {"turn", turn}, {"call", call}, {"attempt", attempt}, {"elapsedMs", elapsed}, {"skipped", true}});
			logEvent ("retry_skipped", playerId, turn, {{"attempt", attempt}, {"budgetLeftMs", left}});
			return fallback (playerId, turn, "план непригоден, на повтор не хватило бюджета хода", ruleOrders);
		}
		const int timeoutMs = static_cast<int> (std::max (1L, std::min<long> (config_.timeoutMs, left)));

		// --- запрос ---
		const bool sendSchema = config_.schema && !g_schemaDisabled;
		const bool sendReasoning = config_.reasoning != "default" && !g_reasoningDisabled;
		nlohmann::ordered_json request;
		if (!config_.model.empty()) request["model"] = config_.model;
		request["messages"] = messages;
		request["temperature"] = 0.2;
		request["stream"] = false;
		if (config_.maxTokens > 0) request["max_tokens"] = config_.maxTokens;
		if (sendReasoning) request["reasoning_effort"] = config_.reasoning;
		if (sendSchema)
			request["response_format"] = {{"type", "json_schema"},
				{"json_schema", {{"name", "maxr_plan"}, {"strict", true}, {"schema", schema}}}};
		const std::string body = request.dump();

		logEvent ("request", playerId, turn, {
			{"attempt", attempt},
			{"url", config_.url},
			{"model", config_.model.empty() ? nlohmann::json() : nlohmann::json (config_.model)},
			{"timeoutMs", timeoutMs},
			{"bodyChars", body.size()},
			{"systemChars", system.size()},
			{"digestChars", digestChars},
			{"userChars", user.size()},
			{"catalog", catalog.size()},
			{"messages", messages.size()},
			{"schema", sendSchema},
			{"reasoning", sendReasoning ? nlohmann::json (config_.reasoning) : nlohmann::json()},
			{"maxTokens", config_.maxTokens > 0 ? nlohmann::json (config_.maxTokens) : nlohmann::json()},
		});

		const std::string requestHash = llmRequestHash (body);
		sHttpResult http;
		if (replay_)
		{
			const auto warnOnce = [this, playerId, turn, call] (const std::string& what) {
				if (replayWarned_) return;
				replayWarned_ = true;
				std::cout << "[ai_headless] llm: REPLAY WARNING player " << playerId << " turn " << turn << " call " << call << ": " << what << std::endl;
			};
			if (recorded == nullptr || recorded->skipped)
			{
				// ход и вызов — в полях события: причина одна на все такие ходы (сводка их группирует)
				warnOnce ("no recorded reply, the rule AI plays this turn");
				logEvent ("error", playerId, turn, {{"stage", "replay"}, {"attempt", attempt}, {"call", call}, {"error", "в записи нет ответа"}});
				return fallback (playerId, turn, "повтор: в записи нет ответа", ruleOrders);
			}
			// запрос не тот, что в записи: состояние игры разошлось (или другие настройки)
			const bool differs = !recorded->requestHash.empty() ? recorded->requestHash != requestHash
			                                                    : recorded->requestChars >= 0 && recorded->requestChars != static_cast<long long> (body.size());
			if (differs)
			{
				warnOnce ("the request differs from the recording (game state or settings diverged)");
				logEvent ("replay_mismatch", playerId, turn, {{"attempt", attempt}, {"call", call}, {"requestChars", body.size()}, {"requestHash", requestHash},
					{"recordedChars", recorded->requestChars}, {"recordedHash", recorded->requestHash}});
			}
			if (recorded->clipped)
			{
				warnOnce ("the recorded reply is clipped (old llm.jsonl), the replay diverges here");
				logEvent ("replay_clipped", playerId, turn, {{"attempt", attempt}, {"call", call}});
			}
			http.ok = recorded->ok;
			http.status = recorded->status;
			http.body = recorded->body;
			http.error = recorded->error;
			http.elapsedMs = recorded->ms;
		}
		else
			http = httpPostJson (config_.url, body, timeoutMs);
		// полный ответ сервера — для повтора партии (MAXR_LLM_REPLAY)
		logReply ({{"player", playerId}, {"turn", turn}, {"call", call}, {"attempt", attempt}, {"elapsedMs", elapsed}, {"timeoutMs", timeoutMs},
			{"requestChars", body.size()}, {"requestHash", requestHash}, {"ok", http.ok}, {"status", http.status}, {"ms", http.elapsedMs},
			{"error", http.error}, {"body", http.body}});
		if (!http.ok)
		{
			logEvent ("error", playerId, turn, {{"stage", "http"}, {"attempt", attempt}, {"error", http.error}, {"ms", http.elapsedMs}});
			transportFailure (playerId, turn, http.error);
			return fallback (playerId, turn, "http: " + http.error, ruleOrders);
		}
		if (http.status == 400 && resends < 2)
		{
			const std::string lower = lowerAscii (http.body);
			if (sendSchema && (lower.find ("response_format") != std::string::npos || lower.find ("json_schema") != std::string::npos))
			{
				g_schemaDisabled = true;
				++resends;
				logEvent ("schema_disabled", playerId, turn, {{"status", http.status}, {"ms", http.elapsedMs}, {"body", clipUtf8 (http.body, kShortLogBytes)}});
				continue;
			}
			if (sendReasoning && lower.find ("reasoning") != std::string::npos)
			{
				g_reasoningDisabled = true;
				++resends;
				logEvent ("reasoning_disabled", playerId, turn, {{"status", http.status}, {"ms", http.elapsedMs}, {"body", clipUtf8 (http.body, kShortLogBytes)}});
				continue;
			}
		}
		if (http.status != 200)
		{
			transportFailures_ = 0; // сервер ответил (ошибка не транспортная)
			logEvent ("error", playerId, turn, {{"stage", "status"}, {"attempt", attempt}, {"status", http.status}, {"ms", http.elapsedMs},
				{"body", clipUtf8 (http.body, kShortLogBytes)}});
			return fallback (playerId, turn, "HTTP-статус " + std::to_string (http.status), ruleOrders);
		}

		// --- ответ: choices[0].message.content (+ reasoning_content, finish_reason) ---
		const auto reply = nlohmann::json::parse (http.body, nullptr, false);
		if (reply.is_discarded() || !reply.is_object())
		{
			logEvent ("error", playerId, turn, {{"stage", "body"}, {"attempt", attempt}, {"error", "тело ответа — не JSON-объект"},
				{"ms", http.elapsedMs}, {"body", clipUtf8 (http.body, kShortLogBytes)}});
			return fallback (playerId, turn, "тело ответа — не JSON", ruleOrders);
		}
		const nlohmann::json* msg = nullptr;
		std::string finishReason;
		if (const auto ch = reply.find ("choices"); ch != reply.end() && ch->is_array() && !ch->empty() && ch->front().is_object())
		{
			const auto& first = ch->front();
			if (const auto m = first.find ("message"); m != first.end() && m->is_object()) msg = &*m;
			if (const auto f = first.find ("finish_reason"); f != first.end() && f->is_string()) finishReason = f->get<std::string>();
		}
		if (msg == nullptr)
		{
			logEvent ("error", playerId, turn, {{"stage", "content"}, {"attempt", attempt}, {"error", "нет choices[0].message"},
				{"ms", http.elapsedMs}, {"body", clipUtf8 (http.body, kShortLogBytes)}});
			return fallback (playerId, turn, "нет choices[0].message", ruleOrders);
		}
		std::string text;
		if (const auto c = msg->find ("content"); c != msg->end() && c->is_string()) text = c->get<std::string>();
		std::size_t reasoningChars = 0;
		for (const char* k : {"reasoning_content", "reasoning"})
			if (const auto r = msg->find (k); r != msg->end() && r->is_string()) reasoningChars = std::max (reasoningChars, r->get_ref<const std::string&>().size());
		nlohmann::json usage;
		if (const auto u = reply.find ("usage"); u != reply.end() && u->is_object())
		{
			usage["prompt"] = u->value ("prompt_tokens", 0);
			usage["completion"] = u->value ("completion_tokens", 0);
			if (const auto d = u->find ("completion_tokens_details"); d != u->end() && d->is_object())
				usage["reasoning"] = d->value ("reasoning_tokens", 0);
		}

		// --- план ---
		std::string planText;
		std::string why;
		nlohmann::json plan;
		bool thinkingTooLong = false;
		if (text.find_first_not_of (" \t\r\n") == std::string::npos)
		{
			// рассуждение — черновик (варианты, «Wait…»), план из него не берём
			thinkingTooLong = finishReason == "length";
			why = thinkingTooLong ? "content пуст: max_tokens кончились на рассуждении (" + std::to_string (reasoningChars) + " символов)"
			                      : "content пуст";
		}
		else if (extractPlanText (text, planText, why))
		{
			plan = nlohmann::json::parse (planText, nullptr, false);
			if (plan.is_discarded()) why = finishReason == "length" ? "битый JSON плана (обрезан по max_tokens)" : "битый JSON плана";
			else if (!plan.is_object()) why = "план — не JSON-объект";
		}
		std::string strategy;
		if (why.empty())
			if (const auto s = plan.find ("strategy"); s != plan.end() && s->is_string()) strategy = clipUtf8 (s->get<std::string>(), kShortLogBytes);

		logEvent ("response", playerId, turn, {
			{"attempt", attempt},
			{"status", http.status},
			{"ms", http.elapsedMs},
			{"finishReason", finishReason},
			{"contentChars", text.size()},
			{"reasoningChars", reasoningChars},
			{"usage", usage},
			{"content", clipUtf8 (text, kContentLogBytes)},
			{"strategy", strategy},
		});

		if (thinkingTooLong)
		{
			// как таймаут: модель думает слишком долго — повтор бесполезен
			logEvent ("error", playerId, turn, {{"stage", "plan"}, {"attempt", attempt}, {"error", why}});
			transportFailure (playerId, turn, why);
			return fallback (playerId, turn, why, ruleOrders);
		}
		transportFailures_ = 0; // модель ответила текстом

		std::vector<std::string> problems;
		if (why.empty())
		{
			auto log = [this, playerId, turn, attempt] (const char* event, nlohmann::json fields) {
				fields["attempt"] = attempt;
				logEvent (event, playerId, turn, std::move (fields));
			};
			sPlanEval ev = evaluatePlan (plan, playerId, model, ruleOrders, config_.switches, log);
			const bool usable = !ev.accepted.empty() || ev.directives.accepted > 0;
			if (usable)
			{
				// --- слияние: приказы правил с ключом из плана заменяются ---
				std::set<std::string> keys = ev.keys;
				for (const auto& o : ev.directiveOrders) keys.insert (llmConflictKey (o));
				std::vector<cOrder> result;
				result.reserve (ruleOrders.size() + ev.accepted.size() + ev.directiveOrders.size());
				nlohmann::json replacedKeys = nlohmann::json::array();
				for (const auto& o : ruleOrders)
				{
					const std::string key = llmConflictKey (o);
					if (keys.count (key) != 0)
					{
						replacedKeys.push_back (key + " " + toString (o.type) + (o.reason.empty() ? "" : " (" + o.reason + ")"));
						continue;
					}
					result.push_back (o);
				}
				const std::size_t ruleKept = result.size();
				result.insert (result.end(), ev.accepted.begin(), ev.accepted.end());
				result.insert (result.end(), ev.directiveOrders.begin(), ev.directiveOrders.end());

				logEvent ("merge", playerId, turn, {
					{"attempt", attempt},
					{"llmAccepted", ev.accepted.size()},
					{"llmRejected", ev.rejected},
					{"directivesAccepted", ev.directives.accepted},
					{"directivesRejected", ev.directives.rejected},
					{"directivesIgnored", ev.directives.ignored},
					{"ordersIgnored", ev.ordersIgnored},
					{"directiveOrders", ev.directiveOrders.size()},
					{"directiveOrdersRejected", ev.directiveOrdersRejected},
					{"ruleOrders", ruleOrders.size()},
					{"ruleReplaced", replacedKeys.size()},
					{"ruleKept", ruleKept},
					{"total", result.size()},
					{"replaced", std::move (replacedKeys)},
				});
				return result;
			}
			problems = ev.problems;
			if (!ev.hasDirectives && !ev.hasOrders && ev.rejected == 0 && ev.directives.rejected == 0 && ev.ordersIgnored == 0)
				why = "план пуст: нет ни директив, ни приказов";
			else
				why = "ни директивы, ни приказы плана не приняты (приказов отклонено " + std::to_string (ev.rejected)
					+ ", директив отклонено " + std::to_string (ev.directives.rejected)
					+ (ev.directives.ignored + ev.ordersIgnored > 0 ? ", выключенных проигнорировано " + std::to_string (ev.directives.ignored + ev.ordersIgnored) : std::string())
					+ ")";
		}

		// --- план непригоден: повтор с текстом ошибки или fallback ---
		logEvent ("error", playerId, turn, {{"stage", "plan"}, {"attempt", attempt}, {"error", why}});
		if (attempt > config_.retries)
			return fallback (playerId, turn, why + (attempt > 1 ? " (после повтора)" : ""), ruleOrders);
		messages.push_back ({{"role", "assistant"}, {"content", clipUtf8 (text.empty() ? std::string ("(empty)") : text, kRetryEchoBytes)}});
		messages.push_back ({{"role", "user"}, {"content", retryMessage (why, problems, config_.switches)}});
		logEvent ("retry", playerId, turn, {{"attempt", attempt + 1}, {"reason", why}, {"problems", problems.size()}});
		++attempt;
	}
}

} // namespace ai
