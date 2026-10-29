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

#ifndef input_mouse_cursor_systemcursorH
#define input_mouse_cursor_systemcursorH

#include <SDL.h>

/**
 * The size of the mouse pointer set in the system: 1 = normal size.
 * Windows: "Pointer size" of the accessibility settings (HKCU\Control Panel\Cursors\CursorBaseSize / 32: 1 to 8).
 * Other systems: 1.
 */
double readSystemPointerScale();

/**
 * Whether the system shows the color cursors of the game enlarged by the size of its mouse pointer
 * (see readSystemPointerScale), so that the cursor images have to be made smaller by it.
 * Windows before 11: yes (SDL2 creates the cursors with CopyImage, that follows the pointer size);
 * Windows 11: no (see createSystemColorCursor); other systems: no.
 */
bool doesSystemEnlargeColorCursors();

/**
 * Like SDL_CreateColorCursor, but the system shows the cursor in the size of the surface:
 * not enlarged by the pointer size or the scale of the display (Windows 11: SetThreadCursorCreationScaling).
 * So the game decides the size of its cursor on the screen.
 */
SDL_Cursor* createSystemColorCursor (SDL_Surface& surface, int hotX, int hotY);

#endif // input_mouse_cursor_systemcursorH
