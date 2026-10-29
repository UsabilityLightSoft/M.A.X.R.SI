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

#ifndef ui_graphical_menu_windows_windowbuildvehicles_windowbuildvehiclesH
#define ui_graphical_menu_windows_windowbuildvehicles_windowbuildvehiclesH

#include "ui/graphical/menu/windows/windowadvancedhangar/windowadvancedhangar.h"

#include <memory>
#include <vector>

class cBuildListItem;
class cBuildSpeedHandlerWidget;
class cBuilding;
class cCheckBox;
class cLabel;
class cMapView;
class cToolTip;
class cTurnTimeClock;
class cUnitListViewItemBuild;
class cUnitsData;

class cWindowBuildVehicles : public cWindowAdvancedHangar<cUnitListViewItemBuild>
{
public:
	cWindowBuildVehicles (const cBuilding& building, const cMapView& map, std::shared_ptr<const cUnitsData> unitsData, std::shared_ptr<const cTurnTimeClock> turnTimeClock);

	void retranslate() override;

	std::vector<sID> getBuildList() const;
	int getSelectedBuildSpeed() const;
	bool isRepeatActive() const;

	void draw (SDL_Surface& destination, const cBox<cPosition>& clipRect) override;

protected:
	void setActiveUnit (const sID& unitId) override;
	/** With an empty queue "Done" does nothing: the list of vehicles is shown, that has to be used first. */
	void okClicked() override;

private:
	cSignalConnectionManager signalConnectionManager;

	const cBuilding& building;

	cBuildSpeedHandlerWidget* speedHandler = nullptr;

	cLabel* titleLabel = nullptr;
	cCheckBox* repeatCheckBox = nullptr;
	/** Why "Done" is not available: shown while the mouse is over it and for a moment after a click on it. */
	cToolTip* emptyQueueHint = nullptr;

	/** "Done" was clicked with an empty queue: the list of vehicles is highlighted for a moment (SDL ticks). */
	Uint32 emptyQueueClickTime = 0;
	bool emptyQueueClicked = false;

	void generateSelectionList (const cBuilding& building, const cMapView& map, const cUnitsData& unitsData);
	void generateBuildList (const cBuilding& building);

	void closeOnUnitDestruction();

	/** "Done" is available only with a vehicle in the queue (double click in the list adds one). */
	void updateDoneButton();
};

#endif // ui_graphical_menu_windows_windowbuildvehicles_windowbuildvehiclesH
