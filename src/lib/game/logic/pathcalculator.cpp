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

#include "pathcalculator.h"

#include "game/data/map/mapfieldview.h"
#include "game/data/map/mapview.h"
#include "game/data/units/unit.h"
#include "game/data/units/unitdata.h"
#include "game/data/units/vehicle.h"
#include "utility/mathtools.h"
#include "utility/narrow_cast.h"
#include "utility/ranges.h"

#include <cassert>
#include <forward_list>

namespace
{
	//--------------------------------------------------------------------------
	// Reusable per-thread scratch buffers for pathfinding.
	//
	// The old code allocated three flat W*H arrays of pointers AND a fresh
	// vector<vector<sPathNode>> pool (one 10-node heap allocation per block) on
	// EVERY cPathCalculator::calcPath() call. On a crowded turn the engine calls
	// calcPath() hundreds/thousands of times (recalculatePath runs a full
	// Dijkstra over the map for every blocked unit), so that malloc/free churn
	// plus the 3 large memset()s were a major cost.
	//
	// These buffers are thread_local so that the server thread (move jobs) and
	// the driver thread (AI brain) each keep their own — no data race, and the
	// results are bit-identical: the buffers are pure storage, never part of the
	// algorithm's state.
	struct PathScratch
	{
		/* heaplist where all nodes are sorted by their costF value */
		std::vector<sPathNode*> nodesHeap;
		/* open nodes map (indexed by map offset) */
		std::vector<sPathNode*> openList;
		/* closed nodes map (indexed by map offset) */
		std::vector<sPathNode*> closedList;
		/* single contiguous node pool; grows once to W*H+1 and is reused */
		std::vector<sPathNode> nodePool;
		/* number of nodes handed out of nodePool in the current calcPath() */
		std::size_t poolUsed = 0;
	};
	thread_local PathScratch pathScratch;

	//--------------------------------------------------------------------------
	void swapHeapNodes (int a, int b)
	{
		auto& heap = pathScratch.nodesHeap;
		std::swap (heap[a], heap[b]);
		heap[a]->heapIndex = a;
		heap[b]->heapIndex = b;
	}
} // namespace

//------------------------------------------------------------------------------
cPathDestHandler::cPathDestHandler (ePathDestinationType type_, const cPosition& destination_, const cVehicle* srcVehicle_, const cUnit* destUnit_) :
	type (type_),
	srcVehicle (srcVehicle_),
	destUnit (destUnit_),
	destination (destination_)
{}

//------------------------------------------------------------------------------
bool cPathDestHandler::hasReachedDestination (const cPosition& position) const
{
	switch (type)
	{
		case ePathDestinationType::Pos:
			return (destination == position);
		case ePathDestinationType::Load:
			return (destUnit && destUnit->isNextTo (position));
		case ePathDestinationType::Attack:
			return (position - destination).l2NormSquared() <= Square (srcVehicle->data.getRange());
		default:
			return true;
	}
}

//------------------------------------------------------------------------------
int cPathDestHandler::heuristicCost (const cPosition& source) const
{
	switch (type)
	{
		case ePathDestinationType::Pos:
		case ePathDestinationType::Load:
			return 0;
		case ePathDestinationType::Attack:
		default:
		{
			return Round (narrow_cast<float> ((destination - source).l2Norm()));
		}
	}
}

//------------------------------------------------------------------------------
cPathCalculator::cPathCalculator (const cVehicle& Vehicle, const cMapView& Map, const cPosition& destPosition, const std::vector<cVehicle*>* group)
{
	destHandler = std::make_unique<cPathDestHandler> (ePathDestinationType::Pos, destPosition, nullptr, nullptr);
	init (Vehicle.getPosition(), Map, Vehicle, group);
}

//------------------------------------------------------------------------------
cPathCalculator::cPathCalculator (const cVehicle& Vehicle, const cMapView& Map, const cUnit& destUnit, bool load)
{
	destHandler = std::make_unique<cPathDestHandler> (load ? ePathDestinationType::Load : ePathDestinationType::Attack, cPosition (0, 0), &Vehicle, &destUnit);
	init (Vehicle.getPosition(), Map, Vehicle, nullptr);
}

