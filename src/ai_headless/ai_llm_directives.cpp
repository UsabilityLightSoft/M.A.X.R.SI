/***************************************************************************
 * ai_llm_directives.cpp — каталог, сводка, схема плана v2 и перевод
 * директив LLM в приказы (этап 5, шаг 2); выключатели директив и доля
 * базовой машины в production.
 ***************************************************************************/

#include "ai_llm_directives.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/id.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"
#include "game/logic/upgradecalculator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <forward_list>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace ai
{

namespace
{

// имена областей исследований — как в digest; индекс = cResearch::eResearchArea
constexpr const char* kAreaNames[] = {"attack", "shots", "range", "armor", "hitpoints", "speed", "scan", "cost"};
static_assert (std::size (kAreaNames) == static_cast<std::size_t> (cResearch::kNrResearchAreas), "области исследований разошлись с cResearch");

constexpr const char* kArmyModes[] = {"attack", "gather", "defend", "explore"};

constexpr int kMaxWeight = 10;          // вес директивы: целое 0..10
constexpr long long kMaxQueue = 10;     // длиннее очередь фабрики не делаем
constexpr int kAttackRadius = 1;        // attack: ближе к цели — уже на месте
constexpr int kGatherRadius = 3;        // gather/defend: ближе к точке — уже на месте
constexpr int kSnapRadius = 6;          // поиск свободной клетки вокруг цели
constexpr int kHealthyPct = 70;         // юнит без приказа правил и с hp ниже — правилам (отход)
constexpr std::size_t kInputLogBytes = 300;
constexpr std::size_t kMaxSummaryEnemies = 8;
constexpr int kMaxOrdersSchema = 5;     // явных приказов в плане (схема)

const sID kBigFactory (1, 11); // первая постройка правил: что строила бы фабрика
// базовая машина правил: «produce tanks» (kIdTank в ai_rule_brain.cpp — держать в согласии);
// если правила этим ходом заказали фабрике очередь, берём её тип оттуда
const sID kBaseVehicle (0, 32);

// обрезать до maxBytes, не разрывая символ UTF-8
std::string clipUtf8 (const std::string& s, std::size_t maxBytes)
{
	if (s.size() <= maxBytes) return s;
	std::size_t cut = maxBytes;
	while (cut > 0 && (static_cast<unsigned char> (s[cut]) & 0xC0) == 0x80) --cut;
	return s.substr (0, cut) + "…";
}

std::string dumpClip (const nlohmann::json& j)
{
	return clipUtf8 (j.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace), kInputLogBytes);
}

std::string typeText (const sID& id)
{
	return std::to_string (id.firstPart) + "." + std::to_string (id.secondPart);
}

nlohmann::json posJson (const cPosition& p)
{
	return nlohmann::json{{"x", p.x()}, {"y", p.y()}};
}

std::shared_ptr<const cPlayer> sharedPlayer (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id)
			return p;
	return nullptr;
}

// Копии isIdle / isCombatUnit из ai_rule_brain.cpp: там они во внутреннем
// пространстве имён, а правила не меняем. Держать в согласии с оригиналом.
bool vehicleIdle (const cVehicle& v)
{
	if (v.isDisabled()) return false;
	if (v.isUnitLoaded()) return false;
	if (v.isUnitMoving() || v.getMoveJob() != nullptr) return false;
	if (v.isUnitBuildingABuilding() || v.isUnitClearing()
		|| v.isUnitLayingMines() || v.isUnitClearingMines()) return false;
	return true;
}

bool combatUnit (const cVehicle& v)
{
	const auto& sd = v.getStaticUnitData();
	const auto& dd = v.data;
	return sd.canAttack > 0 && dd.getRange() > 0 && dd.getShots() > 0;
}

// свои фабрики: здания, которые что-то строят (canBuild не пуст)
std::vector<const cBuilding*> ownFactories (const cPlayer& player)
{
	std::vector<const cBuilding*> res;
	for (const auto& b : player.getBuildings())
		if (!b->getStaticUnitData().canBuild.empty() && !b->isRubble())
			res.push_back (b.get());
	return res;
}

// классы постройки (canBuild) наших фабрик; без фабрик — класс 1.11
std::vector<std::string> factoryClasses (const cModel& model, const cPlayer& player)
{
	std::vector<std::string> classes;
	for (const cBuilding* b : ownFactories (player))
	{
		const std::string& c = b->getStaticUnitData().canBuild;
		if (std::find (classes.begin(), classes.end(), c) == classes.end()) classes.push_back (c);
	}
	const auto& unitsData = model.getUnitsData();
	if (classes.empty() && unitsData && unitsData->isValidId (kBigFactory))
		classes.push_back (unitsData->getStaticUnitData (kBigFactory).canBuild);
	return classes;
}

// типы каталога (техника, buildAs которой — один из классов наших фабрик)
std::vector<const cStaticUnitData*> catalogTypes (const cModel& model, const cPlayer& player)
{
	std::vector<const cStaticUnitData*> res;
	const auto& unitsData = model.getUnitsData();
	if (!unitsData) return res;
	const auto classes = factoryClasses (model, player);
	for (const auto& sd : unitsData->getStaticUnitsData())
	{
		if (!sd.ID.isAVehicle()) continue;
		if (std::find (classes.begin(), classes.end(), sd.buildAs) == classes.end()) continue;
		if (player.getLastUnitData (sd.ID) == nullptr) continue;
		res.push_back (&sd);
	}
	return res;
}

// индекс приказов правил по ключу конфликта (первый приказ с ключом)
std::map<std::string, const cOrder*> indexRuleOrders (const std::vector<cOrder>& ruleOrders)
{
	std::map<std::string, const cOrder*> idx;
	for (const auto& o : ruleOrders)
		idx.emplace (llmConflictKey (o), &o);
	return idx;
}

