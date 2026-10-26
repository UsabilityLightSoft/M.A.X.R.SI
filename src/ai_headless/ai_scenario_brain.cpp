/***************************************************************************
 * ai_scenario_brain.cpp — сценарная проверка новых приказов ИИИ (шаг 11).
 ***************************************************************************/

#include "ai_scenario_brain.h"

#include "ai_types.h"

#include "game/data/map/map.h"
#include "game/data/map/mapview.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/units/building.h"
#include "game/data/units/commandodata.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/pathcalculator.h"
#include "game/logic/turncounter.h"
#include "utility/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <utility>

namespace ai
{

namespace
{

std::shared_ptr<const cPlayer> sharedPlayerOf (const cModel& model, int id)
{
	for (const auto& p : model.getPlayerList())
		if (p->getId() == id) return p;
	return nullptr;
}

// своя машина типа type (имя по данным), не в грузу, с наименьшим id, кроме except
const cVehicle* findOwn (const cPlayer& player, const std::string& type, unsigned int except = kNoUnit)
{
	const cVehicle* best = nullptr;
	for (const auto& v : player.getVehicles())
	{
		if (v->getStaticUnitData().getDefaultName() != type || v->getId() == except || v->isUnitLoaded()) continue;
		if (best == nullptr || v->getId() < best->getId()) best = v.get();
	}
	return best;
}

std::vector<cPosition> toVector (const std::forward_list<cPosition>& path)
{
	return {path.begin(), path.end()};
}

// путь до хода: столько клеток, сколько машина пройдёт за ход
void cutToTurn (const cVehicle& v, const cMapView& view, std::vector<cPosition>& path, std::size_t maxSteps = 1000)
{
	int budget = v.data.getSpeed();
	cPosition prev = v.getPosition();
	std::size_t n = 0;
	for (; n < path.size() && n < maxSteps; ++n)
	{
		const int c = cPathCalculator::calcNextCost (prev, path[n], &v, &view);
		if (c > budget) break;
		budget -= c;
		prev = path[n];
	}
	path.resize (n);
}

nlohmann::json posJson (const cPosition& p)
{
	return {{"x", p.x()}, {"y", p.y()}};
}

cOrder makeOrder (eOrderType type, int playerId, unsigned int unitId, std::string reason)
{
	cOrder o;
	o.type = type;
	o.playerId = playerId;
	o.unitId = unitId;
	o.level = "scenario";
	o.reason = std::move (reason);
	return o;
}

} // namespace

std::vector<nlohmann::json> cScenarioBrain::takeJournalEvents()
{
	return std::exchange (events_, {});
}

void cScenarioBrain::report (const std::string& check, bool ok, nlohmann::json details)
{
	details["event"] = "scenario";
	details["check"] = check;
	details["ok"] = ok;
	Log.info ("ai_scenario: " + check + (ok ? " OK " : " FAIL ") + details.dump());
	events_.push_back (std::move (details));
}

void cScenarioBrain::step (const std::string& check, nlohmann::json details)
{
	details["event"] = "scenario_step";
	details["check"] = check;
	events_.push_back (std::move (details));
}

std::vector<cOrder> cScenarioBrain::decideTurn (int playerId, const cModel& model)
{
	turn_ = model.getTurnCounter() ? model.getTurnCounter()->getTurn() : 0;
	return act (playerId, model, 1);
}

std::vector<cOrder> cScenarioBrain::decidePass (int playerId, const cModel& model, int pass)
{
	return act (playerId, model, pass);
}

std::vector<cOrder> cScenarioBrain::act (int playerId, const cModel& model, int pass)
{
	std::vector<cOrder> out;
	wantPass_ = false;
	const cPlayer* player = model.getPlayer (playerId);
	if (player == nullptr || player->isDefeated) return out;
	cMapView view (model.getMap(), sharedPlayerOf (model, playerId));
	if (playerId == 0)
	{
		if (!assigned_)
		{
			// пехота по проверкам: первая — БМП берёт её (Load), вторая — входит в БМП сама
			// (Move + GetIn), третья — входит в казарму и выходит
			std::vector<unsigned int> inf;
			for (const auto& v : player->getVehicles())
				if (v->getStaticUnitData().getDefaultName() == "infantery") inf.push_back (v->getId());
			std::sort (inf.begin(), inf.end());
			if (inf.size() > 0) apc_.cargoPick = inf[0];
			if (inf.size() > 1) apc2_.cargoPick = inf[1];
			if (inf.size() > 2) store_.cargo = inf[2];
			assigned_ = true;
		}
		sentry (playerId, model, pass, out);
		transport (apc_, playerId, model, view, pass, out);
		if (apc_.done) transport (apc2_, playerId, model, view, pass, out);
		transport (air_, playerId, model, view, pass, out);
		store (playerId, model, view, pass, out);
		commando (alien_, playerId, model, view, pass, out);
		commando (enemy_, playerId, model, view, pass, out);
	}
	else
		victim (playerId, model, view, pass, out);
	return out;
}

// --- транспорт: погрузить → перевезти → выгрузить в одном ходу, по заходу на шаг ------------
void cScenarioBrain::transport (sTransport& t, int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	if (t.done) return;
	const cPlayer& player = *model.getPlayer (playerId);
	const cMap& map = *model.getMap();
	const cVehicle* carrier = t.carrier != kNoUnit ? model.getVehicleFromID (t.carrier) : nullptr;
	const cVehicle* cargo = t.cargo != kNoUnit ? model.getVehicleFromID (t.cargo) : nullptr;
	if (t.state > 0 && t.turn != turn_)
	{
		step (t.check, {{"what", "ход сменился до конца перевозки — заново"}, {"state", t.state}});
		t.state = 0;
	}
	if (t.state > 0 && (carrier == nullptr || cargo == nullptr))
	{
		report (t.check, false, {{"why", "транспорт или груз потерян"}});
		t.done = true;
		return;
	}
	const bool plane = carrier != nullptr && carrier->getStaticUnitData().factorAir > 0;
	switch (t.state)
	{
		case 0: // погрузить — в первом заходе хода: у транспорта и груза весь ход
		{
			if (pass != 1) return;
			if (++t.tries > 6)
			{
				report (t.check, false, {{"why", "за 6 ходов не вышло погрузить, перевезти и выгрузить в одном ходу"}});
				t.done = true;
				return;
			}
			carrier = findOwn (player, t.carrierType);
			cargo = t.cargoPick != kNoUnit ? model.getVehicleFromID (t.cargoPick) : findOwn (player, t.cargoType, sentryTank_);
			if (cargo != nullptr && cargo->isUnitLoaded()) cargo = nullptr;
			if (carrier == nullptr || cargo == nullptr)
			{
				report (t.check, false, {{"why", "нет машин сценария"}});
				t.done = true;
				return;
			}
			t.carrier = carrier->getId();
			t.cargo = cargo->getId();
			const bool airborne = carrier->getStaticUnitData().factorAir > 0;
			if (carrier->canLoad (cargo))
			{
				cOrder o = makeOrder (eOrderType::Load, playerId, t.carrier, "scenario: load");
				o.unitId2 = t.cargo;
				out.push_back (std::move (o));
			}
			else
			{
				// авиатранспорт летит к машине и берёт её (Move + Load); пехота идёт к БМП и входит (Move + GetIn)
				const cVehicle& mover = airborne ? *carrier : *cargo;
				std::vector<cPosition> path;
				if (airborne)
					path = toVector (cPathCalculator (*carrier, view, cargo->getPosition(), static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
				else
					path = toVector (cPathCalculator (*cargo, view, *carrier, true).calcPath());
				const std::size_t full = path.size();
				cutToTurn (mover, view, path);
				if (path.empty())
				{
					step (t.check, {{"what", "нет пути к погрузке"}});
					return;
				}
				cOrder o = makeOrder (eOrderType::Move, playerId, mover.getId(), "scenario: move to load");
				o.path = path;
				o.position = path.back();
				if (path.size() == full) // дойдёт за ход — погрузка в конце пути
				{
					o.unitId2 = airborne ? t.cargo : t.carrier;
					o.moveEnd = airborne ? eMoveEnd::Load : eMoveEnd::GetIn;
				}
				out.push_back (std::move (o));
				if (path.size() != full)
				{
					step (t.check, {{"what", "сближение: за ход не дойти"}, {"left", full - path.size()}});
					return; // в этом ходу перевезти не выйдет — следующий ход заново
				}
			}
			t.turn = turn_;
			t.passes = {pass};
			t.state = 1;
			wantPass_ = true;
			step (t.check, {{"what", "погрузка"}, {"carrier", t.carrier}, {"cargo", t.cargo}, {"pass", pass}});
			return;
		}
		case 1: // перевезти
		{
			if (!cargo->isUnitLoaded() || std::find (carrier->storedUnits.begin(), carrier->storedUnits.end(), cargo) == carrier->storedUnits.end())
			{
				step (t.check, {{"what", "не погрузилась — заново в следующем ходу"}, {"pass", pass}});
				t.state = 0;
				return;
			}
			// клетка в 5 шагах к середине карты
			const cPosition size = map.getSize();
			const cPosition centre (size.x() / 2, size.y() / 2);
			const cPosition d = centre - carrier->getPosition();
			const auto sign = [] (int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); };
			cPosition dest = carrier->getPosition() + cPosition (sign (d.x()) * 5, sign (d.y()) * 5);
			dest = cPosition (std::clamp (dest.x(), 0, size.x() - 1), std::clamp (dest.y(), 0, size.y() - 1));
			auto path = toVector (cPathCalculator (*carrier, view, dest, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
			if (path.empty()) // клетка занята — к ближайшей свободной рядом
				for (int r = 1; r <= 3 && path.empty(); ++r)
					for (int dy = -r; dy <= r && path.empty(); ++dy)
						for (int dx = -r; dx <= r && path.empty(); ++dx)
						{
							const cPosition c = dest + cPosition (dx, dy);
							if (!map.isValidPosition (c) || !view.possiblePlace (*carrier, c)) continue;
							path = toVector (cPathCalculator (*carrier, view, c, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
						}
			cutToTurn (*carrier, view, path, 5);
			if (path.empty())
			{
				report (t.check, false, {{"why", "транспорту некуда ехать"}});
				t.done = true;
				return;
			}
			t.from = carrier->getPosition();
			cOrder o = makeOrder (eOrderType::Move, playerId, t.carrier, "scenario: carry");
			o.path = path;
			o.position = path.back();
			out.push_back (std::move (o));
			t.passes.push_back (pass);
			t.state = 2;
			wantPass_ = true;
			step (t.check, {{"what", "перевозка"}, {"from", posJson (t.from)}, {"to", posJson (path.back())}, {"pass", pass}});
			return;
		}
		case 2: // выгрузить
		{
			std::vector<cPosition> cells;
			if (plane)
				cells.push_back (carrier->getPosition()); // из авиатранспорта — под ним
			else
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						if (dx != 0 || dy != 0) cells.push_back (carrier->getPosition() + cPosition (dx, dy));
			const cPosition* exit = nullptr;
			for (const auto& c : cells)
				if (map.isValidPosition (c) && carrier->canExitTo (c, map, cargo->getStaticUnitData()))
				{
					exit = &c;
					break;
				}
			if (exit == nullptr)
			{
				report (t.check, false, {{"why", "выгрузить некуда"}, {"at", posJson (carrier->getPosition())}});
				t.done = true;
				return;
			}
			cOrder o = makeOrder (eOrderType::ActivateUnit, playerId, t.carrier, "scenario: unload");
			o.unitId2 = t.cargo;
			o.position = *exit;
			out.push_back (std::move (o));
			t.passes.push_back (pass);
			t.state = 3;
			wantPass_ = true;
			step (t.check, {{"what", "выгрузка"}, {"at", posJson (*exit)}, {"pass", pass}});
			return;
		}
		case 3: // проверить
		{
			const bool unloaded = !cargo->isUnitLoaded();
			const bool moved = carrier->getPosition() != t.from;
			const bool nextTo = carrier->isNextTo (cargo->getPosition());
			report (t.check, unloaded && moved && nextTo,
				{{"turn", t.turn}, {"passes", t.passes}, {"checkPass", pass}, {"carrier", t.carrier}, {"cargo", t.cargo},
				 {"from", posJson (t.from)}, {"to", posJson (carrier->getPosition())}, {"cargoAt", posJson (cargo->getPosition())},
				 {"unloaded", unloaded}, {"moved", moved}, {"cargoNextToCarrier", nextTo}, {"tries", t.tries}});
			t.done = true;
			return;
		}
		default: return;
	}
}

// --- казарма: конструктор строит, пехота входит и выходит (погрузка в здание и обратно) -------
void cScenarioBrain::store (int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	if (store_.done) return;
	const cPlayer& player = *model.getPlayer (playerId);
	const cMap& map = *model.getMap();
	const auto fail = [&] (const char* why) {
		report ("store_building", false, {{"why", why}, {"state", store_.state}});
		store_.done = true;
	};
	switch (store_.state)
	{
		case 0: // стройка казармы: 2×2 с клеткой конструктора (иначе — подъехать к свободному месту), скорость обычная
		{
			const cVehicle* builder = findOwn (player, "konstrukt");
			if (pass != 1 && !(builder != nullptr && store_.movedTurn == turn_)) return;
			const auto& all = model.getUnitsData()->getStaticUnitsData();
			// казарма — здание, что хранит пехоту (имя в данных — «baracks», ищем по свойствам)
			const auto it = std::find_if (all.begin(), all.end(), [] (const cStaticUnitData& d) {
				return d.ID.isABuilding() && d.storageUnitsMax > 0
					&& std::find (d.storeUnitsTypes.begin(), d.storeUnitsTypes.end(), "Human") != d.storeUnitsTypes.end();
			});
			if (builder == nullptr || it == all.end())
			{
				report ("store_building", false, {{"why", "нет конструктора или казармы в данных"}, {"builder", builder ? static_cast<int> (builder->getId()) : -1},
					{"unitTypes", all.size()}, {"found", it != all.end()}});
				store_.done = true;
				return;
			}
			const cPosition at = builder->getPosition();
			for (const cPosition& q : std::array<cPosition, 4>{at, at - cPosition (1, 0), at - cPosition (0, 1), at - cPosition (1, 1)})
			{
				bool fits = true;
				for (const cPosition& c : std::array<cPosition, 4>{q, q + cPosition (1, 0), q + cPosition (0, 1), q + cPosition (1, 1)})
					fits = fits && map.isValidPosition (c) && map.possiblePlaceBuilding (*it, c, nullptr, builder);
				if (!fits) continue;
				cOrder o = makeOrder (eOrderType::Build, playerId, builder->getId(), "scenario: build barracks");
				o.buildingType = sTypeId (it->ID.firstPart, it->ID.secondPart);
				o.position = q;
				o.buildSpeed = 0;
				out.push_back (std::move (o));
				store_.builder = builder->getId();
				store_.buildingType = it->ID;
				store_.state = 1;
				step ("store_building", {{"what", "стройка казармы"}, {"at", posJson (q)}});
				return;
			}
			// рядом не встаёт — подъехать: свободное место 2×2 поближе, клетка конструктора — его левый верхний угол
			if (store_.movedTurn != turn_ && builder->data.getSpeed() > 0)
				for (int r = 1; r <= 8; ++r)
					for (int dy = -r; dy <= r; ++dy)
						for (int dx = -r; dx <= r; ++dx)
						{
							if (std::max (std::abs (dx), std::abs (dy)) != r) continue;
							const cPosition q = at + cPosition (dx, dy);
							bool fits = true;
							for (const cPosition& c : std::array<cPosition, 4>{q, q + cPosition (1, 0), q + cPosition (0, 1), q + cPosition (1, 1)})
								fits = fits && map.isValidPosition (c) && map.possiblePlaceBuilding (*it, c, nullptr, builder);
							if (!fits) continue;
							auto path = toVector (cPathCalculator (*builder, view, q, static_cast<const std::vector<cVehicle*>*> (nullptr)).calcPath());
							const std::size_t full = path.size();
							cutToTurn (*builder, view, path);
							if (path.empty()) continue;
							cOrder o = makeOrder (eOrderType::Move, playerId, builder->getId(), "scenario: constructor to the barracks site");
							o.path = path;
							o.position = path.back();
							out.push_back (std::move (o));
							store_.movedTurn = turn_;
							if (path.size() == full) wantPass_ = true; // доехал — строить в следующем заходе
							step ("store_building", {{"what", "конструктор едет к месту"}, {"site", posJson (q)}});
							return;
						}
			if (++store_.waitTurns > 6) return fail ("казарму не поставить у конструктора");
			return;
		}
		case 1: // ждать конца стройки и вывести конструктора (FinishBuild)
		{
			const cVehicle* builder = model.getVehicleFromID (store_.builder);
			if (builder == nullptr) return fail ("конструктор потерян");
			for (const auto& b : player.getBuildings())
				if (b->getStaticUnitData().ID == store_.buildingType)
				{
					store_.building = b->getId();
					store_.state = 2;
					step ("store_building", {{"what", "казарма готова"}, {"building", store_.building}});
					break;
				}
			if (store_.state == 2) break; // сразу к погрузке
			if (pass != 1) return;
			if (builder->isUnitBuildingABuilding() && builder->getBuildTurns() == 0)
			{
				for (int dy = -1; dy <= 2; ++dy)
					for (int dx = -1; dx <= 2; ++dx)
					{
						const cPosition p = builder->getPosition() + cPosition (dx, dy);
						if (!map.isValidPosition (p) || !builder->isNextTo (p) || !map.possiblePlace (*builder, p, false)) continue;
						cOrder o = makeOrder (eOrderType::FinishBuild, playerId, store_.builder, "scenario: finish barracks");
						o.position = p;
						out.push_back (std::move (o));
						wantPass_ = true;
						return;
					}
				return fail ("конструктору некуда выйти");
			}
			if (++store_.waitTurns > 40) return fail ("казарма не достроена за 40 ходов");
			return;
		}
		default: break;
	}
	const cBuilding* building = model.getBuildingFromID (store_.building);
	const cVehicle* cargo = model.getVehicleFromID (store_.cargo);
	if (building == nullptr || cargo == nullptr) return fail ("казарма или пехота потеряны");
	switch (store_.state)
	{
		case 2: // войти: рядом — здание грузит (Load), иначе пехота идёт и входит (Move + GetIn)
		{
			if (building->canLoad (cargo))
			{
				cOrder o = makeOrder (eOrderType::Load, playerId, store_.building, "scenario: load into barracks");
				o.unitId2 = store_.cargo;
				out.push_back (std::move (o));
			}
			else
			{
				if (cargo->isUnitMoving() || cargo->data.getSpeed() <= 0) return;
				auto path = toVector (cPathCalculator (*cargo, view, *building, true).calcPath());
				const std::size_t full = path.size();
				cutToTurn (*cargo, view, path);
				if (path.empty()) return;
				cOrder o = makeOrder (eOrderType::Move, playerId, store_.cargo, "scenario: walk into barracks");
				o.path = path;
				o.position = path.back();
				if (path.size() == full)
				{
					o.unitId2 = store_.building;
					o.moveEnd = eMoveEnd::GetIn;
				}
				out.push_back (std::move (o));
				if (path.size() != full) return; // дойдёт в следующем ходу
			}
			store_.passes = {pass};
			store_.state = 3;
			wantPass_ = true;
			return;
		}
		case 3: // выйти
		{
			if (!cargo->isUnitLoaded())
			{
				step ("store_building", {{"what", "не вошла — снова"}, {"pass", pass}});
				store_.state = 2;
				return;
			}
			for (int dy = -1; dy <= 2; ++dy)
				for (int dx = -1; dx <= 2; ++dx)
				{
					const cPosition p = building->getPosition() + cPosition (dx, dy);
					if (!map.isValidPosition (p) || !building->canExitTo (p, map, cargo->getStaticUnitData())) continue;
					cOrder o = makeOrder (eOrderType::ActivateUnit, playerId, store_.building, "scenario: leave barracks");
					o.unitId2 = store_.cargo;
					o.position = p;
					out.push_back (std::move (o));
					store_.passes.push_back (pass);
					store_.state = 4;
					wantPass_ = true;
					return;
				}
			return fail ("из казармы некуда выйти");
		}
		case 4:
		{
			const bool out_ = !cargo->isUnitLoaded() && building->isNextTo (cargo->getPosition());
			report ("store_building", out_, {{"building", store_.building}, {"cargo", store_.cargo}, {"passes", store_.passes},
				{"checkPass", pass}, {"cargoAt", posJson (cargo->getPosition())}});
			store_.done = true;
			return;
		}
		default: return;
	}
}

// --- коммандос: выключить цель (ничейную технику чужих или здание врага) -------------------
void cScenarioBrain::commando (sCommando& c, int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	if (c.done) return;
	const cPlayer& player = *model.getPlayer (playerId);
	if (c.id == kNoUnit)
	{
		// первая (меньший id) — к чужим, вторая — к врагу
		const cVehicle* first = findOwn (player, "commando");
		const cVehicle* pick = (c.neutral || c.infiltrator) ? first : (first ? findOwn (player, "commando", first->getId()) : nullptr);
		if (pick == nullptr)
		{
			report (c.check, false, {{"why", "нет коммандос"}});
			c.done = true;
			return;
		}
		c.id = pick->getId();
	}
	const cVehicle* cmd = model.getVehicleFromID (c.id);
	if (cmd == nullptr)
	{
		// диверсант против правил: погиб после того, как его обнаружили, — правила его убили
		if (c.infiltrator)
			report (c.check, c.detectedTurn >= 0, {{"detectedTurn", c.detectedTurn}, {"attempts", c.attempts}, {"successes", c.successes},
				{"why", c.detectedTurn >= 0 ? "обнаружен и убит" : "погиб, не будучи обнаружен"}});
		else
			report (c.check, false, {{"why", "коммандос погиб"}, {"attempts", c.attempts}});
		c.done = true;
		return;
	}
	const auto& data = cmd->getCommandoData();
	if (c.pending) // приказ отдан в прошлом заходе: смотреть итог
	{
		c.pending = false;
		const cUnit* target = model.getUnitFromID (c.target);
		const bool success = target != nullptr && target->isDisabled();
		std::vector<int> detectedBy;
		for (const auto& p : model.getPlayerList())
			if (p.get() != cmd->getOwner() && cmd->isDetectedByPlayer (p.get())) detectedBy.push_back (p->getId());
		nlohmann::json d = {{"attempt", c.attempts}, {"target", c.target},
			{"targetType", target ? target->getStaticUnitData().getDefaultName() : std::string ("?")},
			{"targetOwner", target && target->getOwner() ? target->getOwner()->getId() : -1}, {"chance", c.chance},
			{"success", success}, {"disabledTurns", target ? target->getDisabledTurns() : 0}, {"detectedBy", detectedBy},
			{"successes", data.getSuccessCount()}, {"rank", cCommandoData::getLevel (data.getSuccessCount())}};
		step (c.check, d);
		if (c.infiltrator) // диверсант против правил: выключать до обнаружения; итог — когда его убьют
		{
			if (success) ++c.successes;
			if (!detectedBy.empty() && c.detectedTurn < 0) c.detectedTurn = turn_;
			if (c.attempts >= 60)
			{
				d["why"] = "за 60 попыток не убит";
				report (c.check, false, d);
				c.done = true;
			}
			return;
		}
		if (c.neutral && !detectedBy.empty())
		{
			d["why"] = "неудача у ничейной цели подняла тревогу";
			report (c.check, false, d);
			c.done = true;
		}
		else if (success)
		{
			report (c.check, true, d);
			c.done = true;
		}
		else if (c.attempts >= 8)
		{
			d["why"] = "8 неудач подряд";
			report (c.check, false, d);
			c.done = true;
		}
		return;
	}
	// цель: ближайшая к коммандос, которую можно выключить
	const cUnit* target = c.target != kNoUnit ? model.getUnitFromID (c.target) : nullptr;
	if (target != nullptr && (target->isDisabled() || (c.neutral && target->getOwner() != nullptr))) target = nullptr;
	if (c.infiltrator && c.detectedTurn >= 0) return; // обнаружен — стоять и ждать огня (это и проверяется)
	if (c.infiltrator) target = nullptr; // диверсант выбирает цель каждый раз: здание под охраной
	if (target == nullptr)
	{
		long long best = std::numeric_limits<long long>::max();
		const auto consider = [&] (const cUnit& u) {
			if (!u.getStaticUnitData().canBeDisabled || u.isDisabled()) return;
			if (u.isABuilding() && static_cast<const cBuilding&> (u).isRubble()) return;
			if (u.isAVehicle() && u.getStaticUnitData().factorAir > 0) return; // самолёт в воздухе не выключить
			// диверсант против правил — только здание, у которого рядом (до 5 клеток) вооружённая машина
			// врага: неудача должна поднять тревогу там, где есть кому стрелять
			if (c.infiltrator)
			{
				bool guarded = false;
				for (const auto& v : u.getOwner()->getVehicles())
					guarded = guarded || (v->getStaticUnitData().canAttack > 0 && v->data.getShotsMax() > 0 && !v->isUnitLoaded()
						&& (v->getPosition() - u.getPosition()).l2NormSquared() <= 25);
				if (!guarded) return;
			}
			const long long d = (u.getPosition() - cmd->getPosition()).l2NormSquared() * 100000 + u.getId();
			if (d < best)
			{
				best = d;
				target = &u;
			}
		};
		if (c.neutral)
		{
			for (const auto& v : model.getNeutralVehicles()) consider (*v);
		}
		else
			for (const auto& p : model.getPlayerList())
				if (p.get() != &player)
					for (const auto& b : p->getBuildings()) consider (*b);
		if (target == nullptr && c.infiltrator)
		{
			if (turn_ > 150)
			{
				report (c.check, false, {{"why", "за 150 ходов нет здания под охраной"}});
				c.done = true;
			}
			return; // ждать, пока у врага появится охрана
		}
		if (target == nullptr)
		{
			report (c.check, false, {{"why", "нет цели"}});
			c.done = true;
			return;
		}
		c.target = target->getId();
	}
	if (cCommandoData::canDoAction (*cmd, target, false))
	{
		c.chance = data.computeChance (target, false);
		++c.attempts;
		c.pending = true;
		cOrder o = makeOrder (eOrderType::Disable, playerId, c.id, "scenario: disable");
		o.unitId2 = c.target;
		out.push_back (std::move (o));
		wantPass_ = true;
		return;
	}
	// подойти к цели (коммандос стреляет на ходу: подошёл — в следующем заходе выключит)
	if (cmd->data.getSpeed() <= 0 || cmd->isUnitMoving()) return;
	auto path = toVector (cPathCalculator (*cmd, view, *target, true).calcPath());
	cutToTurn (*cmd, view, path);
	if (path.empty()) return;
	cOrder o = makeOrder (eOrderType::Move, playerId, c.id, "scenario: commando to target");
	o.path = path;
	o.position = path.back();
	out.push_back (std::move (o));
	wantPass_ = true;
}

// --- дозор: второй танк игрока 0 встаёт в дозор ---------------------------------------------
void cScenarioBrain::sentry (int playerId, const cModel& model, int pass, std::vector<cOrder>& out)
{
	if (sentryDone_) return;
	const cPlayer& player = *model.getPlayer (playerId);
	if (sentryTank_ == kNoUnit)
	{
		const cVehicle* first = findOwn (player, "tank");
		const cVehicle* second = first ? findOwn (player, "tank", first->getId()) : nullptr;
		if (second == nullptr)
		{
			report ("sentry_on", false, {{"why", "нет второго танка"}});
			sentryDone_ = true;
			return;
		}
		sentryTank_ = second->getId();
	}
	const cVehicle* tank = model.getVehicleFromID (sentryTank_);
	if (tank == nullptr)
	{
		report ("sentry_on", false, {{"why", "танк погиб"}});
		sentryDone_ = true;
		return;
	}
	if (tank->isSentryActive())
	{
		report ("sentry_on", true, {{"tank", sentryTank_}, {"at", posJson (tank->getPosition())}, {"pass", pass}});
		sentryDone_ = true;
		return;
	}
	if (pass != 1) return;
	out.push_back (makeOrder (eOrderType::ToggleSentry, playerId, sentryTank_, "scenario: sentry on"));
	wantPass_ = true;
}

// --- игрок 1: геолог под огонь дозорного (ответный огонь безоружного не трогает — только дозор) ---
void cScenarioBrain::victim (int playerId, const cModel& model, const cMapView& view, int pass, std::vector<cOrder>& out)
{
	const cPlayer& player = *model.getPlayer (playerId);
	// против правил (rule,scenario) у игрока 1 — коммандос: выключает здания врага, пока его не обнаружат
	if (findOwn (player, "commando") != nullptr || infil_.id != kNoUnit) commando (infil_, playerId, model, view, pass, out);
	if (victimDone_) return;
	// дозорный врага (тесту видна вся модель)
	const cVehicle* sentryUnit = nullptr;
	for (const auto& p : model.getPlayerList())
		if (p.get() != &player)
			for (const auto& v : p->getVehicles())
				if (v->isSentryActive() && (sentryUnit == nullptr || v->getId() < sentryUnit->getId())) sentryUnit = v.get();
	if (victimId_ == kNoUnit)
	{
		const cVehicle* s = findOwn (player, "surveyor");
		if (s == nullptr)
		{
			report ("sentry_fire", false, {{"why", "нет геолога"}});
			victimDone_ = true;
			return;
		}
		victimId_ = s->getId();
	}
	const cVehicle* s = model.getVehicleFromID (victimId_);
	if (victimTurn_ == turn_ && pass >= 2) // в прошлом заходе ехали к дозорному — попал ли он
	{
		const int hp = s ? s->data.getHitpoints() : 0;
		nlohmann::json d = {{"surveyor", victimId_}, {"hpBefore", victimHp_}, {"hpAfter", hp}, {"destroyed", s == nullptr},
			{"sentry", sentryUnit ? static_cast<int> (sentryUnit->getId()) : -1}, {"pass", pass}};
		if (s == nullptr || hp < victimHp_)
		{
			if (sentryUnit) d["sentryShotsLeft"] = sentryUnit->data.getShots();
			report ("sentry_fire", true, d);
			victimDone_ = true;
		}
		else
			step ("sentry_fire", d);
		return;
	}
	if (s == nullptr)
	{
		// дозорного у врага нет (игрок 0 — не сценарий, например rule) — проверки дозора в партии нет
		if (sentryUnit != nullptr) report ("sentry_fire", false, {{"why", "геолог погиб раньше"}});
		victimDone_ = true;
		return;
	}
	if (sentryUnit == nullptr || pass != 1) return; // ждать, пока игрок 0 поставит дозор
	auto path = toVector (cPathCalculator (*s, view, *sentryUnit, true).calcPath());
	cutToTurn (*s, view, path);
	if (path.empty()) return;
	victimHp_ = s->data.getHitpoints();
	victimTurn_ = turn_;
	cOrder o = makeOrder (eOrderType::Move, playerId, victimId_, "scenario: surveyor walks to the sentry");
	o.path = path;
	o.position = path.back();
	out.push_back (std::move (o));
	wantPass_ = true;
	step ("sentry_fire", {{"what", "геолог идёт к дозорному"}, {"to", posJson (path.back())}, {"sentry", sentryUnit->getId()}});
}

} // namespace ai
