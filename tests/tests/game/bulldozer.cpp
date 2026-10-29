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

// The bulldozer chain: a bulldozer clears rubble, the metal of the rubble goes into its cargo, the field can be built on again,
// the metal is transferred to a repair vehicle and to a supply truck, which repair and rearm a tank with it.

#include "game/data/gamesettings.h"
#include "game/data/map/map.h"
#include "game/data/model.h"
#include "game/data/player/player.h"
#include "game/data/player/playerbasicdata.h"
#include "game/data/resourcetype.h"
#include "game/data/units/building.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "game/logic/action/actionclear.h"
#include "game/logic/action/actionrepairreload.h"
#include "game/logic/action/actiontransfer.h"

#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

namespace
{
	const sID bulldozerId{0, 110};
	const sID repairId{0, 111};
	const sID supplyId{0, 112};
	const sID tankId{0, 113};
	const sID buildingId{1, 110};

	//--------------------------------------------------------------------------
	void writeLE16 (std::ofstream& file, int value)
	{
		file.put (static_cast<char> (value & 0xFF));
		file.put (static_cast<char> ((value >> 8) & 0xFF));
	}

	//--------------------------------------------------------------------------
	// writes a map file (WRL format) of plain ground, with one terrain
	std::filesystem::path writeGroundMap (int size)
	{
		const auto filename = std::filesystem::temp_directory_path() / ("maxr_tests_" + std::to_string (std::random_device{}()) + ".wrl");
		std::ofstream file (filename, std::ios::binary);
		file.write ("WRL", 3);
		writeLE16 (file, 1);
		writeLE16 (file, size);
		writeLE16 (file, size);
		file << std::string (size * size, '\0'); // mini map
		file << std::string (size * size * 2, '\0'); // all fields use terrain 0
		writeLE16 (file, 1); // number of terrains
		file << std::string (64 * 64, '\0'); // terrain graphic
		file << std::string (256 * 3, '\0'); // palette
		file.put (0); // terrain 0: ground
		return filename;
	}

	//--------------------------------------------------------------------------
	cStaticUnitData groundVehicle (const sID& id)
	{
		cStaticUnitData data;
		data.ID = id;
		data.factorGround = 1.f;
		data.surfacePosition = eSurfacePosition::Ground;
		return data;
	}

	//--------------------------------------------------------------------------
	cDynamicUnitData dynamicData (const sID& id, int hitpoints, int buildCost, int ammo = 0)
	{
		cDynamicUnitData data;
		data.setId (id);
		data.setHitpointsMax (hitpoints);
		data.setBuildCost (buildCost);
		data.setSpeedMax (4 * 7);
		data.setScan (3);
		data.setAmmoMax (ammo);
		data.setShotsMax (ammo > 0 ? 2 : 0);
		data.setDamage (ammo > 0 ? 10 : 0);
		data.setRange (ammo > 0 ? 3 : 0);
		data.setMaximumCurrentValues();
		return data;
	}

	//--------------------------------------------------------------------------
	// as in data/vehicles: bulldozer, repair, trans_metal (rearms), tank; a small building
	std::shared_ptr<cUnitsData> makeUnitsData()
	{
		auto unitsData = std::make_shared<cUnitsData>();

		cStaticUnitData bulldozer = groundVehicle (bulldozerId);
		bulldozer.vehicleData.canClearArea = true;
		bulldozer.storeResType = eResourceType::Metal;
		bulldozer.storageResMax = 30;
		unitsData->addData (bulldozer);
		unitsData->addData (dynamicData (bulldozerId, 14, 15));

		cStaticUnitData repair = groundVehicle (repairId);
		repair.canRepair = true;
		repair.storeResType = eResourceType::Metal;
		repair.storageResMax = 30;
		unitsData->addData (repair);
		unitsData->addData (dynamicData (repairId, 20, 15));

		cStaticUnitData supply = groundVehicle (supplyId);
		supply.canRearm = true;
		supply.storeResType = eResourceType::Metal;
		supply.storageResMax = 50;
		unitsData->addData (supply);
		unitsData->addData (dynamicData (supplyId, 24, 12));

		cStaticUnitData tank = groundVehicle (tankId);
		tank.canAttack = eTerrainFlag::Ground;
		unitsData->addData (tank);
		unitsData->addData (dynamicData (tankId, 24, 12, 14));

		cStaticUnitData building;
		building.ID = buildingId;
		building.factorGround = 1.f;
		building.surfacePosition = eSurfacePosition::Ground;
		unitsData->addData (building);
		cDynamicUnitData buildingDynamic;
		buildingDynamic.setId (buildingId);
		buildingDynamic.setHitpointsMax (10);
		buildingDynamic.setBuildCost (8);
		buildingDynamic.setMaximumCurrentValues();
		unitsData->addData (buildingDynamic);

		return unitsData;
	}