// целый вес 0..kMaxWeight
bool readWeight (const nlohmann::json& v, int& out, std::string& why)
{
	if (!v.is_number_integer())
	{
		why = "вес — целое число 0.." + std::to_string (kMaxWeight);
		return false;
	}
	const long long w = v.is_number_unsigned() ? static_cast<long long> (std::min<unsigned long long> (v.get<unsigned long long>(), 1000000ULL))
	                                           : v.get<long long>();
	if (w < 0 || w > kMaxWeight)
	{
		why = "вес " + std::to_string (w) + " вне 0.." + std::to_string (kMaxWeight);
		return false;
	}
	out = static_cast<int> (w);
	return true;
}

// метод наибольших остатков: раздать total единиц по весам w (сумма > 0);
// при равных остатках — больший вес, затем меньший индекс
std::vector<int> largestRemainder (const std::vector<int>& w, int total)
{
	const long long sum = std::accumulate (w.begin(), w.end(), 0LL);
	std::vector<int> res (w.size(), 0);
	if (sum <= 0 || total <= 0) return res;
	std::vector<long long> rem (w.size(), 0);
	long long given = 0;
	for (std::size_t i = 0; i < w.size(); ++i)
	{
		const long long q = static_cast<long long> (w[i]) * total;
		res[i] = static_cast<int> (q / sum);
		rem[i] = q % sum;
		given += res[i];
	}
	std::vector<std::size_t> order (w.size());
	std::iota (order.begin(), order.end(), 0);
	std::stable_sort (order.begin(), order.end(), [&] (std::size_t a, std::size_t b) {
		if (rem[a] != rem[b]) return rem[a] > rem[b];
		return w[a] > w[b];
	});
	for (std::size_t k = 0; given < total && k < order.size(); ++k, ++given)
		++res[order[k]];
	return res;
}

// взвешенная очередь: сократить веса (НОД, не длиннее kMaxQueue) и
// перемешать плавным взвешенным round-robin (3:1 → A A B A, а не A A A B)
std::vector<sID> weightedQueue (const std::vector<std::pair<sID, int>>& weights)
{
	std::vector<int> w;
	for (const auto& [id, n] : weights) w.push_back (n);
	int g = 0;
	for (int n : w) g = std::gcd (g, n);
	if (g > 1)
		for (int& n : w) n /= g;
	long long sum = std::accumulate (w.begin(), w.end(), 0LL);
	if (sum > kMaxQueue)
	{
		w = largestRemainder (w, static_cast<int> (kMaxQueue));
		sum = std::accumulate (w.begin(), w.end(), 0LL);
	}
	std::vector<sID> queue;
	std::vector<long long> current (w.size(), 0);
	for (long long k = 0; k < sum; ++k)
	{
		std::size_t best = 0;
		for (std::size_t i = 0; i < w.size(); ++i)
		{
			current[i] += w[i];
			if (current[i] > current[best]) best = i;
		}
		current[best] -= sum;
		queue.push_back (weights[best].first);
	}
	return queue;
}

// Доля базовой машины: base получает не меньше share очереди, веса остальных
// типов модели масштабируются в остаток. Модель сама дала base долю ≥ share —
// веса без изменений. Иначе — kMaxQueue мест: base ⌈share·10⌉, прочим — остаток
// методом наибольших остатков по их весам (тип с 0 мест выпадает). base — первым:
// при равенстве round-robin очередь начинается с него.
// Пример (share 0.5): {0.7:3, 0.21:2} → base 5, 0.7 3, 0.21 2 → очередь из 10;
// {0.32:1, 0.7:3} (у base 25%) → base 5, 0.7 5 → НОД → [0.32, 0.7].
// costOf != nullptr (MAXR_LLM_PROD_SHARE_BY=metal) — доля по металлу: вес·цена; base получает
// наименьшее число мест из kMaxQueue, при котором её доля металла очереди ≥ share
// (прочим — остаток по их весам, как выше). costOf == nullptr — прежняя доля по числу машин.
std::vector<std::pair<sID, int>> applyBaseShare (const std::vector<std::pair<sID, int>>& w, const sID& base, double share, nlohmann::json& info,
	const std::function<int (const sID&)>* costOf = nullptr)
{
	auto cost = [&] (const sID& id) -> long long { return costOf != nullptr ? std::max (1, (*costOf) (id)) : 1; };
	long long total = 0;
	long long baseW = 0;
	std::vector<std::pair<sID, int>> others;
	for (const auto& p : w)
	{
		total += p.second * cost (p.first);
		if (p.first == base) baseW += p.second * cost (p.first);
		else others.push_back (p);
	}
	const double modelShare = total > 0 ? static_cast<double> (baseW) / static_cast<double> (total) : 0.0;
	info = {{"type", typeText (base)}, {"modelShare", std::round (modelShare * 1000.0) / 1000.0}, {"minShare", share}};
	if (costOf != nullptr) info["by"] = "metal";
	if (total > 0 && modelShare + 1e-9 >= share)
	{
		info["adjusted"] = false;
		return w;
	}
	info["adjusted"] = true;
	std::vector<int> ow;
	for (const auto& p : others) ow.push_back (p.second);
	const int maxQ = static_cast<int> (kMaxQueue);
	if (costOf == nullptr)
	{
		const int baseSlots = std::clamp (static_cast<int> (std::ceil (share * static_cast<double> (kMaxQueue) - 1e-9)), 1, maxQ);
		std::vector<std::pair<sID, int>> res = {{base, baseSlots}};
		if (baseSlots < maxQ && !others.empty())
		{
			const std::vector<int> slots = largestRemainder (ow, maxQ - baseSlots);
			for (std::size_t i = 0; i < others.size(); ++i)
				if (slots[i] > 0) res.emplace_back (others[i].first, slots[i]);
		}
		return res;
	}
	std::vector<std::pair<sID, int>> res;
	for (int k = 1; k <= maxQ; ++k)
	{
		res = {{base, k}};
		long long met = k * cost (base);
		const long long baseMet = met;
		if (k < maxQ && !others.empty())
		{
			const std::vector<int> slots = largestRemainder (ow, maxQ - k);
			for (std::size_t i = 0; i < others.size(); ++i)
				if (slots[i] > 0)
				{
					res.emplace_back (others[i].first, slots[i]);
					met += slots[i] * cost (others[i].first);
				}
		}
		if (static_cast<double> (baseMet) + 1e-9 >= share * static_cast<double> (met)) break;
	}
	return res;
}

