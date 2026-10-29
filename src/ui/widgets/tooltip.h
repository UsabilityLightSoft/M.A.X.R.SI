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

#ifndef ui_widgets_tooltipH
#define ui_widgets_tooltipH

#include "output/video/unifonts.h"
#include "ui/widgets/widget.h"

#include <string>
#include <vector>

/**
 * A small box with a text next to another widget, e.g. why a button can not be used now.
 *
 * The owner (a window) adds it as its last child, so that it is drawn on top,
 * and shows or hides it, e.g. while the mouse is over the widget.
 * It does not take any mouse events.
 */
class cToolTip : public cWidget
{
public:
	cToolTip();

	/**
	 * Shows the text below the target area, or above it, if there is no room below inside of the boundary.
	 */
	void showFor (const cBox<cPosition>& targetArea, const std::string& text, const cBox<cPosition>& boundary);

	bool isAt (const cPosition&) const override { return false; }

	void draw (SDL_Surface& destination, const cBox<cPosition>& clipRect) override;

private:
	std::string text;
	std::vector<std::string> lines;
	eUnicodeFontType fontType = eUnicodeFontType::LatinNormal;
};

#endif // ui_widgets_tooltipH
