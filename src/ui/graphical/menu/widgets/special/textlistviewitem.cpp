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

#include "ui/graphical/menu/widgets/special/textlistviewitem.h"

#include "SDLutility/tosdl.h"
#include "ui/widgets/label.h"

#include <algorithm>

//------------------------------------------------------------------------------
cTextListViewItem::cTextListViewItem (const std::string& text) :
	cAbstractListViewItem (cPosition (cUnicodeFont::font->getTextWide (text), cUnicodeFont::font->getFontHeight (eUnicodeFontType::LatinNormal)))
{
	label = emplaceChild<cLabel> (cBox<cPosition> (cPosition (0, 0), cPosition (getSize().x() - 1, cUnicodeFont::font->getFontHeight (eUnicodeFontType::LatinNormal))), text);
	label->setConsumeClick (false);
}

//------------------------------------------------------------------------------
const std::string& cTextListViewItem::getText() const
{
	return label->getText();
}

//------------------------------------------------------------------------------
void cTextListViewItem::setDimmed (bool dimmed_)
{
	dimmed = dimmed_;
}

//------------------------------------------------------------------------------
void cTextListViewItem::draw (SDL_Surface& destination, const cBox<cPosition>& clipRect)
{
	cAbstractListViewItem::draw (destination, clipRect);

	if (!dimmed || isHidden() || !getArea().intersects (clipRect)) return;

	// cover the text with the background color of the lists (0x181818), partly transparent
	const auto area = getArea().intersection (clipRect);
	const auto size = area.getSize();
	if (dimSurface == nullptr || dimSurface->w < size.x() || dimSurface->h < size.y())
	{
		dimSurface = UniqueSurface (SDL_CreateRGBSurface (0, std::max (size.x(), getSize().x()), std::max (size.y(), getSize().y()), 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000));
		SDL_FillRect (dimSurface.get(), nullptr, SDL_MapRGBA (dimSurface->format, 0x18, 0x18, 0x18, 170));
		SDL_SetSurfaceBlendMode (dimSurface.get(), SDL_BLENDMODE_BLEND);
	}
	SDL_Rect source{0, 0, size.x(), size.y()};
	SDL_Rect position = toSdlRect (area);
	SDL_BlitSurface (dimSurface.get(), &source, &destination, &position);
}

//------------------------------------------------------------------------------
void cTextListViewItem::handleResized (const cPosition& oldSize)
{
	cAbstractListViewItem::handleResized (oldSize);

	label->resize (getSize());
}
