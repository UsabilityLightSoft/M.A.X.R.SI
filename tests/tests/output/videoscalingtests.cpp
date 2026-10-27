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

#include "output/video/video.h"

#include <doctest.h>

namespace
{
	//--------------------------------------------------------------------------
	bool equal (const SDL_Rect& lhs, const SDL_Rect& rhs)
	{
		return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
	}
} // namespace

//------------------------------------------------------------------------------
TEST_CASE ("computeViewScaling")
{
	const cPosition noLimit (0, 0);

	SUBCASE ("integer scale, if it covers the window almost entirely")
	{
		// a menu in a maximized window on a 4K display: x4 covers 93% of the fitting x4.31
		const auto scaling = computeViewScaling (cPosition (640, 480), cPosition (3840, 2071), true, noLimit);
		CHECK (scaling.scale == 4.);
		CHECK_FALSE (scaling.smooth);
		CHECK (scaling.prescale == 1);
		CHECK (equal (scaling.destination, SDL_Rect{640, 75, 2560, 1920}));
	}
	SUBCASE ("exact integer scale fills the window")
	{
		const auto scaling = computeViewScaling (cPosition (1920, 1080), cPosition (3840, 2160), true, noLimit);
		CHECK (scaling.scale == 2.);
		CHECK_FALSE (scaling.smooth);
		CHECK (equal (scaling.destination, SDL_Rect{0, 0, 3840, 2160}));
	}
	SUBCASE ("otherwise sharp bilinear: integer upscale, then smooth scaling")
	{
		const auto scaling = computeViewScaling (cPosition (1920, 1080), cPosition (3840, 2071), true, noLimit);
		CHECK (scaling.smooth);
		CHECK (scaling.prescale == 2);
		CHECK (equal (scaling.destination, SDL_Rect{79, 0, 3682, 2071}));

		const auto menu = computeViewScaling (cPosition (640, 480), cPosition (3840, 2160), true, noLimit);
		CHECK (menu.smooth); // x4 would cover only 89% of x4.5
		CHECK (menu.prescale == 5);
		CHECK (equal (menu.destination, SDL_Rect{480, 0, 2880, 2160}));
	}
	SUBCASE ("no intermediate upscaling without render targets or above the texture size")
	{
		CHECK (computeViewScaling (cPosition (1920, 1080), cPosition (3840, 2071), false, noLimit).prescale == 1);
		CHECK (computeViewScaling (cPosition (1920, 1080), cPosition (3840, 2071), true, cPosition (2048, 2048)).prescale == 1);
		CHECK (computeViewScaling (cPosition (640, 480), cPosition (3840, 2160), true, cPosition (2048, 2048)).prescale == 3);
	}
	SUBCASE ("a window smaller than the picture: smooth downscaling")
	{
		const auto scaling = computeViewScaling (cPosition (1920, 1080), cPosition (1280, 720), true, noLimit);
		CHECK (scaling.smooth);
		CHECK (scaling.prescale == 1);
		CHECK (equal (scaling.destination, SDL_Rect{0, 0, 1280, 720}));
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("computeViewArea")
{
	const cPosition buffer (1920, 1080);
	const cPosition menu (640, 480);

	// a menu in the middle of the screen buffer
	CHECK (equal (computeViewArea (SDL_Rect{640, 300, 640, 480}, buffer, menu), SDL_Rect{640, 300, 640, 480}));
	// the whole buffer
	CHECK (equal (computeViewArea (SDL_Rect{0, 0, 1920, 1080}, buffer, menu), SDL_Rect{0, 0, 1920, 1080}));
	// a small dialog gets the size of a menu around its center
	CHECK (equal (computeViewArea (SDL_Rect{810, 424, 300, 231}, buffer, menu), SDL_Rect{640, 299, 640, 480}));
	// ... and stays inside of the buffer
	CHECK (equal (computeViewArea (SDL_Rect{0, 0, 300, 200}, buffer, menu), SDL_Rect{0, 0, 640, 480}));
	// outside of the buffer
	CHECK (computeViewArea (SDL_Rect{2000, 0, 100, 100}, buffer, menu).w == 0);
	// a buffer smaller than a menu
	CHECK (equal (computeViewArea (SDL_Rect{0, 0, 100, 100}, cPosition (600, 400), menu), SDL_Rect{0, 0, 600, 400}));
}

//------------------------------------------------------------------------------
TEST_CASE ("windowToPicturePosition")
{
	const SDL_Rect menu{640, 300, 640, 480};
	const SDL_Rect destination{640, 75, 2560, 1920}; // x4
	const cPosition window (3840, 2071);

	// each window pixel of a picture pixel gives this picture pixel
	CHECK (windowToPicturePosition (cPosition (640, 75), menu, destination, window, window) == cPosition (640, 300));
	CHECK (windowToPicturePosition (cPosition (643, 78), menu, destination, window, window) == cPosition (640, 300));
	CHECK (windowToPicturePosition (cPosition (644, 79), menu, destination, window, window) == cPosition (641, 301));
	// a button of the main menu
	CHECK (windowToPicturePosition (cPosition (2594, 1149), menu, destination, window, window) == cPosition (1128, 568));
	// the black bars are outside of the picture
	CHECK (windowToPicturePosition (cPosition (0, 1000), menu, destination, window, window).x() < menu.x);
	CHECK (windowToPicturePosition (cPosition (3839, 2070), menu, destination, window, window).x() >= menu.x + menu.w);

	// mouse positions in other units than pixels (high DPI without DPI awareness)
	CHECK (windowToPicturePosition (cPosition (1000, 300), SDL_Rect{0, 0, 1920, 1080}, SDL_Rect{0, 0, 3840, 2160}, cPosition (1920, 1080), cPosition (3840, 2160)) == cPosition (1000, 300));

	// no picture yet: unchanged
	CHECK (windowToPicturePosition (cPosition (5, 7), menu, SDL_Rect{0, 0, 0, 0}, window, window) == cPosition (5, 7));
}
