/***************************************************************************
 * ai_base2_tech.cpp — замороженная копия ai_tech.cpp (мозг «base2»).
 *
 * ЗАМОРОЖЕН 26.10.2026, НЕ ПРАВИТЬ — эталон турниров с шага 11 (PLAN.md §6, §7 п. 71):
 * итог шага 10 (exe ai_headless_s10final, full D = 0.909 против base). Копия сделана
 * скриптом tmp/step11/tools/make_base2.py; отличия от ai_tech.cpp — только пространство имён
 * ai::base2, стражи заголовков, include копий и name() = "base2".
 ***************************************************************************/

/***************************************************************************
 * ai_tech.cpp — польза исследований и улучшений за золото (шаг 10). Описание — в заголовке.
 ***************************************************************************/

#include "ai_base2_tech.h"

#include "ai_base2_roles.h"
#include "ai_base2_upgrades.h"
#include "ai_base2_world_model.h"

#include "game/data/gamesettings.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/logic/upgradecalculator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ai::base2
{

namespace
{

constexpr double kEnemyDecay = 0.98; // пик числа машин врага по типу тает за ход
constexpr int kMaxLevelsAhead = 3;   // тема: смотреть до стольких уровней вперёд (бонус от исходных данных округляется)
constexpr int kMaxPurchaseSteps = 40;
// оценка силы (опыт «вариант против варианта», отчёт шага 10 §4; ход, обзор, боезапас — малые веса)
constexpr double kRangeK = 0.15;      // поправка на дальность: 1 + kRangeK × (наша − их), в пределах [0.5, 2]
constexpr double kSpeedW = 0.2;       // вес ln(ход)
constexpr double kScanW = 0.05;       // вес ln(обзор)
constexpr double kAmmoW = 0.05;       // вес ln(боезапас)
constexpr double kSwitchMargin = 1.25; // новая тема выгоднее текущей во столько раз — менять
constexpr int kSaveTurns = 20;        // копить на лучшую покупку, если доход дотянет за столько ходов

using RA = cResearch::eResearchArea;
using UT = cUpgradeCalculator::eUpgradeType;

const char* areaName (int a)
{
	static constexpr const char* names[8] = {"attack", "shots", "range", "armor", "hitpoints", "speed", "scan", "cost"};
	return a >= 0 && a < 8 ? names[a] : "none";
}

sCombatStats statsOf (const sID& id, const cDynamicUnitData& d, const cStaticUnitData& sd)
{
	sCombatStats s;
	s.id = id;
	s.damage = d.getDamage();
	s.shots = d.getShotsMax();
	s.range = d.getRange();
	s.armor = d.getArmor();
	s.hp = d.getHitpointsMax();
	s.speed = d.getSpeedMax() / 4;
	s.scan = d.getScan();
	s.ammo = d.getAmmoMax();
	s.cost = std::max (1, d.getBuildCost());
	s.canAttack = sd.canAttack;
	s.plane = sd.factorAir > 0;
	return s;
}

// бьёт ли a по b (по поверхности: самолёт — воздух, прочее — земля)
bool hits (const sCombatStats& a, const sCombatStats& b)
{
	if (a.damage <= 0 || a.shots <= 0 || a.range <= 0) return false;
	return (a.canAttack & (b.plane ? eTerrainFlag::Air : eTerrainFlag::Ground)) != 0;
}

} // namespace

//------------------------------------------------------------------------------
void cTechAdvisor::update (const cModel& model, const cPlayer& player, const cRoleBook& book, const cComposition& comp,
	const cWorldModel& world, int turn)
{
	turn_ = turn;
	const auto settings = model.getGameSettings();
	horizon_ = (settings && settings->victoryConditionType == eGameSettingsVictoryCondition::Turns)
		? static_cast<int> (settings->victoryTurns) : turn + 150;
	horizon_ = std::max (horizon_, turn + 1);
	const auto& ud = *model.getUnitsData();

	// наша будущая армия: роли строя с весами состава (шаг 9), ПВО — когда видна авиация врага
	ours_.clear();
	for (eRole r : {eRole::Tank, eRole::Assault, eRole::Artillery, eRole::Missile})
	{
		const sID id = book.typeFor (r, "BigGroundVehicle");
		const int w = comp.weight (r);
		if (id == sID() || w <= 0) continue;
		const cDynamicUnitData* d = player.getLastUnitData (id);
		if (d == nullptr) continue;
		ours_.push_back ({statsOf (id, *d, ud.getStaticUnitData (id)), static_cast<double> (w)});
	}
	if (comp.needs().antiAir > 0)
	{
		const sID aa = book.typeFor (eRole::AntiAir, "SmallGroundVehicle");
		if (const cDynamicUnitData* d = aa != sID() ? player.getLastUnitData (aa) : nullptr)
		{
			double total = 0;
			for (const auto& o : ours_) total += o.w;
			ours_.push_back ({statsOf (aa, *d, ud.getStaticUnitData (aa)), total * 0.2});
		}
	}
	double wsum = 0;
	for (const auto& o : ours_) wsum += o.w;
	if (wsum > 0)
		for (auto& o : ours_) o.w /= wsum;

	// враг: вооружённые типы из памяти (данные — у последнего наблюдённого), число — пик с таянием
	std::map<sID, int> now;
	std::map<sID, int> latest; // ход наблюдения данных типа
	for (const auto& kv : world.enemies())
	{
		const sEnemyMemory& m = kv.second;
		if (!m.armed() || m.ship) continue;
		const sID id (m.typeFirst, m.typeSecond);
		if (!ud.isValidId (id)) continue;
		++now[id];
		auto it = latest.find (id);
		if (it != latest.end() && it->second > m.seenTurn) continue;
		latest[id] = m.seenTurn;
		const auto& sd = ud.getStaticUnitData (id);
		sCombatStats s;
		s.id = id;
		s.damage = m.damage;
		s.shots = m.shotsMax;
		s.range = m.range;
		s.armor = m.armor;
		s.hp = m.hpMax;
		s.speed = m.speed;
		s.scan = m.scan;
		s.ammo = ud.getDynamicUnitData (id).getAmmoMax();
		s.cost = std::max (1, ud.getDynamicUnitData (id).getBuildCost());
		s.canAttack = m.canAttack;
		s.plane = m.plane;
		enemyStats_[id] = s;
	}
	for (auto& kv : enemyPeak_) kv.second *= kEnemyDecay;
	for (const auto& kv : now) enemyPeak_[kv.first] = std::max (enemyPeak_[kv.first], static_cast<double> (kv.second));
	enemy_.clear();
	for (const auto& kv : enemyPeak_)
		if (kv.second >= 0.5 && enemyStats_.count (kv.first) != 0) enemy_.push_back ({enemyStats_[kv.first], kv.second});
	enemySeen_ = !enemy_.empty();
	if (!enemySeen_)
	{
		// врага не видели — зеркало нашей армии в исходной версии (ни исследований, ни улучшений)
		for (const auto& o : ours_)
		{
			const auto& d = ud.getDynamicUnitData (o.s.id);
			enemy_.push_back ({statsOf (o.s.id, d, ud.getStaticUnitData (o.s.id)), o.w / std::max (1, d.getBuildCost())});
		}
	}
}

//------------------------------------------------------------------------------
double cTechAdvisor::valueOf (const std::vector<sWeighted>& ours) const
{
	if (ours.empty() || enemy_.empty()) return 0;
	double enemyCount = 0;
	for (const auto& e : enemy_) enemyCount += e.w;
	if (enemyCount <= 0) return 0;
	auto rangeF = [] (int ra, int rb) { return std::clamp (1.0 + kRangeK * (ra - rb), 0.5, 2.0); };
	std::vector<double> n (ours.size());
	double N = 0;
	for (size_t i = 0; i < ours.size(); ++i)
	{
		n[i] = ours[i].w / std::max (1, ours[i].s.cost); // машин на металл (квадратичный закон: число — в квадрате)
		N += n[i];
	}
	double dps = 0, hp = 0, edps = 0, extra = 0;
	for (size_t i = 0; i < ours.size(); ++i)
	{
		const auto& u = ours[i].s;
		hp += n[i] * u.hp;
		for (const auto& e : enemy_)
			if (hits (u, e.s))
				dps += n[i] * (e.w / enemyCount) * u.shots * std::max (1, u.damage - e.s.armor) * rangeF (u.range, e.s.range);
		extra += ours[i].w * (kSpeedW * std::log (std::max (1, u.speed)) + kScanW * std::log (std::max (1, u.scan))
			+ kAmmoW * std::log (std::max (1, u.ammo)));
	}
	for (const auto& e : enemy_)
		for (size_t i = 0; i < ours.size(); ++i)
			if (hits (e.s, ours[i].s))
				edps += (e.w / enemyCount) * (n[i] / N) * e.s.shots * std::max (1, e.s.damage - ours[i].s.armor) * rangeF (e.s.range, ours[i].s.range);
	return std::log (std::max (dps, 1e-6)) + std::log (std::max (hp, 1e-6)) - std::log (std::max (edps, 1e-2)) + extra;
}

//------------------------------------------------------------------------------
double cTechAdvisor::value() const
{
	return valueOf (ours_);
}

//------------------------------------------------------------------------------
std::vector<cTechAdvisor::sWeighted> cTechAdvisor::withResearch (const cModel& model, const cPlayer& player, int area, int levels) const
{
	std::vector<sWeighted> out = ours_;
	const auto& ud = *model.getUnitsData();
	const auto& uc = cUpgradeCalculator::instance();
	const int L = player.getResearchState().getCurResearchLevel (static_cast<RA> (area));
	const int L2 = L + 10 * levels;
	for (auto& o : out)
	{
		const cDynamicUnitData& orig = ud.getDynamicUnitData (o.s.id, player.getClan());
		const cDynamicUnitData* last = player.getLastUnitData (o.s.id);
		if (last == nullptr) continue;
		const bool human = ud.getStaticUnitData (o.s.id).vehicleData.isHuman;
		auto delta = [&] (int start) { return uc.calcChangeByResearch (start, L2) - uc.calcChangeByResearch (start, L); };
		switch (static_cast<RA> (area))
		{
			case RA::AttackResearch: o.s.damage = last->getDamage() + delta (orig.getDamage()); break;
			case RA::ShotsResearch: o.s.shots = last->getShotsMax() + delta (orig.getShotsMax()); break;
			case RA::RangeResearch: o.s.range = last->getRange() + delta (orig.getRange()); break;
			case RA::ArmorResearch: o.s.armor = last->getArmor() + delta (orig.getArmor()); break;
			case RA::HitpointsResearch: o.s.hp = last->getHitpointsMax() + delta (orig.getHitpointsMax()); break;
			case RA::SpeedResearch: o.s.speed = (last->getSpeedMax() + delta (orig.getSpeedMax())) / 4; break;
			case RA::ScanResearch: o.s.scan = last->getScan() + delta (orig.getScan()); break;
			case RA::CostResearch:
			{
				const auto type = human ? cUpgradeCalculator::eUnitType::Infantry : cUpgradeCalculator::eUnitType::StandardUnit;
				const int d = uc.calcChangeByResearch (orig.getBuildCost(), L2, UT::Cost, type)
					- uc.calcChangeByResearch (orig.getBuildCost(), L, UT::Cost, type);
				o.s.cost = std::max (1, last->getBuildCost() + d);
				break;
			}
		}
	}
	return out;
}

//------------------------------------------------------------------------------
std::array<int, 8> cTechAdvisor::researchPlan (const cModel& model, const cPlayer& player, int centers)
{
	std::array<int, 8> plan {};
	researchWhy_.clear();
	if (centers <= 0 || ours_.empty()) return plan;
	const cResearch& rs = player.getResearchState();
	const auto& uc = cUpgradeCalculator::instance();
	const double v0 = valueOf (ours_);
	const int left = std::max (1, horizon_ - turn_);

	// для каждой темы: прирост V и очки до 1…3 уровней вперёд
	struct sOpt
	{
		double gain = 0;
		int points = 0;
	};
	std::array<std::array<sOpt, kMaxLevelsAhead>, 8> opts {};
	for (int a = 0; a < 8; ++a)
	{
		const RA area = static_cast<RA> (a);
		int pts = rs.getRemainingTurns (area, 1); // при одном центре — оставшиеся очки уровня
		if (pts <= 0) continue;                    // предел таблицы
		int level = rs.getCurResearchLevel (area);
		for (int k = 1; k <= kMaxLevelsAhead; ++k)
		{
			if (k > 1)
			{
				const auto more = uc.calcResearchTurns (level, rs.getUpgradeCalculatorUpgradeType (area));
				if (!more) break;
				pts += *more;
			}
			level += 10;
			opts[a][k - 1] = {valueOf (withResearch (model, player, a, k)) - v0, pts};
		}
	}
	// польза темы при c центрах: лучший из горизонтов, прирост × доля партии после × на очко
	auto score = [&] (int a, int c) {
		double best = 0;
		for (const auto& o : opts[a])
		{
			if (o.points <= 0 || o.gain <= 0) continue;
			const double t = static_cast<double> (o.points) / std::max (1, c);
			const double benefit = std::max (0.0, left - t) / left;
			best = std::max (best, o.gain * benefit / o.points);
		}
		return best;
	};

	// все центры — на одну тему (klei1984; деление по пользе — в шуме, отчёт §4)
	int best = -1;
	double bestScore = 0;
	for (int a = 0; a < 8; ++a)
	{
		const double s = score (a, centers);
		if (s > bestScore)
		{
			bestScore = s;
			best = a;
		}
	}
	// менять тему — только если новая заметно выгоднее (вложенные очки текущей не пропадают, но
	// дают пользу, лишь когда уровень взят)
	if (topic_ >= 0 && best >= 0 && best != topic_ && score (topic_, centers) * kSwitchMargin >= bestScore)
		best = topic_;
	if (best < 0) best = topic_ >= 0 ? topic_ : static_cast<int> (RA::AttackResearch);
	topic_ = best;
	plan[best] = centers;
	researchWhy_ = std::string ("research by use: ") + areaName (best);
	char buf[64];
	for (int a = 0; a < 8; ++a)
	{
		const double s = score (a, centers);
		if (s <= 0) continue;
		std::snprintf (buf, sizeof (buf), " %s=%.2g", areaName (a), s * 1000);
		researchWhy_ += buf;
	}
	return plan;
}

//------------------------------------------------------------------------------
std::vector<cTechAdvisor::sPurchase> cTechAdvisor::purchases (const cModel& model, const cPlayer& player, int credits, int income)
{
	std::vector<sPurchase> out;
	saving_ = false;
	if (credits <= 0 || ours_.empty()) return out;
	const auto& ud = *model.getUnitsData();
	const auto& uc = cUpgradeCalculator::instance();

	std::vector<sWeighted> cur = ours_;
	std::vector<std::array<int, 8>> steps (cur.size());
	std::vector<int> spent (cur.size(), 0); // цена купленного по типу (кредиты)
	int left = credits;
	double v = valueOf (cur);

	// данные после ещё одного шага вида k у типа i
	auto stepped = [&] (size_t i, int k) {
		sCombatStats s = cur[i].s;
		const cDynamicUnitData& orig = ud.getDynamicUnitData (s.id, player.getClan());
		switch (kUpgradeKinds[k])
		{
			case sUnitUpgrade::eUpgradeType::Damage: s.damage += uc.calcIncreaseByUpgrade (orig.getDamage()); break;
			case sUnitUpgrade::eUpgradeType::Shots: s.shots += uc.calcIncreaseByUpgrade (orig.getShotsMax()); break;
			case sUnitUpgrade::eUpgradeType::Range: s.range += uc.calcIncreaseByUpgrade (orig.getRange()); break;
			case sUnitUpgrade::eUpgradeType::Ammo: s.ammo += uc.calcIncreaseByUpgrade (orig.getAmmoMax()); break;
			case sUnitUpgrade::eUpgradeType::Armor: s.armor += uc.calcIncreaseByUpgrade (orig.getArmor()); break;
			case sUnitUpgrade::eUpgradeType::Hits: s.hp += uc.calcIncreaseByUpgrade (orig.getHitpointsMax()); break;
			case sUnitUpgrade::eUpgradeType::Scan: s.scan += uc.calcIncreaseByUpgrade (orig.getScan()); break;
			case sUnitUpgrade::eUpgradeType::Speed: s.speed += uc.calcIncreaseByUpgrade (orig.getSpeedMax() / 4); break;
			default: break;
		}
		return s;
	};

	for (int iter = 0; iter < kMaxPurchaseSteps; ++iter)
	{
		struct sCand
		{
			size_t i = 0;
			int k = 0;
			int price = 0;
			double gain = 0;
			sCombatStats s;
		};
		bool haveBest = false, haveAffordable = false;
		sCand best, bestAffordable;
		for (size_t i = 0; i < cur.size(); ++i)
			for (int k = 0; k < 8; ++k)
			{
				auto st = steps[i];
				++st[k];
				cUnitUpgrade upg;
				int total = 0;
				if (!makeUnitUpgrade (model, player, cur[i].s.id, st, upg, total)) continue;
				const int price = total - spent[i];
				if (price <= 0) continue;
				sCand c;
				c.i = i;
				c.k = k;
				c.price = price;
				c.s = stepped (i, k);
				auto trial = cur;
				trial[i].s = c.s;
				c.gain = valueOf (trial) - v;
				if (c.gain <= 0) continue;
				auto better = [] (const sCand& a, const sCand& b) { return a.gain * b.price > b.gain * a.price; };
				if (!haveBest || better (c, best))
				{
					best = c;
					haveBest = true;
				}
				if (price <= left && (!haveAffordable || better (c, bestAffordable)))
				{
					bestAffordable = c;
					haveAffordable = true;
				}
			}
		if (!haveBest) break;
		sCand take = best;
		if (best.price > left)
		{
			// лучшее не по карману: копить, если доход дотянет; иначе — лучшее из доступного
			if (income > 0 && left + income * kSaveTurns >= best.price)
			{
				saving_ = true;
				break;
			}
			if (!haveAffordable) break;
			take = bestAffordable;
		}
		++steps[take.i][take.k];
		spent[take.i] += take.price;
		left -= take.price;
		cur[take.i].s = take.s;
		v += take.gain;
	}
	for (size_t i = 0; i < cur.size(); ++i)
	{
		if (spent[i] <= 0) continue;
		sPurchase p;
		p.id = cur[i].s.id;
		p.steps = steps[i];
		p.cost = spent[i];
		p.gain = valueOf (cur) - valueOf (ours_);
		out.push_back (p);
	}
	return out;
}

} // namespace ai::base2
