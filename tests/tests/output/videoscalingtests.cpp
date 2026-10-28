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

#include <cmath>
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

//------------------------------------------------------------------------------
TEST_CASE ("computeUiScale")
{
	SUBCASE ("automatic: 2.4 times the display scale, rounded to 0.5")
	{
		// 4K at 125%: x3 in full screen and in the maximized window (2071 = 2160 - title bar - task bar)
		CHECK (computeUiScale (0, cPosition (3840, 2160), 1.25) == 3.);
		CHECK (computeUiScale (0, cPosition (3840, 2071), 1.25) == 3.);
		// 4K at 100%: 2.4 -> 2.5
		CHECK (computeUiScale (0, cPosition (3840, 2160), 1.) == 2.5);
		// 8K at 200%: 4.8 -> 5
		CHECK (computeUiScale (0, cPosition (7680, 4320), 2.) == 5.);
		// an unknown display scale counts as 100%
		CHECK (computeUiScale (0, cPosition (3840, 2160), 0.) == 2.5);
		CHECK (computeUiScale (0, cPosition (3840, 2160), std::nan ("")) == 2.5);
	}
	SUBCASE ("automatic: reduced while the picture would be smaller than 1280x720 (a tenth less is accepted)")
	{
		// 4K at 150%: 3.5 would give 1097x617
		CHECK (computeUiScale (0, cPosition (3840, 2160), 1.5) == 3.);
		// full HD at 100%: x1.5, also maximized (1280x672)
		CHECK (computeUiScale (0, cPosition (1920, 1080), 1.) == 1.5);
		CHECK (computeUiScale (0, cPosition (1920, 1009), 1.) == 1.5);
		// a window of 2480x1465 on a 4K display at 125%: x2 (1240x732)
		CHECK (computeUiScale (0, cPosition (2480, 1465), 1.25) == 2.);
		// 2560x1440 at 100%: x2
		CHECK (computeUiScale (0, cPosition (2560, 1440), 1.) == 2.);
		// small displays and windows: not below 1
		CHECK (computeUiScale (0, cPosition (1366, 768), 1.) == 1.);
		CHECK (computeUiScale (0, cPosition (640, 480), 2.) == 1.);
	}
	SUBCASE ("chosen scale: reduced while the picture would be smaller than 640x480")
	{
		CHECK (computeUiScale (300, cPosition (3840, 2160), 1.25) == 3.);
		CHECK (computeUiScale (250, cPosition (3840, 2160), 1.25) == 2.5);
		CHECK (computeUiScale (400, cPosition (3840, 2071), 1.25) == 4.);
		CHECK (computeUiScale (400, cPosition (2480, 1465), 1.25) == 3.);
		CHECK (computeUiScale (250, cPosition (1920, 1009), 1.) == 2.);
		CHECK (computeUiScale (100, cPosition (3840, 2160), 1.25) == 1.);
		CHECK (computeUiScale (200, cPosition (640, 480), 1.) == 1.);
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("computePictureSize")
{
	CHECK (computePictureSize (cPosition (3840, 2160), 3.) == cPosition (1280, 720));
	CHECK (computePictureSize (cPosition (3840, 2071), 3.) == cPosition (1280, 690));
	CHECK (computePictureSize (cPosition (3840, 2160), 2.5) == cPosition (1536, 864));
	CHECK (computePictureSize (cPosition (2480, 1465), 2.5) == cPosition (992, 586));
	CHECK (computePictureSize (cPosition (1920, 1080), 1.5) == cPosition (1280, 720));
	CHECK (computePictureSize (cPosition (1920, 1080), 1.) == cPosition (1920, 1080));
	// at least the size of the menus
	CHECK (computePictureSize (cPosition (1000, 700), 2.) == cPosition (640, 480));
	CHECK (computePictureSize (cPosition (600, 400), 1.) == cPosition (640, 480));
	// never enlarged
	CHECK (computePictureSize (cPosition (1920, 1080), 0.5) == cPosition (1920, 1080));
}

//------------------------------------------------------------------------------
TEST_CASE ("isUiScaleFitting")
{
	CHECK (isUiScaleFitting (0, cPosition (640, 480)));
	CHECK (isUiScaleFitting (100, cPosition (640, 480)));
	CHECK (isUiScaleFitting (400, cPosition (3840, 2071)));
	CHECK_FALSE (isUiScaleFitting (400, cPosition (2480, 1465)));
	CHECK (isUiScaleFitting (300, cPosition (2480, 1465)));
	CHECK_FALSE (isUiScaleFitting (250, cPosition (1920, 1009)));
	CHECK (isUiScaleFitting (200, cPosition (1920, 1009)));
}