bool rotationEqual (const std::vector<sID>& a, const std::vector<sID>& b)
{
	if (a.size() != b.size()) return false;
	if (a.empty()) return true;
	for (std::size_t s = 0; s < b.size(); ++s)
	{
		bool same = true;
		for (std::size_t i = 0; i < a.size() && same; ++i)
			same = a[i] == b[(i + s) % b.size()];
		if (same) return true;
	}
	return false;
}

nlohmann::json queueJson (const std::vector<sID>& q)
{
	nlohmann::json j = nlohmann::json::array();
	for (const auto& id : q) j.push_back (typeText (id));
	return j;
}

std::vector<sTypeId> toTypeIds (const std::vector<sID>& q)
{
	std::vector<sTypeId> res;
	for (const auto& id : q) res.emplace_back (id.firstPart, id.secondPart);
	return res;
}

nlohmann::json directiveEvent (const char* kind, const nlohmann::json& input)
{
	return nlohmann::json{{"kind", kind}, {"input", dumpClip (input)}};
}

void finishRejected (sDirectivesResult& res, nlohmann::json ev, const std::string& reason)
{
	ev["status"] = "rejected";
	ev["reason"] = reason;
	ev["orders"] = 0;
	res.problems.push_back (ev["kind"].get<std::string>() + ": " + reason);
	++res.rejected;
	res.events.push_back (std::move (ev));
}

// --- production -----------------------------------------------------------------

