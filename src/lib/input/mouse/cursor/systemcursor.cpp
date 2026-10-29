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

#include "input/mouse/cursor/systemcursor.h"

#include <algorithm>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#endif

namespace
{
#ifdef _WIN32
	/** The pointer size 1 of Windows: CursorBaseSize 32 (each further step of the settings adds 16). */
	constexpr DWORD normalCursorBaseSize = 32;
	/** The largest pointer size of Windows (15: 256 pixels). */
	constexpr double maximalPointerScale = 8.;

	/** SetThreadCursorCreationScaling (Windows 11, build 22000): "the cursors created by this thread are never scaled". */
	constexpr UINT cursorCreationScalingNone = 1;

	using SetThreadCursorCreationScalingFunction = UINT (WINAPI*) (UINT);

	//--------------------------------------------------------------------------
	SetThreadCursorCreationScalingFunction getSetThreadCursorCreationScaling()
	{
		static const auto function = []() -> SetThreadCursorCreationScalingFunction {
			const HMODULE user32 = GetModuleHandleW (L"user32.dll");
			if (user32 == nullptr) return nullptr;
			return reinterpret_cast<SetThreadCursorCreationScalingFunction> (reinterpret_cast<void*> (GetProcAddress (user32, "SetThreadCursorCreationScaling")));
		}();
		return function;
	}
#endif
} // namespace

//------------------------------------------------------------------------------
double readSystemPointerScale()
{
#ifdef _WIN32
	DWORD size = 0;
	DWORD bytes = sizeof (size);
	if (RegGetValueW (HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, nullptr, &size, &bytes) != ERROR_SUCCESS || size == 0)
	{
		return 1.;
	}
	return std::clamp (static_cast<double> (size) / normalCursorBaseSize, 1., maximalPointerScale);
#else
	return 1.;
#endif
}

//------------------------------------------------------------------------------
bool doesSystemEnlargeColorCursors()
{
#ifdef _WIN32
	return getSetThreadCursorCreationScaling() == nullptr;
#else
	return false;
#endif
}

//------------------------------------------------------------------------------
SDL_Cursor* createSystemColorCursor (SDL_Surface& surface, int hotX, int hotY)
{
#ifdef _WIN32
	// SDL2 copies the cursor with CopyImage, so that Windows enlarges it by the pointer size:
	// with a large pointer the cursor of the game would be several times bigger than wanted.
	// Cursors created with "no scaling" are shown as they are.
	if (const auto setScaling = getSetThreadCursorCreationScaling())
	{
		const UINT previous = setScaling (cursorCreationScalingNone);
		SDL_Cursor* cursor = SDL_CreateColorCursor (&surface, hotX, hotY);
		setScaling (previous);
		return cursor;
	}
#endif
	return SDL_CreateColorCursor (&surface, hotX, hotY);
}
