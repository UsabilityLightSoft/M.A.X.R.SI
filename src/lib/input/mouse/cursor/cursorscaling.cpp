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

#include <algorithm>
#include <cmath>

namespace
{
	//--------------------------------------------------------------------------
	bool isIntegerScale (double scale)
	{
		return std::abs (scale - std::round (scale)) < 1e-6;
	}

	//--------------------------------------------------------------------------
	/**
	 * Multiplies (premultiply) or divides (!premultiply) the color channels of an ARGB8888 surface by its alpha,
	 * so that smooth scaling does not mix the color of transparent pixels into the edges.
	 */
	void applyAlpha (SDL_Surface& surface, bool premultiply)
	{
		if (SDL_LockSurface (&surface) != 0) return;
		for (int y = 0; y < surface.h; ++y)
		{
			auto* row = reinterpret_cast<Uint32*> (static_cast<Uint8*> (surface.pixels) + y * surface.pitch);
			for (int x = 0; x < surface.w; ++x)
			{
				const Uint32 pixel = row[x];
				const Uint32 alpha = pixel >> 24;
				if (alpha == 0xFF) continue;
				if (alpha == 0)
				{
					row[x] = 0;
					continue;
				}
				Uint32 result = alpha << 24;
				for (int shift = 0; shift <= 16; shift += 8)
				{
					const Uint32 channel = (pixel >> shift) & 0xFF;
					const Uint32 value = premultiply ? (channel * alpha + 127) / 255 : std::min<Uint32> (0xFF, (channel * 255 + alpha / 2) / alpha);
					result |= value << shift;
				}
				row[x] = result;
			}
		}
		SDL_UnlockSurface (&surface);
	}

	//--------------------------------------------------------------------------
	UniqueSurface createArgbSurface (int width, int height)
	{
		return UniqueSurface (SDL_CreateRGBSurfaceWithFormat (0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
	}
} // namespace

//------------------------------------------------------------------------------
cPosition computeScaledCursorSize (const cPosition& imageSize, double scale)
{
	scale = std::max (1., scale);
	return cPosition (std::max (1, static_cast<int> (std::lround (imageSize.x() * scale))), std::max (1, static_cast<int> (std::lround (imageSize.y() * scale))));
}

//------------------------------------------------------------------------------
cPosition computeScaledCursorHotPoint (const cPosition& hotPoint, const cPosition& imageSize, const cPosition& scaledSize)
{
	const auto scaleAxis = [] (int hot, int size, int scaledSize) {
		if (size <= 0 || scaledSize <= 0) return 0;
		const double factor = static_cast<double> (scaledSize) / size;
		const int scaled = static_cast<int> (std::floor ((hot + 0.5) * factor));
		return std::clamp (scaled, 0, scaledSize - 1);
	};
	return cPosition (scaleAxis (hotPoint.x(), imageSize.x(), scaledSize.x()), scaleAxis (hotPoint.y(), imageSize.y(), scaledSize.y()));
}

//------------------------------------------------------------------------------
UniqueSurface makeScaledCursorSurface (SDL_Surface& image, double scale)
{
	// the conversion turns the color key into transparent pixels
	UniqueSurface converted (SDL_ConvertSurfaceFormat (&image, SDL_PIXELFORMAT_ARGB8888, 0));
	if (converted == nullptr) return nullptr;
	// copy all pixels as they are from now on
	SDL_SetColorKey (converted.get(), SDL_FALSE, 0);
	SDL_SetSurfaceBlendMode (converted.get(), SDL_BLENDMODE_NONE);

	scale = std::max (1., scale);
	const cPosition size = computeScaledCursorSize (cPosition (converted->w, converted->h), scale);
	if (size.x() == converted->w && size.y() == converted->h) return converted;

	// pixels as they are: by the factor itself, if it is an integer, else by the next integer
	const int integerScale = isIntegerScale (scale) ? static_cast<int> (std::lround (scale)) : static_cast<int> (std::ceil (scale));
	UniqueSurface enlarged = createArgbSurface (converted->w * integerScale, converted->h * integerScale);
	if (enlarged == nullptr) return converted;
	SDL_SetSurfaceBlendMode (enlarged.get(), SDL_BLENDMODE_NONE);
	if (SDL_BlitScaled (converted.get(), nullptr, enlarged.get(), nullptr) != 0) return converted;
	if (enlarged->w == size.x() && enlarged->h == size.y()) return enlarged;

	// then smoothly down to the size
	UniqueSurface scaled = createArgbSurface (size.x(), size.y());
	if (scaled == nullptr) return enlarged;
	applyAlpha (*enlarged, true);
	if (SDL_SoftStretchLinear (enlarged.get(), nullptr, scaled.get(), nullptr) != 0)
	{
		applyAlpha (*enlarged, false);
		return enlarged;
	}
	applyAlpha (*scaled, false);
	return scaled;
}