void translateProduction (const nlohmann::json& v, const cPlayer& player, const cModel& model,
	const std::map<std::string, const cOrder*>& ruleIdx, const std::set<std::string>& takenKeys, double baseShare, bool shareByMetal, sDirectivesResult& res)
{
	nlohmann::json ev = directiveEvent ("production", v);
	if (!v.is_object())
		return finishRejected (res, std::move (ev), "нужен объект {\"<id из каталога>\": вес}");

	const auto catalog = catalogTypes (model, player);
	std::vector<std::pair<sID, int>> weights; // порядок — как в плане (ключи JSON отсортированы)
	nlohmann::json problems = nlohmann::json::array();
	for (const auto& [key, value] : v.items())
	{
		sTypeId t;
		if (!llmParseTypeId (key, t))
		{
			problems.push_back ("«" + clipUtf8 (key, 40) + "» — не id «a.b»");
			continue;
		}
		const sID id (t.firstPart, t.secondPart);
		const auto it = std::find_if (catalog.begin(), catalog.end(), [&] (const cStaticUnitData* sd) { return sd->ID == id; });
		if (it == catalog.end())
		{
			problems.push_back ("тип " + typeText (id) + " не в каталоге — наши фабрики его не строят");
			continue;
		}
		int w = 0;
		std::string why;
		if (!readWeight (value, w, why))
		{
			problems.push_back (typeText (id) + ": " + why);
			continue;
		}
		if (w == 0) continue;
		auto same = std::find_if (weights.begin(), weights.end(), [&] (const auto& p) { return p.first == id; });
		if (same != weights.end()) same->second = std::min (kMaxWeight, same->second + w); // «0.32» и «00 32»
		else weights.emplace_back (id, w);
	}
	if (!problems.empty())
	{
		ev["problems"] = problems;
		for (const auto& p : problems) res.problems.push_back ("production: " + p.get<std::string>());
	}
	if (weights.empty())
	{
		if (!problems.empty()) return finishRejected (res, std::move (ev), "ни одного годного типа");
		ev["status"] = "accepted";
		ev["orders"] = 0;
		ev["note"] = "пусто или все веса 0 — очереди фабрик остаются правилам";
		++res.accepted;
		res.events.push_back (std::move (ev));
		return;
	}

	const auto& unitsData = model.getUnitsData();
	const sID constructorId = unitsData->getConstructorID();
	nlohmann::json details = nlohmann::json::array();
	int emitted = 0;
	for (const cBuilding* b : ownFactories (player))
	{
		const std::string key = "buildlist:" + std::to_string (b->getId());
		nlohmann::json d = {{"factory", b->getId()}};
		auto skip = [&] (const std::string& why) {
			d["action"] = "skip";
			d["why"] = why;
			details.push_back (d);
		};

		// типы плана, которые умеет эта фабрика
		std::vector<std::pair<sID, int>> subset;
		for (const auto& p : weights)
			if (unitsData->getStaticUnitData (p.first).buildAs == b->getStaticUnitData().canBuild) subset.push_back (p);
		if (subset.empty())
		{
			skip ("фабрика не строит ни один тип из production");
			continue;
		}
		if (b->isDisabled())
		{
			skip ("фабрика выведена из строя");
			continue;
		}
		if (takenKeys.count (key) != 0)
		{
			skip ("есть явный приказ плана");
			continue;
		}
		const auto ruleIt = ruleIdx.find (key);
		if (ruleIt != ruleIdx.end() && ruleIt->second->reason == "produce constructor")
		{
			skip ("правила заказали конструктора (экономический минимум)");
			continue;
		}

		// доля базовой машины правил: та, что правила ставят этой фабрике, иначе kBaseVehicle
		if (baseShare > 0.0)
		{
			sID base = kBaseVehicle;
			if (ruleIt != ruleIdx.end() && ruleIt->second->type == eOrderType::ChangeBuildList && !ruleIt->second->buildList.empty())
				base = sID (ruleIt->second->buildList.front().firstPart, ruleIt->second->buildList.front().secondPart);
			const bool inCatalog = std::find_if (catalog.begin(), catalog.end(), [&] (const cStaticUnitData* sd) { return sd->ID == base; }) != catalog.end();
			if (inCatalog && unitsData->getStaticUnitData (base).buildAs == b->getStaticUnitData().canBuild)
			{
				nlohmann::json info;
				const std::function<int (const sID&)> costOf = [&] (const sID& id) {
					const auto* dd = player.getLastUnitData (id);
					return dd != nullptr ? dd->getBuildCost() : 0;
				};
				subset = applyBaseShare (subset, base, baseShare, info, shareByMetal ? &costOf : nullptr);
				d["baseShare"] = std::move (info);
			}
			else
				d["baseShare"] = {{"type", typeText (base)}, {"adjusted", false}, {"why", "фабрика не строит базовую машину"}};
		}

		const std::vector<sID> desired = weightedQueue (subset);
		std::vector<sID> current;
		for (std::size_t i = 0; i < b->getBuildListSize(); ++i) current.push_back (b->getBuildListItem (i).getType());
		const bool repeat = b->getRepeatBuild();
		int speed = current.empty() ? 1 : b->getBuildSpeed();
		std::vector<sID> list;
		std::string how;
		if (current.empty())
		{
			list = desired;
			how = "очередь была пуста";
		}
		else
		{
			const auto& item0 = b->getBuildListItem (0);
			const sID first = item0.getType();
			if (item0.getRemainingMetal() == 0)
			{
				skip ("готовый юнит выезжает в этом ходу — очередь не трогаем");
				continue;
			}
			if (repeat && rotationEqual (current, desired))
			{
				d["action"] = "keep";
				d["why"] = "очередь уже такая";
				details.push_back (d);
				continue;
			}
			const auto pos = std::find (desired.begin(), desired.end(), first);
			if (pos != desired.end())
			{
				// тот же первый тип — сервер сохранит прогресс текущей позиции
				list.assign (pos, desired.end());
				list.insert (list.end(), desired.begin(), pos);
				how = "первый тип тот же — прогресс сохранён";
			}
			else if (first == constructorId && !repeat)
			{
				skip ("строится конструктор правил — ждём выезда");
				continue;
			}
			else
			{
				std::array<int, 3> rounds{};
				std::array<int, 3> costs{};
				const auto* dd = player.getLastUnitData (first);
				if (speed < 0 || speed > 2) speed = 1;
				if (dd != nullptr) b->calcTurboBuild (rounds, costs, dd->getBuildCost());
				if (dd != nullptr && item0.getRemainingMetal() >= costs[speed])
				{
					list = desired;
					how = "в текущую позицию металл не вложен — заменяем сразу";
				}
				else
				{
					// сначала достроить начатое: первый тип не меняется — металл не теряется
					list.push_back (first);
					list.insert (list.end(), desired.begin(), desired.end());
					how = "сначала достроить " + typeText (first) + " (металл вложен)";
				}
			}
			if (repeat && list == current && b->getBuildSpeed() == speed)
			{
				d["action"] = "keep";
				d["why"] = "очередь уже такая";
				details.push_back (d);
				continue;
			}
		}

		cOrder o;
		o.type = eOrderType::ChangeBuildList;
		o.playerId = player.getId();
		o.unitId = b->getId();
		o.buildList = toTypeIds (list);
		o.repeat = true;
		o.buildSpeed = speed;
		o.level = "llm";
		std::string q;
		for (const auto& id : list) q += (q.empty() ? "" : ",") + typeText (id);
		o.reason = "llm-dir production: " + q;
		res.orders.push_back (std::move (o));
		++emitted;
		d["action"] = "set";
		d["how"] = how;
		d["from"] = queueJson (current);
		d["to"] = queueJson (list);
		details.push_back (d);
	}

	nlohmann::json want = nlohmann::json::object();
	for (const auto& [id, w] : weights) want[typeText (id)] = w;
	ev["status"] = "accepted";
	ev["weights"] = want;
	ev["prodBaseShare"] = baseShare;
	if (shareByMetal) ev["prodShareBy"] = "metal";
	ev["orders"] = emitted;
	ev["factories"] = details;
	if (details.empty()) ev["note"] = "своих фабрик нет — директива ждёт первой фабрики";
	++res.accepted;
	res.events.push_back (std::move (ev));
}

// --- research -------------------------------------------------------------------

