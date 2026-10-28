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

#ifndef game_data_gui_mapzoomH
#define game_data_gui_mapzoomH

#include "utility/position.h"

/*
 * Zoom of the game map.
 *
 * A map tile is drawn with a whole number of pixels: from mapZoomFullTileSize (zoom factor 1, the nearest zoom)
 * down to the size at which the whole map fits into the map area on both axes (the farthest zoom).
 * A map smaller than the area is shown in the center of the area.
 */

/** Size of a map tile in pixels at zoom factor 1. */
constexpr int mapZoomFullTileSize = 64;

/**
 * The smallest tile size (in pixels) at which the whole map fits into the area on both axes:
 * at least 1 (a map with more tiles than the area has pixels can not fit), at most mapZoomFullTileSize.
 */
int computeMapFittingTileSize (const cPosition& areaSize, const cPosition& mapSize);

/**
 * Valid offset of the shown part of the map on one axis, in map pixels at zoom factor 1:
 * a map larger than the area is not left by the area;
 * a map smaller than the area is centered (the offset is negative then and can not be changed).
 */
int clampMapPixelOffset (int pixelOffset, int areaSize, int mapTiles, int tileSize);

/**
 * Distance in pixels on the screen from the left (upper) edge of the first map tile
 * to the left (upper) edge of the area: negative, if the map starts inside the area.
 * All tiles are moved by the same whole number of pixels.
 */
int computeZoomedMapOffset (int pixelOffset, int tileSize);

/**
 * The zoom slider has a logarithmic scale: the zoom factor changes by the same ratio along the whole slider.
 * Value 0 is zoom factor 1, value sliderMaxValue is the minimal zoom factor.
 */
float computeZoomFromSliderValue (int value, int sliderMaxValue, float minimalZoom);
int computeSliderValueFromZoom (float zoom, int sliderMaxValue, float minimalZoom);

/**
 * Zoom factor for zooming in or out by one step: the logarithm of the zoom factor changes by
 * the given fraction of the whole range (from the minimal zoom factor to 1),
 * and the tile size changes by at least one pixel (until a limit is reached).
 */
float computeNextZoom (float zoom, float minimalZoom, double fractionOfRange, bool zoomIn);

#endif // game_data_gui_mapzoomH
