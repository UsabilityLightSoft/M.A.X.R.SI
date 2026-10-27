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

#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"

#include <doctest.h>

//------------------------------------------------------------------------------
// The unit window shows the status text of the selected vehicle and refreshes it on statusChanged:
// starting or stopping to build has to signal it, for vehicles created in a game
// as well as for loaded ones (they are created by different constructors).
TEST_CASE ("VehicleSignalsStatusChangeOnBuilding")
{
	const cStaticUnitData staticData;
	const cDynamicUnitData dynamicData;
	cVehicle createdInGame (staticData, dynamicData, nullptr, 1);
	cVehicle loaded (2);

	for (cVehicle* vehicle : {&createdInGame, &loaded})
	{
		int statusChanges = 0;
		vehicle->statusChanged.connect ([&]() { ++statusChanges; });

		vehicle->setBuildingABuilding (true);
		CHECK (statusChanges == 1);

		vehicle->setBuildingABuilding (false);
		CHECK (statusChanges == 2);

		vehicle->setClearing (true);
		CHECK (statusChanges == 3);

		vehicle->setLayMines (true);
		CHECK (statusChanges == 4);

		vehicle->setClearMines (true);
		CHECK (statusChanges == 5);

		const sID buildingType (1, 7);
		vehicle->setBuildingType (buildingType);
		CHECK (statusChanges == 6);
		CHECK (vehicle->getBuildingType() == buildingType);

		vehicle->setBuildingType (buildingType); // unchanged
		CHECK (statusChanges == 6);
	}
}