void translateResearch (const nlohmann::json& v, const cPlayer& player, const std::set<std::string>& takenKeys, sDirectivesResult& res)
{
	nlohmann::json ev = directiveEvent ("research", v);
	if (!v.is_object())
		return finishRejected (res, std::move (ev), "нужен объект {область: вес}");

	std::vector<int> w (std::size (kAreaNames), 0);
	nlohmann::json problems = nlohmann::json::array();
	for (const auto& [name, value] : v.items())
	{
		std::size_t idx = std::size (kAreaNames);
		for (std::size_t i = 0; i < std::size (kAreaNames); ++i)
			if (name == kAreaNames[i]) idx = i;
		if (idx == std::size (kAreaNames))
		{
			problems.push_back ("неизвестная область «" + clipUtf8 (name, 40) + "»");
			continue;
		}
		std::string why;
		int n = 0;
		if (!readWeight (value, n, why))
		{
			problems.push_back (name + ": " + why);
			continue;
		}
		w[idx] = n;
	}
	if (!problems.empty())
	{
		ev["problems"] = problems;
		for (const auto& p : problems) res.problems.push_back ("research: " + p.get<std::string>());
	}
	const int sum = std::accumulate (w.begin(), w.end(), 0);
	auto noop = [&] (const std::string& note) {
		ev["status"] = "accepted";
		ev["orders"] = 0;
		ev["note"] = note;
		++res.accepted;
		res.events.push_back (std::move (ev));
	};
	if (sum == 0)
	{
		if (!problems.empty()) return finishRejected (res, std::move (ev), "ни одной годной области");
		return noop ("все веса 0 — исследования остаются правилам");
	}
	if (takenKeys.count ("research") != 0) return noop ("есть явный ChangeResearch в плане");
	const int total = player.getResearchCentersWorkingTotal();
	if (total <= 0) return noop ("нет работающих исследовательских центров — директива ждёт их");

	const std::vector<int> centers = largestRemainder (w, total);
	cOrder o;
	o.type = eOrderType::ChangeResearch;
	o.playerId = player.getId();
	o.level = "llm";
	nlohmann::json split = nlohmann::json::object();
	std::string text;
	for (std::size_t i = 0; i < centers.size(); ++i)
	{
		o.researchAreas[i] = centers[i];
		if (centers[i] > 0)
		{
			split[kAreaNames[i]] = centers[i];
			text += (text.empty() ? "" : ",") + std::string (kAreaNames[i]) + " " + std::to_string (centers[i]);
		}
	}
	o.reason = "llm-dir research: " + text;
	res.orders.push_back (std::move (o));
	ev["status"] = "accepted";
	ev["orders"] = 1;
	ev["centers"] = total;
	ev["split"] = split;
	++res.accepted;
	res.events.push_back (std::move (ev));
}

// --- army -----------------------------------------------------------------------

// путь до первой клетки, достижимой в этом ходу (по очкам хода юнита):
// юнит каждый ход снова свободен, и микро правил успевает вступить в бой
std::vector<cPosition> segmentForThisTurn (const cVehicle& v, const cMapView& view, const std::forward_list<cPosition>& path)
{
	std::vector<cPosition> seg;
	int left = v.data.getSpeed();
	cPosition prev = v.getPosition();
	for (const auto& p : path)
	{
		const int cost = cPathCalculator::calcNextCost (prev, p, &v, &view);
		if (cost > left) break;
		left -= cost;
		seg.push_back (p);
		prev = p;
	}
	return seg;
}