//------------------------------------------------------------------------------
cPathCalculator::cPathCalculator (const cVehicle& Vehicle, const cMapView& Map, const cPosition& destPosition, bool attack)
{
	destHandler = std::make_unique<cPathDestHandler> (attack ? ePathDestinationType::Attack : ePathDestinationType::Pos, destPosition, &Vehicle, nullptr);
	init (Vehicle.getPosition(), Map, Vehicle, nullptr);
}

//------------------------------------------------------------------------------
void cPathCalculator::init (const cPosition& source, const cMapView& Map, const cVehicle& Vehicle, const std::vector<cVehicle*>* group)
{
	this->source = source;
	this->Map = &Map;
	this->Vehicle = &Vehicle;
	this->group = group;
	bPlane = Vehicle.getStaticUnitData().factorAir > 0;
	bShip = Vehicle.getStaticUnitData().factorSea > 0 && Vehicle.getStaticUnitData().factorGround == 0;

	heapCount = 0;
}

//------------------------------------------------------------------------------
cPathCalculator::~cPathCalculator()
{
}

//------------------------------------------------------------------------------
std::forward_list<cPosition> cPathCalculator::calcPath()
{
	std::forward_list<cPosition> path;

	// generate open and closed list (reused scratch buffers, cleared each run)
	const int mapCells = Map->getSize().x() * Map->getSize().y();
	auto& nodesHeap = pathScratch.nodesHeap;
	auto& openList = pathScratch.openList;
	auto& closedList = pathScratch.closedList;
	auto& nodePool = pathScratch.nodePool;

	nodesHeap.assign (mapCells + 1, nullptr);
	openList.assign (mapCells + 1, nullptr);
	closedList.assign (mapCells + 1, nullptr);
	// reserve space for the maximum possible node count; never reallocates
	// during the search, so node 'prev' pointers into the pool stay valid.
	if (nodePool.size() < static_cast<std::size_t> (mapCells + 1))
		nodePool.resize (mapCells + 1);
	pathScratch.poolUsed = 0;

	// generate startnode
	sPathNode* StartNode = allocNode();
	StartNode->position = source;
	StartNode->costG = 0;
	StartNode->costH = destHandler->heuristicCost (source);
	StartNode->costF = StartNode->costG + StartNode->costH;

	StartNode->prev = nullptr;
	openList[Map->getOffset (source)] = StartNode;
	insertToHeap (StartNode, false);

	while (heapCount > 0)
	{
		// get the node with the lowest F value
		sPathNode* CurrentNode = nodesHeap[1];

		// move it from the open to the closed list
		openList[Map->getOffset (CurrentNode->position)] = nullptr;
		closedList[Map->getOffset (CurrentNode->position)] = CurrentNode;
		deleteFirstFromHeap();

		// generate waypoints when destination has been reached
		if (destHandler->hasReachedDestination (CurrentNode->position))
		{
			sPathNode* pathNode = CurrentNode;

			cPosition waypoint;
			while (pathNode->prev != nullptr)
			{
				waypoint = pathNode->position;
				path.push_front (waypoint);

				pathNode = pathNode->prev;
			}

			return path;
		}

		// expand node
		expandNodes (CurrentNode);
	}

	// there is no path to the destination field
	return path;
}

