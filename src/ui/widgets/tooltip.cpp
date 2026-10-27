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

#include "ui/widgets/tooltip.h"

#include "SDLutility/drawing.h"
#include "SDLutility/tosdl.h"
#include "utility/color.h"

#include <algorithm>

namespace
{
	constexpr int maximalTextWidth = 220;
	constexpr int padding = 4;
	constexpr int distance = 3;
} // namespace

//------------------------------------------------------------------------------
cToolTip::cToolTip()
{
	hide();
	disable();
}

//------------------------------------------------------------------------------
void cToolTip::showFor (const cBox<cPosition>& targetArea, const std::string& text_, const cBox<cPosition>& boundary)
{
	auto* font = cUnicodeFont::font.get();
	if (text_ != text || lines.empty())
	{
		text = text_;
		lines = font->breakText (text, maximalTextWidth, fontType);
	}
	int textWidth = 0;
	for (const auto& line : lines)
	{
		textWidth = std::max (textWidth, font->getTextWide (line, fontType));
	}
	const cPosition size (textWidth + 2 * padding, static_cast<int> (lines.size()) * font->getFontHeight (fontType) + 2 * padding);

	// centered below the target, above it if there is no room below; inside of the boundary
	cPosition position (targetArea.getMinCorner().x() + targetArea.getSize().x() / 2 - size.x() / 2, targetArea.getMaxCorner().y() + distance);
	if (position.y() + size.y() > boundary.getMaxCorner().y())
	{
		position.y() = targetArea.getMinCorner().y() - distance - size.y();
	}
	position.x() = std::clamp (position.x(), boundary.getMinCorner().x(), std::max (boundary.getMinCorner().x(), boundary.getMaxCorner().x() - size.x()));
	position.y() = std::max (position.y(), boundary.getMinCorner().y());

	setArea (cBox<cPosition> (position, cPosition (position.x() + size.x() - 1, position.y() + size.y() - 1)));
	show();
}

//------------------------------------------------------------------------------
void cToolTip::draw (SDL_Surface& destination, const cBox<cPosition>& clipRect)
{
	if (isHidden()) return;

	// dark box with a light frame, like the lists of the menus
	auto rect = toSdlRect (getArea());
	SDL_FillRect (&destination, &rect, toSdlColor (cRgbColor (0x18, 0x18, 0x18), destination));
	drawRectangle (destination, getArea(), cRgbColor (0xE3, 0xDA, 0xCF));

	auto* font = cUnicodeFont::font.get();
	int y = getPosition().y() + padding;
	for (const auto& line : lines)
	{
		font->showText (getPosition().x() + padding, y, line, fontType);
		y += font->getFontHeight (fontType);
	}

	cWidget::draw (destination, clipRect);
}