void translateArmy (const nlohmann::json& v, const cPlayer& player, const cModel& model,
	const std::map<std::string, const cOrder*>& ruleIdx, const std::set<std::string>& takenKeys, sDirectivesResult& res)
{
	nlohmann::json ev = directiveEvent ("army", v);
	if (!v.is_object())
		return finishRejected (res, std::move (ev), "нужен объект {\"mode\":…, \"target\":{x,y}}");
	const auto modeIt = v.find ("mode");
	if (modeIt == v.end() || !modeIt->is_string())
		return finishRejected (res, std::move (ev), "нет поля mode (attack|gather|defend|explore)");
	const std::string mode = modeIt->get<std::string>();
	if (std::find (std::begin (kArmyModes), std::end (kArmyModes), mode) == std::end (kArmyModes))
		return finishRejected (res, std::move (ev), "неизвестный mode «" + clipUtf8 (mode, 40) + "» (attack|gather|defend|explore)");
	ev["mode"] = mode;
	if (mode == "explore")
	{
		ev["status"] = "accepted";
		ev["orders"] = 0;
		ev["note"] = "разведка — правилам";
		++res.accepted;
		res.events.push_back (std::move (ev));
		return;
	}

	const auto& map = model.getMap();
	cPosition target = player.getLandingPos();
	if (mode != "defend")
	{
		const auto t = v.find ("target");
		if (t == v.end() || t->is_null()) return finishRejected (res, std::move (ev), "для " + mode + " нужен target {x,y}");
		const auto x = t->is_object() ? t->find ("x") : t->end();
		const auto y = t->is_object() ? t->find ("y") : t->end();
		if (!t->is_object() || x == t->end() || y == t->end() || !x->is_number_integer() || !y->is_number_integer())
			return finishRejected (res, std::move (ev), "target — объект {\"x\":int,\"y\":int}");
		const long long tx = x->get<long long>();
		const long long ty = y->get<long long>();
		const cPosition size = map->getSize();
		if (tx < 0 || ty < 0 || tx >= size.x() || ty >= size.y())
			return finishRejected (res, std::move (ev), "target (" + std::to_string (tx) + "," + std::to_string (ty) + ") вне карты "
				+ std::to_string (size.x()) + "x" + std::to_string (size.y()));
		target = cPosition (static_cast<int> (tx), static_cast<int> (ty));
	}
	ev["target"] = posJson (target);
	// gather/defend: чем больше армия, тем шире «уже на месте» (иначе юниты
	// толкаются у точки сбора)
	int combatTotal = 0;
	for (const auto& vp : player.getVehicles())
		if (combatUnit (*vp)) ++combatTotal;
	const int radius = mode == "attack" ? kAttackRadius : kGatherRadius + combatTotal / 10;

	const cMapView view (map, sharedPlayer (model, player.getId()));
	std::vector<const cVehicle*> candidates;
	int keptRule = 0;
	int notFree = 0;
	int arrived = 0;
	for (const auto& vp : player.getVehicles())
	{
		const cVehicle& u = *vp;
		if (!combatUnit (u)) continue;
		const std::string key = "act:" + std::to_string (u.getId());
		if (takenKeys.count (key) != 0) continue; // явный приказ плана
		const auto r = ruleIdx.find (key);
		if (r != ruleIdx.end())
		{
			const std::string& why = r->second->reason;
			if (why != "survey unexplored" && why != "advance (scout)")
			{
				++keptRule; // бой, отход, ремонт — микро правил
				continue;
			}
		}
		else
		{
			// без приказа правил: свободен ли и не стоит ли на точке отхода
			const int hpMax = u.data.getHitpointsMax();
			if (!vehicleIdle (u) || u.isBeingAttacked() || hpMax <= 0 || u.data.getHitpoints() * 100 / hpMax < kHealthyPct)
			{
				++notFree;
				continue;
			}
		}
		if ((u.getPosition() - target).l2NormSquared() <= radius * radius)
		{
			++arrived;
			continue;
		}
		candidates.push_back (&u);
	}

	// раздать юнитам разные клетки у цели (ближайшие свободные), путь — до
	// клетки, достижимой в этом ходу
	std::vector<std::pair<int, int>> reserved;   // клетки назначения
	std::vector<std::pair<int, int>> stepEnds;   // где юниты остановятся в этом ходу
	int emitted = 0;
	int unreachable = 0;
	nlohmann::json moved = nlohmann::json::array();
	for (const cVehicle* u : candidates)
	{
		std::vector<std::pair<int, cPosition>> cells; // (расстояние², клетка) по кольцам
		for (int r = 0; r <= kSnapRadius; ++r)
			for (int dy = -r; dy <= r; ++dy)
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
					const cPosition p = target + cPosition (dx, dy);
					if (!view.isValidPosition (p)) continue;
					if (std::find (reserved.begin(), reserved.end(), std::make_pair (p.x(), p.y())) != reserved.end()) continue;
					if (p != u->getPosition() && !view.possiblePlace (*u, p)) continue;
					cells.emplace_back ((p - target).l2NormSquared(), p);
				}
		std::stable_sort (cells.begin(), cells.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
		bool done = false;
		for (std::size_t k = 0; k < cells.size() && k < 4 && !done; ++k) // до 4 попыток пути
		{
			const cPosition dest = cells[k].second;
			if (dest == u->getPosition())
			{
				done = true; // ближайшая свободная клетка — та, где он стоит
				++arrived;
				reserved.emplace_back (dest.x(), dest.y());
				break;
			}
			cPathCalculator pc (*u, view, dest, false);
			const auto path = pc.calcPath();
			if (path.empty()) continue;
			std::vector<cPosition> seg = segmentForThisTurn (*u, view, path);
			done = true;
			reserved.emplace_back (dest.x(), dest.y());
			// два юнита не должны закончить ход в одной клетке: второй упрётся и
			// сервер пересчитает ему путь в обход
			while (!seg.empty() && std::find (stepEnds.begin(), stepEnds.end(), std::make_pair (seg.back().x(), seg.back().y())) != stepEnds.end())
				seg.pop_back();
			if (seg.empty()) break; // очков хода нет или некуда встать — в этом ходу не двигаем
			stepEnds.emplace_back (seg.back().x(), seg.back().y());
			cOrder o;
			o.type = eOrderType::Move;
			o.playerId = player.getId();
			o.unitId = u->getId();
			o.position = seg.back(); // куда дойдёт в этом ходу (журнал, сверка)
			o.path = std::move (seg);
			o.level = "llm";
			o.reason = "llm-dir army " + mode + " -> (" + std::to_string (target.x()) + "," + std::to_string (target.y()) + ")";
			moved.push_back ({{"unitId", u->getId()}, {"dest", posJson (dest)}, {"step", posJson (o.position)}, {"pathLen", o.path.size()}});
			res.orders.push_back (std::move (o));
			++emitted;
		}
		if (!done) ++unreachable;
	}

	ev["candidates"] = candidates.size();
	ev["keptRule"] = keptRule;
	ev["notFree"] = notFree;
	ev["arrived"] = arrived;
	ev["unreachable"] = unreachable;
	ev["moved"] = moved;
	if (!candidates.empty() && unreachable == static_cast<int> (candidates.size()))
		return finishRejected (res, std::move (ev), "target (" + std::to_string (target.x()) + "," + std::to_string (target.y())
			+ ") недостижим: ни одному юниту не нашёлся путь к клетке в радиусе " + std::to_string (kSnapRadius));
	ev["status"] = "accepted";
	ev["orders"] = emitted;
	if (candidates.empty())
		ev["note"] = arrived > 0 ? "свободные боевые юниты уже у цели" : "нет свободных боевых юнитов (в бою — правила)";
	++res.accepted;
	res.events.push_back (std::move (ev));
}

using ojson = nlohmann::ordered_json; // схема: порядок полей = порядок вывода модели

ojson cellSchema()
{
	return {{"type", "object"},
		{"properties", {{"x", {{"type", "integer"}}}, {"y", {{"type", "integer"}}}}},
		{"required", {"x", "y"}},
		{"additionalProperties", false}};
}

ojson weightSchema()
{
	return {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxWeight}};
}

// вариант приказа для anyOf: types — значения поля type, required/optional — поля
ojson orderVariant (std::initializer_list<const char*> types, std::initializer_list<std::pair<const char*, ojson>> required,
	std::initializer_list<std::pair<const char*, ojson>> optional = {})
{
	ojson typeEnum = ojson::array();
	for (const char* t : types) typeEnum.push_back (t);
	ojson props = ojson::object();
	props["type"] = {{"type", "string"}, {"enum", typeEnum}};
	ojson req = ojson::array ({"type"});
	for (const auto& [k, v] : required)
	{
		props[k] = v;
		req.push_back (k);
	}
	for (const auto& [k, v] : optional) props[k] = v;
	props["reason"] = {{"type", "string"}};
	return {{"type", "object"}, {"properties", props}, {"required", req}, {"additionalProperties", false}};
}

} // namespace

