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

#include "events/eventmanager.h"

#include "events/keyboardevents.h"
#include "events/mouseevents.h"
#include "output/video/video.h"

//------------------------------------------------------------------------------
cEventManager::cEventManager()
{}

//------------------------------------------------------------------------------
cEventManager& cEventManager::getInstance()
{
	static cEventManager instance;
	return instance;
}

//------------------------------------------------------------------------------
bool cEventManager::shouldExit() const
{
	return isDone;
}

//------------------------------------------------------------------------------
void cEventManager::run()
{
	SDL_Event event;
	while (SDL_PollEvent (&event))
	{
		if (!handleSdlEvent (event))
		{
			isDone = true;
			break;
		}
	}
	// The picture has been scaled or moved in the window (window resized, menu shown instead of the game):
	// the mouse points now to another position of the screen buffer.
	if (Video.getPresentationVersion() != lastPresentationVersion)
	{
		lastPresentationVersion = Video.getPresentationVersion();
		SDL_Window* mouseFocus = SDL_GetMouseFocus();
		if (lastMouseWindowPosition && mouseFocus != nullptr)
		{
			SDL_MouseMotionEvent motion{};
			motion.type = SDL_MOUSEMOTION;
			motion.timestamp = SDL_GetTicks();
			motion.windowID = SDL_GetWindowID (mouseFocus);
			motion.state = SDL_GetMouseState (nullptr, nullptr);
			sendMouseMotion (motion, *lastMouseWindowPosition);
		}
	}
	SDL_Delay (1);
}

//------------------------------------------------------------------------------
void cEventManager::sendMouseMotion (const SDL_MouseMotionEvent& sdlEvent, const cPosition& windowPosition)
{
	lastMouseWindowPosition = windowPosition;

	const auto position = Video.windowToBuffer (windowPosition);
	if (lastMouseBufferPosition == position) return;
	const cPosition offset = lastMouseBufferPosition ? cPosition (position - *lastMouseBufferPosition) : cPosition (0, 0);
	lastMouseBufferPosition = position;

	SDL_MouseMotionEvent motion = sdlEvent;
	motion.x = position.x();
	motion.y = position.y();
	motion.xrel = offset.x();
	motion.yrel = offset.y();
	mouseMotionEvent (cEventMouseMotion (motion));
}

//------------------------------------------------------------------------------
bool cEventManager::handleSdlEvent (const SDL_Event& event)
{
	switch (event.type)
	{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			keyboardEvent (cKeyboardEvent (event.key));
			break;
		case SDL_TEXTINPUT:
			textInputEvent (cTextInputEvent (event.text));
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		{
			// NOTE: we do not handle all mouse buttons yet.
			if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_RIGHT && event.button.button != SDL_BUTTON_MIDDLE) break;
			const cPosition windowPosition (event.button.x, event.button.y);
			if (lastMouseWindowPosition != windowPosition)
			{
				// the mouse has to be where the button has been pressed
				SDL_MouseMotionEvent motion{};
				motion.type = SDL_MOUSEMOTION;
				motion.timestamp = event.button.timestamp;
				motion.windowID = event.button.windowID;
				motion.which = event.button.which;
				motion.state = SDL_GetMouseState (nullptr, nullptr);
				sendMouseMotion (motion, windowPosition);
			}
			SDL_MouseButtonEvent button = event.button;
			const auto position = Video.windowToBuffer (windowPosition);
			button.x = position.x();
			button.y = position.y();
			mouseButtonEvent (cEventMouseButton (button));
			break;
		}
		case SDL_MOUSEWHEEL:
			mouseWheelEvent (cEventMouseWheel (event.wheel));
			break;
		case SDL_QUIT:
			return false;
			break;
		case SDL_MOUSEMOTION:
			sendMouseMotion (event.motion, cPosition (event.motion.x, event.motion.y));
			break;
		case SDL_WINDOWEVENT:
			Video.handleWindowEvent (event.window);
			Video.draw();
			break;
		default: break;
	}
	// Everything is OK, continue the cycle
	return true;
}
