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

#ifndef input_mouse_cursor_cursorscalingH
#define input_mouse_cursor_cursorscalingH

#include "SDLutility/uniquesurface.h"
#include "utility/position.h"

/**
 * The size of a cursor image enlarged by a factor: rounded, at least 1 pixel.
 */
cPosition computeScaledCursorSize (const cPosition& imageSize, double scale);

/**
 * The hot point of a cursor image enlarged to a size:
 * the center of the pixel of the hot point, enlarged in the same way as the image,
 * so that the hot point stays at the tip of the hand and in the center of the cross hairs for every factor.
 * Always inside of the enlarged image.
 */
cPosition computeScaledCursorHotPoint (const cPosition& hotPoint, const cPosition& imageSize, const cPosition& scaledSize);

/**
 * The cursor image enlarged by a factor, as an ARGB8888 surface (the color key becomes transparency).
 * Integer factors keep the pixels as they are.
 * Other factors enlarge the image first by the next integer factor (pixels as they are)
 * and then smoothly to the size ("sharp bilinear"): sharp edges without pixels of different width.
 * Factors below 1 are taken as 1.
 */
UniqueSurface makeScaledCursorSurface (SDL_Surface& image, double scale);

#endif // input_mouse_cursor_cursorscalingH
