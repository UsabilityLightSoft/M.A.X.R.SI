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

#include "input/mouse/cursor/cursorscaling.h"
#include "output/video/video.h"

#include <cstdlib>
#include <doctest.h>

namespace
{
	//--------------------------------------------------------------------------
	Uint32 pixelAt (const SDL_Surface& surface, int x, int y)
	{
		return reinterpret_cast<const Uint32*> (static_cast<const Uint8*> (surface.pixels) + y * surface.pitch)[x];
	}

	//--------------------------------------------------------------------------
	/** A cursor image like those of the game: 32 bit without alpha, magenta as color key; one opaque pixel. */
	UniqueSurface makeImage (int width, int height, int opaqueX, int opaqueY)
	{
		UniqueSurface image (SDL_CreateRGBSurface (0, width, height, 32, 0, 0, 0, 0));
		SDL_FillRect (image.get(), nullptr, 0xFF00FF);
		SDL_SetColorKey (image.get(), SDL_TRUE, 0xFF00FF);
		SDL_Rect rect{opaqueX, opaqueY, 1, 1};
		SDL_FillRect (image.get(), &rect, 0x2060A0);
		return image;
	}
} // namespace

//------------------------------------------------------------------------------
TEST_CASE ("computeCursorScale")
{
	constexpr auto Menu = eCursorScaleContext::Menu;
	constexpr auto Map = eCursorScaleContext::Map;

	SUBCASE ("automatic, menus: two thirds of the scale of the user interface, not below 1, times the pointer size of the system")
	{
		CHECK (computeCursorScale (3., 1., Menu, 0) == doctest::Approx (2.)); // automatic on 4K at 125%: the hand 36 pixels
		CHECK (computeCursorScale (4., 1., Menu, 0) == doctest::Approx (8. / 3.));
		CHECK (computeCursorScale (2.5, 1., Menu, 0) == doctest::Approx (5. / 3.));
		CHECK (computeCursorScale (2., 1., Menu, 0) == doctest::Approx (4. / 3.));
		CHECK (computeCursorScale (1.5, 1., Menu, 0) == doctest::Approx (1.));
		CHECK (computeCursorScale (1., 1., Menu, 0) == doctest::Approx (1.));
		CHECK (computeCursorScale (0., 1., Menu, 0) == doctest::Approx (1.));
		// the pointer of the system three times bigger ("pointer size" 5 of Windows): as Windows enlarged the cursor until now
		CHECK (computeCursorScale (1.5, 3., Menu, 0) == doctest::Approx (3.)); // the hand 54 pixels
		CHECK (computeCursorScale (3., 3., Menu, 0) == doctest::Approx (6.)); // the hand 108 pixels
		CHECK (computeCursorScale (3., 0., Menu, 0) == doctest::Approx (2.)); // unknown pointer size: normal
		for (double scale = 1.5; scale < 4.; scale += 0.5)
		{
			CHECK (computeCursorScale (scale + 0.5, 1., Menu, 0) > computeCursorScale (scale, 1., Menu, 0));
		}
	}
	SUBCASE ("automatic, map: as in the menus, but not bigger than the pictures of the game")
	{
		CHECK (computeCursorScale (3., 1., Map, 0) == doctest::Approx (2.));
		CHECK (computeCursorScale (1.5, 1., Map, 0) == doctest::Approx (1.));
		// the big pointer of the system does not make the cursor cover several tiles:
		// the circle (26 pixels) is about a tile of 24 pixels (the zoom of a typical game)
		CHECK (computeCursorScale (1.5, 3., Map, 0) == doctest::Approx (1.5));
		CHECK (computeCursorScale (3., 3., Map, 0) == doctest::Approx (3.));
		CHECK (computeCursorScale (2., 1.5, Map, 0) == doctest::Approx (2.));
		for (double scale = 1.; scale < 4.; scale += 0.5)
		{
			for (double pointer = 1.; pointer <= 8.; pointer += 0.5)
			{
				CHECK (computeCursorScale (scale, pointer, Map, 0) <= scale + 1e-9);
				CHECK (computeCursorScale (scale, pointer, Map, 0) <= computeCursorScale (scale, pointer, Menu, 0) + 1e-9);
			}
		}
	}
	SUBCASE ("chosen size: on the map that part of the pictures of the game, in the menus relatively as big as automatically")
	{
		CHECK (computeCursorScale (1.5, 3., Map, 100) == doctest::Approx (1.5));
		CHECK (computeCursorScale (1.5, 3., Map, 75) == doctest::Approx (1.125));
		CHECK (computeCursorScale (1.5, 3., Map, 125) == doctest::Approx (1.875));
		CHECK (computeCursorScale (1.5, 3., Menu, 100) == doctest::Approx (3.));
		CHECK (computeCursorScale (1.5, 3., Menu, 75) == doctest::Approx (2.25));
		CHECK (computeCursorScale (3., 1., Map, 100) == doctest::Approx (3.));
		CHECK (computeCursorScale (3., 1., Menu, 100) == doctest::Approx (3.));
		CHECK (computeCursorScale (3., 1., Map, 200) == doctest::Approx (6.));
		// a bigger size gives a bigger cursor everywhere
		for (std::size_t i = 1; i < cursorSizePercents.size(); ++i)
		{
			CHECK (computeCursorScale (1.5, 3., Map, cursorSizePercents[i]) > computeCursorScale (1.5, 3., Map, cursorSizePercents[i - 1]));
			CHECK (computeCursorScale (1.5, 3., Menu, cursorSizePercents[i]) > computeCursorScale (1.5, 3., Menu, cursorSizePercents[i - 1]));
		}
	}
	SUBCASE ("limits")
	{
		CHECK (computeCursorScale (1., 1., Map, 50) == doctest::Approx (0.5));
		CHECK (computeCursorScale (1., 1., Map, 10) == doctest::Approx (0.5)); // from the settings file
		CHECK (computeCursorScale (4., 8., Menu, 200) == doctest::Approx (16.));
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("computeAutoCursorSize")
{
	CHECK (computeAutoCursorSize (1.5, 3.) == 100);
	CHECK (computeAutoCursorSize (3., 3.) == 100);
	CHECK (computeAutoCursorSize (3., 1.) == 67);
	CHECK (computeAutoCursorSize (1.5, 1.) == 67);
	CHECK (computeAutoCursorSize (1., 1.) == 100);
	// the automatic size is the chosen size of the same percentage
	CHECK (computeCursorScale (3., 1., eCursorScaleContext::Map, 0) == doctest::Approx (computeCursorScale (3., 1., eCursorScaleContext::Map, 67)).epsilon (0.01));
}

//------------------------------------------------------------------------------
TEST_CASE ("computeScaledCursorSize")
{
	CHECK (computeScaledCursorSize (cPosition (18, 18), 2.) == cPosition (36, 36));
	CHECK (computeScaledCursorSize (cPosition (26, 25), 2.) == cPosition (52, 50));
	CHECK (computeScaledCursorSize (cPosition (18, 18), 4. / 3.) == cPosition (24, 24));
	CHECK (computeScaledCursorSize (cPosition (26, 25), 8. / 3.) == cPosition (69, 67));
	CHECK (computeScaledCursorSize (cPosition (18, 18), 0.5) == cPosition (9, 9));
	CHECK (computeScaledCursorSize (cPosition (26, 25), 1.5) == cPosition (39, 38));
	CHECK (computeScaledCursorSize (cPosition (2, 2), 0.1) == cPosition (1, 1));
}

//------------------------------------------------------------------------------
TEST_CASE ("computeScaledCursorHotPoint")
{
	SUBCASE ("the center of the pixel of the hot point, for integer factors")
	{
		CHECK (computeScaledCursorHotPoint (cPosition (12, 12), cPosition (26, 25), cPosition (26, 25)) == cPosition (12, 12));
		CHECK (computeScaledCursorHotPoint (cPosition (12, 12), cPosition (26, 25), cPosition (52, 50)) == cPosition (25, 25));
		CHECK (computeScaledCursorHotPoint (cPosition (12, 12), cPosition (26, 25), cPosition (78, 75)) == cPosition (37, 37));
		CHECK (computeScaledCursorHotPoint (cPosition (0, 0), cPosition (18, 18), cPosition (36, 36)) == cPosition (1, 1));
		CHECK (computeScaledCursorHotPoint (cPosition (17, 8), cPosition (18, 18), cPosition (54, 54)) == cPosition (52, 25));
	}
	SUBCASE ("and for other factors")
	{
		// 26x25 by 8/3 = 69x67: the center of the pixel (12, 12) is at 12.5 * 69 / 26 = 33.2, 12.5 * 67 / 25 = 33.5
		CHECK (computeScaledCursorHotPoint (cPosition (12, 12), cPosition (26, 25), cPosition (69, 67)) == cPosition (33, 33));
		// 18x18 by 4/3 = 24x24
		CHECK (computeScaledCursorHotPoint (cPosition (0, 0), cPosition (18, 18), cPosition (24, 24)) == cPosition (0, 0));
		CHECK (computeScaledCursorHotPoint (cPosition (17, 17), cPosition (18, 18), cPosition (24, 24)) == cPosition (23, 23));
	}
	SUBCASE ("always inside of the image")
	{
		CHECK (computeScaledCursorHotPoint (cPosition (30, -2), cPosition (18, 18), cPosition (36, 36)) == cPosition (35, 0));
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("makeScaledCursorSurface")
{
	SUBCASE ("integer factor: pixels as they are, color key transparent")
	{
		const auto image = makeImage (4, 3, 1, 2);
		const auto scaled = makeScaledCursorSurface (*image, 2.);
		REQUIRE (scaled != nullptr);
		CHECK (scaled->w == 8);
		CHECK (scaled->h == 6);
		CHECK (scaled->format->format == SDL_PIXELFORMAT_ARGB8888);
		CHECK (pixelAt (*scaled, 2, 4) == 0xFF2060A0);
		CHECK (pixelAt (*scaled, 3, 5) == 0xFF2060A0);
		CHECK ((pixelAt (*scaled, 1, 4) >> 24) == 0);
		CHECK ((pixelAt (*scaled, 4, 4) >> 24) == 0);
		CHECK ((pixelAt (*scaled, 0, 0) >> 24) == 0);
	}
	SUBCASE ("factor 1: the same size")
	{
		const auto image = makeImage (5, 5, 2, 2);
		const auto scaled = makeScaledCursorSurface (*image, 1.);
		REQUIRE (scaled != nullptr);
		CHECK (scaled->w == 5);
		CHECK (pixelAt (*scaled, 2, 2) == 0xFF2060A0);
	}
	SUBCASE ("factor below 1: smaller, no color of transparent pixels at the edges")
	{
		const auto image = makeImage (18, 18, 9, 9);
		SDL_Rect block{4, 4, 9, 9};
		SDL_FillRect (image.get(), &block, 0x2060A0);
		const auto scaled = makeScaledCursorSurface (*image, 0.75);
		REQUIRE (scaled != nullptr);
		CHECK (scaled->w == 14);
		CHECK (scaled->h == 14);
		CHECK (scaled->format->format == SDL_PIXELFORMAT_ARGB8888);
		CHECK (pixelAt (*scaled, 6, 6) == 0xFF2060A0);
		CHECK ((pixelAt (*scaled, 0, 0) >> 24) == 0);
		for (int x = 0; x < scaled->w; ++x)
		{
			const Uint32 pixel = pixelAt (*scaled, x, 6);
			if ((pixel >> 24) < 64) continue;
			CHECK (std::abs (static_cast<int> ((pixel >> 16) & 0xFF) - 0x20) <= 4);
			CHECK (std::abs (static_cast<int> (pixel & 0xFF) - 0xA0) <= 4);
		}
	}
	SUBCASE ("other factor: sharp bilinear, no color of transparent pixels at the edges")
	{
		const auto image = makeImage (18, 18, 9, 9);
		// pixels 5..10: by 8/3 from 13.3 to 29.3 - the edges fall inside of pixels
		SDL_Rect block{5, 5, 6, 6};
		SDL_FillRect (image.get(), &block, 0x2060A0);
		const auto scaled = makeScaledCursorSurface (*image, 8. / 3.);
		REQUIRE (scaled != nullptr);
		CHECK (scaled->w == 48);
		CHECK (scaled->h == 48);
		// inside of the block opaque, with its color
		CHECK (pixelAt (*scaled, 21, 21) == 0xFF2060A0);
		// far outside transparent
		CHECK ((pixelAt (*scaled, 2, 2) >> 24) == 0);
		// the edges: partly transparent pixels keep the color of the block (no magenta mixed in)
		int edgePixels = 0;
		for (int x = 0; x < scaled->w; ++x)
		{
			const Uint32 pixel = pixelAt (*scaled, x, 21);
			const Uint32 alpha = pixel >> 24;
			if (alpha < 64 || alpha == 0xFF) continue;
			++edgePixels;
			CHECK (std::abs (static_cast<int> ((pixel >> 16) & 0xFF) - 0x20) <= 4);
			CHECK (std::abs (static_cast<int> ((pixel >> 8) & 0xFF) - 0x60) <= 4);
			CHECK (std::abs (static_cast<int> (pixel & 0xFF) - 0xA0) <= 4);
		}
		CHECK (edgePixels > 0);
	}
}
