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

#ifndef game_logic_pathcalculatorH
#define game_logic_pathcalculatorH

#include "game/data/map/map.h"
#include "game/data/units/building.h"
#include "game/data/units/vehicle.h"
#include "utility/position.h"

#include <forward_list>

class cVehicle;
class cUnit;
class cMapView;

/* node structure for pathfinding */
struct sPathNode
{
	/* x and y coords */
	cPosition position;
	/* the different cost types */
	int costF = 0;
	int costG = 0;
	int costH = 0;
	/* previous node of this one in the hole path */
	sPathNode* prev = nullptr;
	/* current index in the heap; 0 = not in heap. Lets insertToHeap find the
	 * node in O(1) instead of scanning the whole heap (O(N)) on every cost
	 * update. Purely an accelerator for the heap bookkeeping — the resulting
	 * heap ordering is identical. */
	int heapIndex = 0;
};

enum class ePathDestinationType
{
	Pos,
	Load,
	Attack
};

class cPathDestHandler
{
	ePathDestinationType type;

	const cVehicle* srcVehicle = nullptr;

	const cUnit* destUnit = nullptr;
	cPosition destination;

public:
	cPathDestHandler (ePathDestinationType, const cPosition& destination, const cVehicle* srcVehicle, const cUnit* destUnit);

	bool hasReachedDestination (const cPosition& position) const;
	int heuristicCost (const cPosition& source) const;
};

class cPathCalculator
{
	void init (const cPosition& source, const cMapView&, const cVehicle&, const std::vector<cVehicle*>* group);

public:
	cPathCalculator (const cVehicle&, const cMapView&, const cPosition& destPosition, const std::vector<cVehicle*>* group);
	cPathCalculator (const cVehicle&, const cMapView&, const cUnit& destUnit, bool load);
	cPathCalculator (const cVehicle&, const cMapView&, const cPosition& destPosition, bool attack);
	~cPathCalculator();

	/**
	* calculates the best path in costs and length
	*@author alzi alias DoctorDeath
	*/
	std::forward_list<cPosition> calcPath();

	/**
	* calculates the costs for moving from the source- to the destinationfield
	*@author alzi alias DoctorDeath
	*/
	template <typename T>
	static int calcNextCost (const cPosition& source, const cPosition& destination, const cVehicle*, const T* map);

	/* the map on which the path will be calculated */
	const cMapView* Map = nullptr;
	/* the moving vehicle */
	const cVehicle* Vehicle = nullptr;
	/* if more then one vehicle is moving in a group this is the list of all moving vehicles */
	const std::vector<cVehicle*>* group = nullptr;
	/* source and destination coords */
	cPosition source;
	bool bPlane = false;
	bool bShip = false;
	std::unique_ptr<cPathDestHandler> destHandler;

private:
	/* number of nodes saved on the heaplist; equal to number of nodes in the openlist */
	int heapCount = 0;
	/**
	* expands the nodes around the overgiven one
	*@author alzi alias DoctorDeath
	*/
	void expandNodes (sPathNode* Node);
	/**
	* returns a pointer to allocated memory and allocates a new block in memory if necessary
	*@author alzi alias DoctorDeath
	*/
	sPathNode* allocNode();
	/**
	* inserts a node into the heaplist and sets it to the right position by its costF value.
	*@author alzi alias DoctorDeath
	*/
	void insertToHeap (sPathNode* Node, bool exists);
	/**
	* deletes the first node in the heaplist and resorts the rest.
	*@author alzi alias DoctorDeath
	*/
	void deleteFirstFromHeap();
};

template <typename T>
int cPathCalculator::calcNextCost (const cPosition& source, const cPosition& destination, const cVehicle* vehicle, const T* map)
{
	static_assert (std::is_same<T, cMap>::value || std::is_same<T, cMapView>::value, "Type must be cMap or cMapView");

	int costs = 4;
	// select base movement factor
	const auto& unitData = vehicle->getStaticUnitData();
	// fetch the destination field once; the field view construction (cMapView)
	// used to be repeated up to three times per call.
	const auto& field = map->getField (destination);
	if (unitData.factorAir > 0)
	{
		costs = (int) (4 * unitData.factorAir);
	}
	else if (map->isWater (destination) && !(field.hasBridgeOrPlatform() && unitData.factorGround > 0))
	{
		costs = (int) (4 * unitData.factorSea);
	}
	else if (map->isCoast (destination) && !(field.hasBridgeOrPlatform() && unitData.factorGround > 0))
	{
		costs = (int) (4 * unitData.factorCoast);
	}
	else
	{
		costs = (int) (4 * unitData.factorGround);
	}

	// moving on a road is cheaper
	// assuming, only speed of ground units can be modified
	const cBuilding* building = field.getBaseBuilding();
	if (building && building->getStaticData().modifiesSpeed != 0 && unitData.factorGround > 0)
	{
		costs = (int) (costs * building->getStaticData().modifiesSpeed);
	}

	// multiply with the factor 1.5 for diagonal movements
	if (source.x() != destination.x() && source.y() != destination.y())
	{
		costs = (int) (costs * 1.5f);
	}

	return costs;
}

#endif //game_logic_pathcalculatorH
