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

#include "utility/mathtools.h"

#include <algorithm>
#include <cmath>

namespace
{
	//--------------------------------------------------------------------------
	int floorDivision (int value, int divisor)
	{
		const int quotient = value / divisor;
		return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
	}

	//--------------------------------------------------------------------------
	int toTileSize (float zoom)
	{
		return Round (zoom * mapZoomFullTileSize);
	}
} // namespace

//------------------------------------------------------------------------------
int computeMapFittingTileSize (const cPosition& areaSize, const cPosition& mapSize)
{
	if (mapSize.x() <= 0 || mapSize.y() <= 0) return mapZoomFullTileSize;

	const int tileSize = std::min (areaSize.x() / mapSize.x(), areaSize.y() / mapSize.y());
	return std::clamp (tileSize, 1, mapZoomFullTileSize);
}

//------------------------------------------------------------------------------
int clampMapPixelOffset (int pixelOffset, int areaSize, int mapTiles, int tileSize)
{
	if (mapTiles * tileSize >= areaSize)
	{
		const int maximalOffset = mapTiles * mapZoomFullTileSize - static_cast<int> (areaSize * static_cast<double> (mapZoomFullTileSize) / tileSize);
		return std::clamp (pixelOffset, 0, std::max (0, maximalOffset));
	}
	// the map is smaller than the area: the same empty space on both sides
	const int margin = (areaSize - mapTiles * tileSize) / 2;
	return -static_cast<int> (std::lround (margin * static_cast<double> (mapZoomFullTileSize) / tileSize));
}

//------------------------------------------------------------------------------
int computeZoomedMapOffset (int pixelOffset, int tileSize)
{
	const int tiles = floorDivision (pixelOffset, mapZoomFullTileSize);
	const int rest = pixelOffset - tiles * mapZoomFullTileSize;
	return tiles * tileSize + Round (rest * (tileSize / static_cast<float> (mapZoomFullTileSize)));
}

//------------------------------------------------------------------------------
float computeZoomFromSliderValue (int value, int sliderMaxValue, float minimalZoom)
{
	if (sliderMaxValue <= 0 || minimalZoom <= 0.f || minimalZoom >= 1.f) return 1.f;

	const double part = std::clamp (value, 0, sliderMaxValue) / static_cast<double> (sliderMaxValue);
	return static_cast<float> (std::pow (static_cast<double> (minimalZoom), part));
}

//------------------------------------------------------------------------------
int computeSliderValueFromZoom (float zoom, int sliderMaxValue, float minimalZoom)
{
	if (sliderMaxValue <= 0 || minimalZoom <= 0.f || minimalZoom >= 1.f || zoom >= 1.f) return 0;
	if (zoom <= minimalZoom) return sliderMaxValue;

	const double part = std::log (static_cast<double> (zoom)) / std::log (static_cast<double> (minimalZoom));
	return std::clamp (static_cast<int> (std::lround (part * sliderMaxValue)), 0, sliderMaxValue);
}

//------------------------------------------------------------------------------
float computeNextZoom (float zoom, float minimalZoom, double fractionOfRange, bool zoomIn)
{
	const int minimalTileSize = std::clamp (toTileSize (minimalZoom), 1, mapZoomFullTileSize);
	if (minimalTileSize >= mapZoomFullTileSize) return 1.f;

	const int tileSize = std::clamp (toTileSize (zoom), minimalTileSize, mapZoomFullTileSize);
	const double step = std::max (0., fractionOfRange) * std::log (static_cast<double> (mapZoomFullTileSize) / minimalTileSize);
	const double logTileSize = std::log (static_cast<double> (tileSize)) + (zoomIn ? step : -step);

	int nextTileSize = static_cast<int> (std::lround (std::exp (logTileSize)));
	// at the far end one step of the logarithm is less than a pixel: at least one pixel
	if (zoomIn)
		nextTileSize = std::max (nextTileSize, tileSize + 1);
	else
		nextTileSize = std::min (nextTileSize, tileSize - 1);
	nextTileSize = std::clamp (nextTileSize, minimalTileSize, mapZoomFullTileSize);

	return nextTileSize / static_cast<float> (mapZoomFullTileSize);
}