	//--------------------------------------------------------------------------
	// a map of 16x16 fields of ground, the units above and one player
	void initModel (cModel& model)
	{
		auto staticMap = std::make_shared<cStaticMap>();
		const auto mapFilename = writeGroundMap (16);
		REQUIRE (staticMap->loadMap (mapFilename));
		std::filesystem::remove (mapFilename);

		model.setUnitsData (makeUnitsData());
		model.setGameSettings (cGameSettings{});
		model.setMap (staticMap);
		model.setPlayerList ({cPlayerBasicData ({"Player 0", cRgbColor::red()}, 0, false)});
	}

	//--------------------------------------------------------------------------
	void startClearing (cModel& model, const cVehicle& bulldozer)
	{
		cActionClear action (bulldozer);
		action.From (bulldozer.getOwner()->getId());
		action.execute (model);
	}

	//--------------------------------------------------------------------------
	void transfer (cModel& model, const cUnit& source, const cUnit& destination, int value)
	{
		cActionTransfer action (source, destination, value, eResourceType::Metal);
		action.From (source.getOwner()->getId());
		action.execute (model);
	}

	//--------------------------------------------------------------------------
	void supply (cModel& model, const cUnit& source, const cUnit& destination, eSupplyType type)
	{
		cActionRepairReload action (source, destination, type);
		action.From (source.getOwner()->getId());
		action.execute (model);
	}
} // namespace

//------------------------------------------------------------------------------
TEST_CASE ("BulldozerClearsRubbleTheMetalGoesIntoItsCargoAndTheFieldCanBeBuiltOnAgain")
{
	cModel model;
	initModel (model);
	cPlayer& player = *model.getPlayer (0);
	const cMap& map = *model.getMap();
	const auto& buildingData = model.getUnitsData()->getStaticUnitData (buildingId);

	const cPosition field (5, 5);
	model.addRubble (field, 10, false);
	REQUIRE (map.getField (field).getRubble() != nullptr);
	CHECK (map.getField (field).getRubble()->getRubbleValue() == 10);
	// no building on rubble (cMap::possiblePlaceBuildingWithMines)
	CHECK_FALSE (map.possiblePlaceBuilding (buildingData, field, nullptr));

	// the bulldozer can drive onto the rubble
	cVehicle& bulldozer = model.addVehicle (cPosition (4, 5), bulldozerId, &player);
	CHECK (map.possiblePlace (bulldozer, field, false));
	model.getMap()->moveVehicle (bulldozer, field);
	REQUIRE (bulldozer.getPosition() == field);

	startClearing (model, bulldozer);
	CHECK (bulldozer.isUnitClearing());
	CHECK (bulldozer.getClearingTurns() == 1);
	CHECK (bulldozer.getStoredResources() == 0);

	player.makeTurnStart (model);

	CHECK_FALSE (bulldozer.isUnitClearing());
	CHECK (bulldozer.getStoredResources() == 10);
	CHECK (map.getField (field).getRubble() == nullptr);
	// the field can be built on again: by the vehicle standing there (a constructor would) and, when it has left, by anybody
	CHECK (map.possiblePlaceBuilding (buildingData, field, nullptr, &bulldozer));
	model.getMap()->moveVehicle (bulldozer, cPosition (4, 5));
	CHECK (map.possiblePlaceBuilding (buildingData, field, nullptr));
	model.addBuilding (field, buildingId, &player);
	CHECK (map.getField (field).getBuilding() != nullptr);
}

