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

#include "input/mouse/mouse.h"

#include "SDLutility/uniquesurface.h"
#include "events/eventmanager.h"
#include "events/mouseevents.h"
#include "input/mouse/cursor/mousecursorsimple.h"
#include "output/video/video.h"

#include <utility>

namespace
{
	constexpr std::chrono::milliseconds doubleClickTime{500};

	//--------------------------------------------------------------------------
	/**
	 * Enlarges the cursor image by an integer factor (pixels as they are),
	 * so that the cursor has the size of the scaled picture.
	 */
	UniqueSurface makeScaledCursorSurface (SDL_Surface& surface, int scale)
	{
		UniqueSurface converted (SDL_ConvertSurfaceFormat (&surface, SDL_PIXELFORMAT_ARGB8888, 0));
		if (converted == nullptr || scale <= 1) return converted;

		UniqueSurface scaled (SDL_CreateRGBSurfaceWithFormat (0, converted->w * scale, converted->h * scale, 32, SDL_PIXELFORMAT_ARGB8888));
		if (scaled == nullptr) return converted;
		// the conversion has turned the color key into transparent pixels: copy all pixels as they are
		SDL_SetColorKey (converted.get(), SDL_FALSE, 0);
		SDL_SetSurfaceBlendMode (converted.get(), SDL_BLENDMODE_NONE);
		if (SDL_BlitScaled (converted.get(), nullptr, scaled.get(), nullptr) != 0) return converted;
		return scaled;
	}
} // namespace

//------------------------------------------------------------------------------
cMouse::cMouse()
{
	setCursor (std::make_unique<cMouseCursorSimple> (eMouseCursorSimpleType::Hand), true);

	signalConnectionManager.connect (cEventManager::getInstance().mouseMotionEvent, [this] (const cEventMouseMotion& event) { handleMouseMotionEvent (event); });
	signalConnectionManager.connect (cEventManager::getInstance().mouseButtonEvent, [this] (const cEventMouseButton& event) { handleMouseButtonEvent (event); });
	signalConnectionManager.connect (cEventManager::getInstance().mouseWheelEvent, [this] (const cEventMouseWheel& event) { handleMouseWheelEvent (event); });
	signalConnectionManager.connect (Video.cursorScaleChanged, [this]() { updateSdlCursor(); });
}

//------------------------------------------------------------------------------
cMouse::~cMouse()
{}

//------------------------------------------------------------------------------
void cMouse::handleMouseMotionEvent (const cEventMouseMotion& event)
{
	position = event.getNewPosition();
	moved (*this, event.getOffset());
}

//------------------------------------------------------------------------------
void cMouse::handleMouseButtonEvent (const cEventMouseButton& event)
{
	auto button = event.getButton();

	switch (event.getType())
	{
		case cEventMouseButton::eType::Down:
		{
			buttonPressedState[button] = true;

			const auto currentClickTime = std::chrono::steady_clock::now();
			auto& lastClickTime = lastClickTimes[button];
			if (currentClickTime - lastClickTime <= doubleClickTime)
			{
				++buttonClickCount[button];
			}
			else
			{
				buttonClickCount[button] = 1;
			}
			lastClickTime = currentClickTime;

			pressed (*this, button);
			break;
		}
		case cEventMouseButton::eType::Up:
		{
			buttonPressedState[button] = false;
			released (*this, button);
			break;
		}
	}
}

//------------------------------------------------------------------------------
void cMouse::handleMouseWheelEvent (const cEventMouseWheel& event)
{
	wheelMoved (*this, event.getAmount());
}

//------------------------------------------------------------------------------
const cPosition& cMouse::getPosition() const
{
	return position;
}

//------------------------------------------------------------------------------
bool cMouse::setCursor (std::unique_ptr<cMouseCursor> cursor_, bool force)
{
	if (cursor_ == nullptr) return false;

	if (!force && cursor != nullptr && *cursor_ == *cursor) return false;

	cursor = std::move (cursor_);

	updateSdlCursor();

	return true;
}

//------------------------------------------------------------------------------
void cMouse::updateSdlCursor()
{
	if (cursor == nullptr) return;

	auto cursorSurface = cursor->getSurface();
	if (cursorSurface == nullptr) return;
	auto hotPoint = cursor->getHotPoint();

	const int scale = Video.getCursorScale();
	const auto scaledSurface = makeScaledCursorSurface (*cursorSurface, scale);
	auto newSdlCursor = scaledSurface ? SdlCursorPtrType (SDL_CreateColorCursor (scaledSurface.get(), hotPoint.x() * scale, hotPoint.y() * scale)) : SdlCursorPtrType (SDL_CreateColorCursor (cursorSurface, hotPoint.x(), hotPoint.y()));

	SDL_SetCursor (newSdlCursor.get());

	sdlCursor = std::move (newSdlCursor);
}

//------------------------------------------------------------------------------
void cMouse::show()
{
	SDL_ShowCursor (true);
}

//------------------------------------------------------------------------------
void cMouse::hide()
{
	SDL_ShowCursor (false);
}

//------------------------------------------------------------------------------
const cMouseCursor* cMouse::getCursor() const
{
	return cursor.get();
}

//------------------------------------------------------------------------------
bool cMouse::isButtonPressed (eMouseButtonType button) const
{
	auto iter = buttonPressedState.find (button);
	if (iter == buttonPressedState.end())
	{
		return false;
	}
	else
		return iter->second;
}

//------------------------------------------------------------------------------
unsigned int cMouse::getButtonClickCount (eMouseButtonType button) const
{
	auto iter = buttonClickCount.find (button);
	if (iter == buttonClickCount.end())
	{
		return 0;
	}
	else
		return iter->second;
}
