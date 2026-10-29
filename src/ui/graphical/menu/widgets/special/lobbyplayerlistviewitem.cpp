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

#include "ui/graphical/menu/widgets/special/lobbyplayerlistviewitem.h"

#include "game/data/player/playerbasicdata.h"
#include "output/video/video.h"
#include "resources/playercolor.h"
#include "resources/uidata.h"
#include "SDLutility/drawing.h"
#include "SDLutility/tosdl.h"
#include "input/mouse/mouse.h"
#include "ui/widgets/clickablewidget.h"
#include "ui/widgets/image.h"
#include "ui/widgets/label.h"
#include "utility/color.h"

#include <cassert>

/**
 * The ready dot of a player in the lobby, centered in a bigger cell that can be clicked.
 * If it can be clicked, the cell is highlighted while the mouse is over it.
 */
class cLobbyPlayerReadyButton : public cClickableWidget
{
public:
	explicit cLobbyPlayerReadyButton (const cBox<cPosition>& area) :
		cClickableWidget (area)
	{}

	void setImage (SDL_Surface& image_)
	{
		image = UniqueSurface (SDL_CreateRGBSurface (0, image_.w, image_.h, Video.getColDepth(), 0, 0, 0, 0));
		SDL_FillRect (image.get(), nullptr, 0xFF00FF);
		SDL_SetColorKey (image.get(), SDL_TRUE, 0xFF00FF);
		SDL_BlitSurface (&image_, nullptr, image.get(), nullptr);
	}

	void setClickable (bool clickable_) { clickable = clickable_; }

	cSignal<void()> clicked;

	void draw (SDL_Surface& destination, const cBox<cPosition>& clipRect) override
	{
		const auto* mouse = getActiveMouse();
		if (clickable && mouse && isAt (mouse->getPosition()) && getArea().intersects (clipRect))
		{
			// highlight: a lighter cell with a light frame (like the menu lists), the dot stays as it is
			SDL_Rect oldClip;
			SDL_GetClipRect (&destination, &oldClip);
			const auto clip = toSdlRect (getArea().intersection (clipRect));
			SDL_SetClipRect (&destination, &clip);
			auto cell = toSdlRect (getArea());
			SDL_FillRect (&destination, &cell, toSdlColor (isPressed ? cRgbColor (0x60, 0x60, 0x60) : cRgbColor (0x3C, 0x3C, 0x3C), destination));
			drawRectangle (destination, getArea(), cRgbColor (0xE3, 0xDA, 0xCF));
			SDL_SetClipRect (&destination, &oldClip);
		}
		if (image)
		{
			const cPosition imagePosition = getPosition() + (getSize() - cPosition (image->w, image->h)) / 2;
			blitClipped (*image, cBox<cPosition> (imagePosition, imagePosition + cPosition (image->w - 1, image->h - 1)), destination, clipRect);
		}
		cClickableWidget::draw (destination, clipRect);
	}

protected:
	bool handleClicked (cApplication&, cMouse&, eMouseButtonType button) override
	{
		if (button != eMouseButtonType::Left) return false;
		clicked();
		return true;
	}

private:
	UniqueSurface image;
	bool clickable = false;
};

namespace
{
	constexpr int readyDotSize = 10;
	const cPosition readyCellSize (readyDotSize + 2 * cLobbyPlayerListViewItem::readyCellMarginX, readyDotSize + 2 * cLobbyPlayerListViewItem::readyCellMarginY);
} // namespace

//------------------------------------------------------------------------------
cLobbyPlayerListViewItem::cLobbyPlayerListViewItem (std::shared_ptr<cPlayerBasicData> player_) :
	cAbstractListViewItem (cPosition (50, 0)),
	player (std::move (player_))
{
	assert (player != nullptr);

	const cPosition readyCellPosition = getPosition() + cPosition (getSize().x() - readyCellSize.x(), 0);
	readyButton = emplaceChild<cLobbyPlayerReadyButton> (cBox<cPosition> (readyCellPosition, readyCellPosition + readyCellSize - cPosition (1, 1)));
	signalConnectionManager.connect (readyButton->clicked, [this]() { readyClicked(); });

	colorImage = emplaceChild<cImage> (getPosition() + cPosition (0, readyCellMarginY));

	updatePlayerColor();
	updatePlayerReady();

	// the name ends where the cell of the ready dot begins
	nameLabel = emplaceChild<cLabel> (cBox<cPosition> (cPosition (colorImage->getEndPosition().x() + 4, colorImage->getPosition().y()), cPosition (readyButton->getPosition().x(), colorImage->getPosition().y() + readyDotSize)), player->getName());
	nameLabel->setConsumeClick (false);

	fitToChildren();

	signalConnectionManager.connect (player->nameChanged, [this]() { updatePlayerName(); });
	signalConnectionManager.connect (player->colorChanged, [this]() { updatePlayerColor(); });
	signalConnectionManager.connect (player->readyChanged, [this]() { updatePlayerReady(); });
}

//------------------------------------------------------------------------------
const std::shared_ptr<cPlayerBasicData>& cLobbyPlayerListViewItem::getPlayer() const
{
	return player;
}

//------------------------------------------------------------------------------
void cLobbyPlayerListViewItem::update()
{
	updatePlayerColor();
	updatePlayerName();
	updatePlayerReady();
}

//------------------------------------------------------------------------------
void cLobbyPlayerListViewItem::updatePlayerName()
{
	nameLabel->setText (player->getName());
}

//------------------------------------------------------------------------------
void cLobbyPlayerListViewItem::updatePlayerColor()
{
	SDL_Rect src = {0, 0, 10, 10};

	UniqueSurface colorSurface (SDL_CreateRGBSurface (0, src.w, src.h, Video.getColDepth(), 0, 0, 0, 0));
	SDL_BlitSurface (cPlayerColor::getTexture (player->getColor()), &src, colorSurface.get(), nullptr);

	colorImage->setImage (colorSurface.get());
}

//------------------------------------------------------------------------------
void cLobbyPlayerListViewItem::updatePlayerReady()
{
	SDL_Rect src = {player->isReady() ? 10 : 0, 0, 10, 10};

	UniqueSurface readySurface (SDL_CreateRGBSurface (0, src.w, src.h, Video.getColDepth(), 0, 0, 0, 0));
	SDL_SetColorKey (readySurface.get(), SDL_TRUE, 0xFF00FF);
	SDL_FillRect (readySurface.get(), nullptr, 0xFF00FF);
	SDL_BlitSurface (GraphicsData.gfx_player_ready.get(), &src, readySurface.get(), nullptr);

	readyButton->setImage (*readySurface);
}

//------------------------------------------------------------------------------
void cLobbyPlayerListViewItem::setReadyClickable (bool clickable)
{
	readyButton->setClickable (clickable);
}

//------------------------------------------------------------------------------
void cLobbyPlayerListViewItem::handleResized (const cPosition& oldSize)
{
	cAbstractListViewItem::handleResized (oldSize);

	if (oldSize.x() == getSize().x()) return;

	readyButton->moveTo (getPosition() + cPosition (getSize().x() - readyCellSize.x(), 0));

	nameLabel->resize (cPosition (getSize().x() - readyCellSize.x() - colorImage->getSize().x() - 4, readyDotSize));

	fitToChildren();
}