//------------------------------------------------------------------------------
std::string sLlmSwitches::directivesText() const
{
	std::string s;
	if (production) s += "production";
	if (research) s += std::string (s.empty() ? "" : ",") + "research";
	if (army) s += std::string (s.empty() ? "" : ",") + "army";
	return s.empty() ? "none" : s;
}

//------------------------------------------------------------------------------
std::string llmBaseVehicleId()
{
	return typeText (kBaseVehicle);
}

//------------------------------------------------------------------------------
std::string llmConflictKey (const cOrder& o)
{
	const std::string id = std::to_string (o.unitId);
	switch (o.type)
	{
		case eOrderType::Move:
		case eOrderType::Attack:
		case eOrderType::Build:
		case eOrderType::BuildPath:
		case eOrderType::Repair:
		case eOrderType::Rearm:
		case eOrderType::Transfer:
		case eOrderType::Load:
		case eOrderType::ActivateUnit:
			return "act:" + id;
		case eOrderType::ChangeBuildList: return "buildlist:" + id;
		case eOrderType::ChangeResearch: return "research";
		case eOrderType::StartWork:
		case eOrderType::StopWork:
			return "work:" + id;
		case eOrderType::FinishBuild: return "finish:" + id;
		default: return std::string (toString (o.type)) + ":" + id;
	}
}

//------------------------------------------------------------------------------
bool llmParseTypeId (const std::string& s, sTypeId& out)
{
	const auto dot = s.find_first_of (". ");
	if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size() || dot > 4 || s.size() - dot - 1 > 5) return false;
	for (std::size_t i = 0; i < s.size(); ++i)
		if (i != dot && (s[i] < '0' || s[i] > '9')) return false;
	out = sTypeId (std::atoi (s.substr (0, dot).c_str()), std::atoi (s.substr (dot + 1).c_str()));
	return true;
}

//------------------------------------------------------------------------------
nlohmann::json buildLlmCatalog (const cModel& model, int playerId)
{
	nlohmann::json cat = nlohmann::json::array();
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr) return cat;
	for (const cStaticUnitData* sd : catalogTypes (model, *player))
	{
		const cDynamicUnitData* dd = player->getLastUnitData (sd->ID);
		cat.push_back ({
			{"id", typeText (sd->ID)},
			{"name", sd->getDefaultName()},
			{"cost", dd->getBuildCost()},
			{"hp", dd->getHitpointsMax()},
			{"armor", dd->getArmor()},
			{"damage", dd->getDamage()},
			{"range", dd->getRange()},
			{"shots", dd->getShotsMax()},
			{"speed", dd->getSpeedMax()},
			{"scan", dd->getScan()},
			{"combat", sd->canAttack > 0 && dd->getRange() > 0},
		});
	}
	return cat;
}

//------------------------------------------------------------------------------
nlohmann::json buildLlmSummary (const cModel& model, int playerId)
{
	nlohmann::json s;
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr) return s;
	const cPosition base = player->getLandingPos();
	s["base"] = posJson (base);

	nlohmann::json factories = nlohmann::json::array();
	for (const cBuilding* b : ownFactories (*player))
	{
		std::vector<sID> q;
		for (std::size_t i = 0; i < b->getBuildListSize(); ++i) q.push_back (b->getBuildListItem (i).getType());
		factories.push_back ({{"id", b->getId()}, {"typeId", typeText (b->getStaticUnitData().ID)}, {"queue", queueJson (q)},
			{"repeat", b->getRepeatBuild()}, {"working", b->isUnitWorking()}});
	}
	s["factories"] = std::move (factories);

	int combat = 0;
	int idle = 0;
	long long sx = 0;
	long long sy = 0;
	for (const auto& v : player->getVehicles())
	{
		if (!combatUnit (*v)) continue;
		++combat;
		if (vehicleIdle (*v)) ++idle;
		sx += v->getPosition().x();
		sy += v->getPosition().y();
	}
	nlohmann::json army = {{"combatUnits", combat}, {"idleCombatUnits", idle}};
	army["center"] = combat > 0 ? posJson (cPosition (static_cast<int> (sx / combat), static_cast<int> (sy / combat))) : nlohmann::json();
	s["army"] = std::move (army);

	// видимые враги: ближайшие к базе
	const cMapView view (model.getMap(), sharedPlayer (model, playerId));
	std::vector<std::pair<int, nlohmann::json>> enemies;
	for (const auto& p : model.getPlayerList())
	{
		if (p->getId() == playerId) continue;
		for (const auto& v : p->getVehicles())
			if (view.canSeeUnit (*v))
				enemies.emplace_back ((v->getPosition() - base).l2NormSquared(),
					nlohmann::json{{"id", v->getId()}, {"type", v->getStaticUnitData().getDefaultName()}, {"pos", posJson (v->getPosition())}});
		for (const auto& b : p->getBuildings())
			if (view.canSeeUnit (*b))
				enemies.emplace_back ((b->getPosition() - base).l2NormSquared(),
					nlohmann::json{{"id", b->getId()}, {"type", b->getStaticUnitData().getDefaultName()}, {"pos", posJson (b->getPosition())}});
	}
	std::stable_sort (enemies.begin(), enemies.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
	s["enemiesVisible"] = enemies.size();
	nlohmann::json nearest = nlohmann::json::array();
	for (std::size_t i = 0; i < enemies.size() && i < kMaxSummaryEnemies; ++i) nearest.push_back (enemies[i].second);
	s["nearestEnemies"] = std::move (nearest);
	s["researchCentersWorking"] = player->getResearchCentersWorkingTotal();
	return s;
}