//------------------------------------------------------------------------------
TEST_CASE ("BulldozerClearsBigRubbleInFourTurnsAndTheCargoIsLimitedByItsStorage")
{
	cModel model;
	initModel (model);
	cPlayer& player = *model.getPlayer (0);
	const cMap& map = *model.getMap();

	const cPosition corner (8, 8);
	model.addRubble (corner, 44, true);
	REQUIRE (map.getField (corner + cPosition (1, 1)).getRubble() != nullptr);

	cVehicle& bulldozer = model.addVehicle (corner + cPosition (1, 0), bulldozerId, &player);
	bulldozer.setStoredResources (5);
	startClearing (model, bulldozer);
	REQUIRE (bulldozer.isUnitClearing());
	CHECK (bulldozer.getClearingTurns() == 4);
	// the bulldozer stands on all four fields of the big rubble
	CHECK (bulldozer.getIsBig());
	CHECK (bulldozer.getPosition() == corner);

	for (int turn = 0; turn < 3; ++turn)
		player.makeTurnStart (model);
	CHECK (bulldozer.isUnitClearing());
	CHECK (map.getField (corner).getRubble() != nullptr);

	player.makeTurnStart (model);
	CHECK_FALSE (bulldozer.isUnitClearing());
	CHECK (map.getField (corner).getRubble() == nullptr);
	CHECK (map.getField (corner + cPosition (1, 1)).getRubble() == nullptr);
	// back on its field; 5 + 44 metal is more than its storage (30): the rest is lost (cUnit::setStoredResources)
	CHECK_FALSE (bulldozer.getIsBig());
	CHECK (bulldozer.getPosition() == corner + cPosition (1, 0));
	CHECK (bulldozer.getStoredResources() == 30);
}

//------------------------------------------------------------------------------
TEST_CASE ("MetalOfTheBulldozerRepairsAndRearmsOwnUnitsThroughARepairVehicleAndASupplyTruck")
{
	cModel model;
	initModel (model);
	cPlayer& player = *model.getPlayer (0);

	// bulldozer (with the metal of rubble), repair vehicle and supply truck next to it, a tank next to both
	cVehicle& bulldozer = model.addVehicle (cPosition (5, 5), bulldozerId, &player);
	cVehicle& repair = model.addVehicle (cPosition (6, 5), repairId, &player);
	cVehicle& truck = model.addVehicle (cPosition (5, 6), supplyId, &player);
	cVehicle& tank = model.addVehicle (cPosition (6, 6), tankId, &player);
	bulldozer.setStoredResources (20);

	transfer (model, bulldozer, repair, 12);
	CHECK (bulldozer.getStoredResources() == 8);
	CHECK (repair.getStoredResources() == 12);

	transfer (model, bulldozer, truck, 8);
	CHECK (bulldozer.getStoredResources() == 0);
	CHECK (truck.getStoredResources() == 8);

	// no transfer from an empty bulldozer, no transfer of more than the destination can store
	transfer (model, bulldozer, truck, 1);
	CHECK (truck.getStoredResources() == 8);
	bulldozer.setStoredResources (30);
	transfer (model, bulldozer, repair, 30);
	CHECK (repair.getStoredResources() == 12);
	CHECK (bulldozer.getStoredResources() == 30);

	// the tank is damaged and out of ammo
	tank.data.setHitpoints (8);
	tank.data.setAmmo (0);

	// repair: 4 * 24 / 12 = 8 hitpoints per metal, 16 hitpoints missing - 2 metal
	supply (model, repair, tank, eSupplyType::REPAIR);
	CHECK (tank.data.getHitpoints() == 24);
	CHECK (repair.getStoredResources() == 10);

	// rearm: the full ammo for 1 metal
	supply (model, truck, tank, eSupplyType::REARM);
	CHECK (tank.data.getAmmo() == 14);
	CHECK (truck.getStoredResources() == 7);
}

//------------------------------------------------------------------------------
TEST_CASE ("BulldozerTransfersToVehiclesOfTheSameResourceOnlyAndNotWhileClearing")
{
	cModel model;
	initModel (model);
	cPlayer& player = *model.getPlayer (0);

	cVehicle& bulldozer = model.addVehicle (cPosition (5, 5), bulldozerId, &player);
	cVehicle& repair = model.addVehicle (cPosition (6, 5), repairId, &player);
	cVehicle& tank = model.addVehicle (cPosition (4, 5), tankId, &player); // stores nothing
	bulldozer.setStoredResources (10);

	transfer (model, bulldozer, tank, 5);
	CHECK (bulldozer.getStoredResources() == 10);

	// a clearing bulldozer gives nothing
	model.addRubble (cPosition (5, 5), 6, false);
	startClearing (model, bulldozer);
	REQUIRE (bulldozer.isUnitClearing());
	transfer (model, bulldozer, repair, 5);
	CHECK (repair.getStoredResources() == 0);

	player.makeTurnStart (model);
	REQUIRE_FALSE (bulldozer.isUnitClearing());
	CHECK (bulldozer.getStoredResources() == 16);
	transfer (model, bulldozer, repair, 16);
	CHECK (repair.getStoredResources() == 16);
	CHECK (bulldozer.getStoredResources() == 0);
}