//------------------------------------------------------------------------------
void cPathCalculator::expandNodes (sPathNode* ParentNode)
{
	auto& openList = pathScratch.openList;
	auto& closedList = pathScratch.closedList;

	// add all nearby nodes
	const int minx = std::max (ParentNode->position.x() - 1, 0);
	const int maxx = std::min (ParentNode->position.x() + 1, Map->getSize().y() - 1);
	const int miny = std::max (ParentNode->position.y() - 1, 0);
	const int maxy = std::min (ParentNode->position.y() + 1, Map->getSize().y() - 1);

	for (int y = miny; y <= maxy; ++y)
	{
		for (int x = minx; x <= maxx; ++x)
		{
			const cPosition currentPosition (x, y);
			if (currentPosition == ParentNode->position) continue;

			if (!Map->possiblePlace (*Vehicle, currentPosition))
			{
				// when we have a group of units, the units will not block each other
				if (group)
				{
					const auto& field = Map->getField (currentPosition);
					// get the blocking unit
					cVehicle* blockingUnit = (Vehicle->getStaticUnitData().factorAir > 0) ? field.getPlane() : field.getVehicle();
					// check whether the blocking unit is the group
					bool isInGroup = ranges::contains (*group, blockingUnit);
					if (!isInGroup) continue;
				}
				else
					continue;
			}
			const int offset = Map->getOffset (currentPosition);
			if (closedList[offset] != nullptr) continue;

			if (openList[offset] == nullptr)
			{
				// generate new node
				sPathNode* NewNode = allocNode();
				NewNode->position = currentPosition;
				NewNode->costG = calcNextCost (ParentNode->position, currentPosition, Vehicle, Map) + ParentNode->costG;
				NewNode->costH = destHandler->heuristicCost (currentPosition);
				NewNode->costF = NewNode->costG + NewNode->costH;
				NewNode->prev = ParentNode;
				openList[offset] = NewNode;
				insertToHeap (NewNode, false);
			}
			else
			{
				// modify existing node
				int costG, costH, costF;
				costG = calcNextCost (ParentNode->position, currentPosition, Vehicle, Map) + ParentNode->costG;
				costH = destHandler->heuristicCost (currentPosition);
				costF = costG + costH;
				if (costF < openList[offset]->costF)
				{
					openList[offset]->costG = costG;
					openList[offset]->costH = costH;
					openList[offset]->costF = costF;
					openList[offset]->prev = ParentNode;
					insertToHeap (openList[offset], true);
				}
			}
		}
	}
}

//------------------------------------------------------------------------------
sPathNode* cPathCalculator::allocNode()
{
	// nodes are handed out of a single pre-sized pool; the pool is sized to the
	// whole map up front in calcPath(), so these pointers never dangle.
	assert (pathScratch.poolUsed < pathScratch.nodePool.size());
	return &pathScratch.nodePool[pathScratch.poolUsed++];
}

//------------------------------------------------------------------------------
void cPathCalculator::insertToHeap (sPathNode* Node, bool exists)
{
	auto& nodesHeap = pathScratch.nodesHeap;
	int i = 0;
	if (exists)
	{
		// O(1) lookup of the node's position (previously O(N) linear scan)
		i = Node->heapIndex;
		assert (i >= 1 && i <= heapCount);
		assert (nodesHeap[i] == Node);
	}
	else
	{
		// add the new node in the end
		heapCount++;
		nodesHeap[heapCount] = Node;
		Node->heapIndex = heapCount;
		i = heapCount;
	}
	// resort the nodes
	while (i > 1)
	{
		assert (nodesHeap[i] == Node);
		if (Node->costF < nodesHeap[i / 2]->costF)
		{
			swapHeapNodes (i / 2, i);
			i = i / 2;
		}
		else
			break;
	}
}

//------------------------------------------------------------------------------
void cPathCalculator::deleteFirstFromHeap()
{
	auto& nodesHeap = pathScratch.nodesHeap;
	// overwrite the first node by the last one
	nodesHeap[1] = nodesHeap[heapCount];
	nodesHeap[1]->heapIndex = 1;
	nodesHeap[heapCount] = nullptr;
	heapCount--;
	int v = 1;
	while (true)
	{
		int u = v;
		if (2 * u + 1 <= heapCount) // both children in the heap exists
		{
			if (nodesHeap[u]->costF >= nodesHeap[u * 2]->costF) v = 2 * u;
			if (nodesHeap[v]->costF >= nodesHeap[u * 2 + 1]->costF) v = 2 * u + 1;
		}
		else if (2 * u <= heapCount) // only one children exists
		{
			if (nodesHeap[u]->costF >= nodesHeap[u * 2]->costF) v = 2 * u;
		}
		// do the resort
		if (u != v)
		{
			swapHeapNodes (u, v);
		}
		else
			break;
	}
}