//------------------------------------------------------------------------------
nlohmann::ordered_json buildLlmPlanSchema (const nlohmann::json& catalog, const sLlmSwitches& sw)
{
	ojson prodProps = ojson::object();
	for (const auto& c : catalog)
		if (c.contains ("id") && c["id"].is_string()) prodProps[c["id"].get<std::string>()] = weightSchema();
	const ojson production = {{"type", "object"}, {"properties", prodProps}, {"additionalProperties", false}};

	ojson researchProps = ojson::object();
	for (const char* a : kAreaNames) researchProps[a] = weightSchema();
	const ojson research = {{"type", "object"}, {"properties", researchProps}, {"additionalProperties", false}};

	const ojson army = {{"type", "object"},
		{"properties", {{"mode", {{"type", "string"}, {"enum", {"attack", "gather", "defend", "explore"}}}}, {"target", cellSchema()}}},
		{"required", {"mode"}},
		{"additionalProperties", false}};

	// приказы: по варианту на тип — у каждого свои обязательные поля
	const ojson id = {{"type", "integer"}, {"minimum", 0}};
	const ojson typeId = {{"type", "string"}, {"pattern", "^[0-9]{1,2}[. ][0-9]{1,3}$"}};
	const ojson speed = {{"type", "integer"}, {"minimum", 0}, {"maximum", 2}};
	const ojson list = {{"type", "array"}, {"items", typeId}, {"minItems", 1}, {"maxItems", kMaxQueue}};
	const ojson areas = {{"type", "object"}, {"properties", researchProps}, {"additionalProperties", false}};
	const ojson value = {{"type", "integer"}, {"minimum", 1}};
	const ojson res = {{"type", "string"}, {"enum", {"metal", "oil", "gold"}}};
	const ojson variants = ojson::array ({
		orderVariant ({"Move"}, {{"unitId", id}, {"to", cellSchema()}}),
		orderVariant ({"Attack"}, {{"unitId", id}, {"target", cellSchema()}}),
		orderVariant ({"Build"}, {{"unitId", id}, {"building", typeId}, {"pos", cellSchema()}}, {{"buildSpeed", speed}}),
		orderVariant ({"BuildPath"}, {{"unitId", id}, {"building", typeId}, {"pos", cellSchema()}, {"pos2", cellSchema()}}, {{"buildSpeed", speed}}),
		orderVariant ({"ChangeBuildList"}, {{"unitId", id}, {"list", list}}, {{"repeat", {{"type", "boolean"}}}, {"buildSpeed", speed}}),
		orderVariant ({"ChangeResearch"}, {{"areas", areas}}),
		orderVariant ({"Repair", "Rearm", "Load"}, {{"unitId", id}, {"targetId", id}}),
		orderVariant ({"Transfer"}, {{"unitId", id}, {"targetId", id}, {"value", value}, {"res", res}}),
		orderVariant ({"ActivateUnit"}, {{"unitId", id}, {"targetId", id}, {"pos", cellSchema()}}),
		orderVariant ({"FinishBuild"}, {{"unitId", id}, {"pos", cellSchema()}}),
		orderVariant ({"StartWork", "StopWork", "ToggleSentry", "ToggleManualFire", "ToggleLayMines", "ToggleCollectMines"}, {{"unitId", id}}),
	});

	// выключенных директив и orders в схеме нет: модель о них не знает
	ojson dirProps = ojson::object();
	if (sw.production) dirProps["production"] = production;
	if (sw.research) dirProps["research"] = research;
	if (sw.army) dirProps["army"] = army;
	ojson props = ojson::object();
	props["strategy"] = {{"type", "string"}};
	ojson required = ojson::array ({"strategy"});
	if (sw.anyDirective())
	{
		props["directives"] = {{"type", "object"}, {"properties", dirProps}, {"additionalProperties", false}};
		required.push_back ("directives");
	}
	if (sw.orders) props["orders"] = {{"type", "array"}, {"items", {{"anyOf", variants}}}, {"maxItems", kMaxOrdersSchema}};
	return {{"type", "object"}, {"properties", props}, {"required", required}, {"additionalProperties", false}};
}

//------------------------------------------------------------------------------
sDirectivesResult translateLlmDirectives (const nlohmann::json& directives, int playerId, const cModel& model,
                                          const std::vector<cOrder>& ruleOrders, const std::set<std::string>& takenKeys,
                                          const sLlmSwitches& sw)
{
	sDirectivesResult res;
	if (!directives.is_object())
	{
		finishRejected (res, directiveEvent ("directives", directives), "directives — не JSON-объект");
		return res;
	}
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return res;

	const auto ruleIdx = indexRuleOrders (ruleOrders);
	for (const auto& [name, value] : directives.items())
		if (name != "production" && name != "research" && name != "army")
			finishRejected (res, directiveEvent ("unknown", value), "неизвестная директива «" + clipUtf8 (name, 40) + "» (production|research|army)");
	// выключенная директива: в промпте и схеме её нет; пришла всё же — игнорируем, область правилам
	auto enabled = [&] (const char* kind, bool on, const nlohmann::json& value) {
		if (on) return true;
		nlohmann::json ev = directiveEvent (kind, value);
		ev["status"] = "ignored";
		ev["orders"] = 0;
		ev["reason"] = "директива выключена (MAXR_LLM_DIRECTIVES) — решают правила";
		++res.ignored;
		res.events.push_back (std::move (ev));
		return false;
	};
	if (const auto it = directives.find ("production"); it != directives.end() && !it->is_null() && enabled ("production", sw.production, *it))
		translateProduction (*it, *player, model, ruleIdx, takenKeys, sw.prodBaseShare, sw.prodShareByMetal, res);
	if (const auto it = directives.find ("research"); it != directives.end() && !it->is_null() && enabled ("research", sw.research, *it))
		translateResearch (*it, *player, takenKeys, res);
	if (const auto it = directives.find ("army"); it != directives.end() && !it->is_null() && enabled ("army", sw.army, *it))
		translateArmy (*it, *player, model, ruleIdx, takenKeys, res);
	return res;
}

} // namespace ai
