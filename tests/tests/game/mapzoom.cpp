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

#include "game/data/gui/mapzoom.h"

#include <cmath>
#include <doctest.h>
#include <vector>

namespace
{
	//--------------------------------------------------------------------------
	int tileSizeOf (float zoom)
	{
		return static_cast<int> (std::lround (zoom * mapZoomFullTileSize));
	}
} // namespace

//------------------------------------------------------------------------------
TEST_CASE ("computeMapFittingTileSize")
{
	SUBCASE ("the whole map fits on both axes")
	{
		// map area of a 4K screen with the interface scale x3 (picture 1280x720)
		CHECK (computeMapFittingTileSize (cPosition (1088, 688), cPosition (250, 250)) == 2);
		CHECK (computeMapFittingTileSize (cPosition (1088, 688), cPosition (112, 112)) == 6);
		// map area of a 4K screen with the interface scale 100%
		CHECK (computeMapFittingTileSize (cPosition (3648, 2128), cPosition (250, 250)) == 8);
		// not a square map: the narrower axis decides
		CHECK (computeMapFittingTileSize (cPosition (1000, 300), cPosition (100, 50)) == 6);
		CHECK (computeMapFittingTileSize (cPosition (300, 1000), cPosition (50, 100)) == 6);
	}
	SUBCASE ("never nearer than zoom factor 1")
	{
		CHECK (computeMapFittingTileSize (cPosition (1088, 688), cPosition (10, 10)) == mapZoomFullTileSize);
	}
	SUBCASE ("at least one pixel, even if the map can not fit")
	{
		CHECK (computeMapFittingTileSize (cPosition (448, 448), cPosition (800, 800)) == 1);
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("clampMapPixelOffset")
{
	SUBCASE ("a map larger than the area: the area does not leave the map")
	{
		// 250 tiles of 5 pixels = 1250 pixels in an area of 1088
		CHECK (clampMapPixelOffset (-100, 1088, 250, 5) == 0);
		CHECK (clampMapPixelOffset (500, 1088, 250, 5) == 500);
		CHECK (clampMapPixelOffset (100000, 1088, 250, 5) == 250 * 64 - 1088 * 64 / 5);
		// exactly as large as the area
		CHECK (clampMapPixelOffset (1000, 1000, 250, 4) == 0);
	}
	SUBCASE ("a map smaller than the area is centered and does not move")
	{
		// 250 tiles of 2 pixels = 500 pixels in an area of 1088: 294 pixels on each side
		CHECK (clampMapPixelOffset (0, 1088, 250, 2) == -294 * 32);
		CHECK (clampMapPixelOffset (12345, 1088, 250, 2) == -294 * 32);
		CHECK (clampMapPixelOffset (-12345, 1088, 250, 2) == -294 * 32);
		CHECK (computeZoomedMapOffset (clampMapPixelOffset (0, 1088, 250, 2), 2) == -294);
	}
	SUBCASE ("the centered map starts at a whole pixel for every tile size")
	{
		for (int tileSize = 1; tileSize <= mapZoomFullTileSize; ++tileSize)
		{
			for (int areaSize : {448, 641, 688, 1088, 2128, 3648})
			{
				for (int mapTiles : {20, 64, 112, 250})
				{
					if (mapTiles * tileSize >= areaSize) continue;
					const int margin = (areaSize - mapTiles * tileSize) / 2;
					CHECK (computeZoomedMapOffset (clampMapPixelOffset (0, areaSize, mapTiles, tileSize), tileSize) == -margin);
				}
			}
		}
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("computeZoomedMapOffset")
{
	SUBCASE ("positive offsets: whole tiles plus the rounded rest, as before")
	{
		CHECK (computeZoomedMapOffset (0, 5) == 0);
		CHECK (computeZoomedMapOffset (64 * 10, 5) == 50);
		CHECK (computeZoomedMapOffset (64 * 10 + 32, 5) == 53); // 52.5 rounded up
		CHECK (computeZoomedMapOffset (64 * 10 + 63, 64) == 703);
	}
	SUBCASE ("negative offsets are rounded the same way")
	{
		CHECK (computeZoomedMapOffset (-64, 2) == -2);
		CHECK (computeZoomedMapOffset (-1, 64) == -1);
		CHECK (computeZoomedMapOffset (-32, 2) == -1);
		CHECK (computeZoomedMapOffset (-64 * 7 - 16, 8) == -58);
	}
}

//------------------------------------------------------------------------------
TEST_CASE ("zoom slider with a logarithmic scale")
{
	const int maxValue = 1000;
	const float minimalZoom = 2.f / 64;

	CHECK (computeZoomFromSliderValue (0, maxValue, minimalZoom) == doctest::Approx (1.f));
	CHECK (computeZoomFromSliderValue (maxValue, maxValue, minimalZoom) == doctest::Approx (minimalZoom));
	// the middle of the slider is the geometric middle of the range: 64 -> 2 pixels, middle at 11.3 pixels
	CHECK (computeZoomFromSliderValue (maxValue / 2, maxValue, minimalZoom) * 64 == doctest::Approx (std::sqrt (64. * 2.)).epsilon (0.001));

	CHECK (computeSliderValueFromZoom (1.f, maxValue, minimalZoom) == 0);
	CHECK (computeSliderValueFromZoom (minimalZoom, maxValue, minimalZoom) == maxValue);
	CHECK (computeSliderValueFromZoom (0.001f, maxValue, minimalZoom) == maxValue);

	// every tile size keeps its own slider value and comes back the same
	for (int tileSize = 2; tileSize <= 64; ++tileSize)
	{
		const int value = computeSliderValueFromZoom (tileSize / 64.f, maxValue, minimalZoom);
		CHECK (tileSizeOf (computeZoomFromSliderValue (value, maxValue, minimalZoom)) == tileSize);
	}
	// no range: the map fits already at zoom factor 1
	CHECK (computeZoomFromSliderValue (500, maxValue, 1.f) == doctest::Approx (1.f));
	CHECK (computeSliderValueFromZoom (0.5f, maxValue, 1.f) == 0);
}

//------------------------------------------------------------------------------
TEST_CASE ("computeNextZoom")
{
	const float minimalZoom = 2.f / 64; // a map of 250 tiles on a 4K screen, interface x3

	SUBCASE ("the mouse wheel goes through the whole range in comfortable steps")
	{
		std::vector<int> sizes{64};
		float zoom = 1.f;
		while (sizes.size() < 100)
		{
			zoom = computeNextZoom (zoom, minimalZoom, 0.05, false);
			if (tileSizeOf (zoom) == sizes.back()) break;
			sizes.push_back (tileSizeOf (zoom));
		}
		CHECK (sizes.back() == 2);
		CHECK (sizes.size() >= 16);
		CHECK (sizes.size() <= 26);
		for (std::size_t i = 1; i < sizes.size(); ++i)
		{
			CHECK (sizes[i] < sizes[i - 1]);
			// not coarser than 1.25 per step, where pixels allow
			if (sizes[i] >= 8) CHECK (sizes[i - 1] <= sizes[i] * 1.25 + 0.5);
		}
		// and back
		for (std::size_t i = 0; i < 100 && tileSizeOf (zoom) < 64; ++i)
		{
			const int before = tileSizeOf (zoom);
			zoom = computeNextZoom (zoom, minimalZoom, 0.05, true);
			CHECK (tileSizeOf (zoom) > before);
		}
		CHECK (tileSizeOf (zoom) == 64);
	}
	SUBCASE ("the limits hold")
	{
		CHECK (tileSizeOf (computeNextZoom (1.f, minimalZoom, 0.05, true)) == 64);
		CHECK (tileSizeOf (computeNextZoom (minimalZoom, minimalZoom, 0.05, false)) == 2);
		CHECK (tileSizeOf (computeNextZoom (1.f, 1.f, 0.05, false)) == 64);
	}
	SUBCASE ("the zoom buttons: about six presses from near to far")
	{
		float zoom = 1.f;
		int presses = 0;
		while (tileSizeOf (zoom) > 2 && presses < 20)
		{
			zoom = computeNextZoom (zoom, minimalZoom, 1. / 6, false);
			++presses;
		}
		CHECK (presses >= 6);
		CHECK (presses <= 7);
	}
}
