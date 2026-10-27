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

#include "video.h"

#include "SDLutility/tosdl.h"
#include "SDLutility/uniquesurface.h"
#include "input/keyboard/keyboard.h"
#include "output/video/unifonts.h"
#include "resources/pcx.h"
#include "resources/uidata.h"
#include "utility/log.h"
#include "utility/mathtools.h"
#include "utility/narrow_cast.h"
#include "utility/os.h"
#include "utility/string/utf-8.h"
#include "utility/thread/ismainthread.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

cVideo Video;

/*static*/ SDL_Surface* cVideo::buffer = nullptr; // the screen buffer

#define COLOURDEPTH 32
/** Minimum video mode resolution we need */
#define MINWIDTH 640
#define MINHEIGHT 480

namespace
{
	/**
	 * An integer scale factor is used (sharp pixels, no filtering),
	 * if the picture covers at least this part of the size it would have with the fitting scale factor.
	 * Otherwise the picture is scaled smoothly to fit the window.
	 */
	constexpr double integerScaleCoverage = 0.9;

	/** The automatic scale of the user interface is this factor times the scale of the display (DPI / 96). */
	constexpr double autoUiScalePerDisplayScale = 2.4;
	/** The automatic scale leaves a picture of at least this size (up to the tolerance less) ... */
	constexpr int autoUiScaleMinimalWidth = 1280;
	constexpr int autoUiScaleMinimalHeight = 720;
	/** ... a maximized window loses some height to the title bar and the task bar (4K at 125%: 2071 instead of 2160). */
	constexpr double autoUiScaleTolerance = 0.9;
	/** The scales of the user interface are multiples of this step. */
	constexpr double uiScaleStep = 0.5;
	/** The screen buffer is recreated for a new window size not more often than this (milliseconds). */
	constexpr Uint32 pictureSizeUpdateInterval = 150;
	/** The default size of a window that is not maximized is a multiple of this size. */
	constexpr int defaultWindowUnitWidth = 1280;
	constexpr int defaultWindowUnitHeight = 720;

	//--------------------------------------------------------------------------
	bool isPictureAtLeast (const cPosition& pictureSize, double minimalWidth, double minimalHeight)
	{
		return pictureSize.x() >= minimalWidth && pictureSize.y() >= minimalHeight;
	}
} // namespace

//------------------------------------------------------------------------------
cVideo::cVideo() :
	resolutionX (MINWIDTH),
	resolutionY (MINHEIGHT),
	displayIndex (MINWIDTH),
	colorDepth (COLOURDEPTH)
{
	signalConnectionManager.connect (cKeyboard::getInstance().keyPressed, [this] (cKeyboard& keyboard, SDL_Keycode key) { keyPressed (keyboard, key); });
}

//------------------------------------------------------------------------------
cVideo::~cVideo()
{
	SDL_FreeSurface (cVideo::buffer);
	SDL_DestroyTexture (scaledTexture);
	SDL_DestroyTexture (sdlTexture);
	SDL_DestroyRenderer (sdlRenderer);
	SDL_DestroyWindow (sdlWindow);
}

//------------------------------------------------------------------------------
void cVideo::clearMemory()
{
	SDL_FreeSurface (cVideo::buffer);
	SDL_DestroyTexture (scaledTexture);
	SDL_DestroyTexture (sdlTexture);
	SDL_DestroyRenderer (sdlRenderer);
	SDL_DestroyWindow (sdlWindow);
	cVideo::buffer = nullptr;
	scaledTexture = nullptr;
	sdlTexture = nullptr;
	sdlRenderer = nullptr;
	sdlWindow = nullptr;
}

//------------------------------------------------------------------------------
void cVideo::init (std::shared_ptr<SDLComponent> sdlComponent, const std::string& title, const std::filesystem::path& iconPath)
{
	this->sdlComponent = std::move (sdlComponent);
	// No SDL_WINDOW_OPENGL: the renderer chooses the graphics API
	// (and recreates the window, if it needs an OpenGL window).
	sdlWindow = SDL_CreateWindow (title.c_str(),
	                              SDL_WINDOWPOS_CENTERED_DISPLAY (getDisplayIndex()),
	                              SDL_WINDOWPOS_CENTERED_DISPLAY (getDisplayIndex()),
	                              MINWIDTH,
	                              MINHEIGHT,
	                              SDL_WINDOW_BORDERLESS);
	if (sdlWindow == nullptr)
	{
		throw std::runtime_error (std::string ("Could not create window: ") + SDL_GetError());
	}

	{
		auto icon = UniqueSurface (SDL_LoadBMP (utf8::to_string (iconPath).c_str()));
		SDL_SetColorKey (icon.get(), 1, 0xFF00FF);
		SDL_SetWindowIcon (sdlWindow, icon.get());
	}

	sdlRenderer = SDL_CreateRenderer (sdlWindow, -1, 0);
	if (sdlRenderer == nullptr)
	{
		throw std::runtime_error (std::string ("Could not create renderer: ") + SDL_GetError());
	}

	// make the scaled rendering look smoother.
	// (The screen textures choose their scaling themselves, see draw.)
	SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "linear");

	SDL_RendererInfo rendererInfo;
	if (SDL_GetRendererInfo (sdlRenderer, &rendererInfo) == 0)
	{
		Log.info (std::string ("cVideo: Renderer ") + rendererInfo.name + ", maximal texture size " + std::to_string (rendererInfo.max_texture_width) + "x" + std::to_string (rendererInfo.max_texture_height));
		maximalTextureSize = cPosition (rendererInfo.max_texture_width, rendererInfo.max_texture_height);
	}
	sharpScalingAvailable = SDL_RenderTargetSupported (sdlRenderer);
}

//------------------------------------------------------------------------------
void cVideo::showSplashScreen (const std::filesystem::path& splashScreenPath)
{
	UniqueSurface splash (LoadPCX (splashScreenPath));

	// enlarge the splash screen on big displays: to about half of the display size
	int scale = 1;
	SDL_Rect displayBounds;
	if (SDL_GetDisplayBounds (std::max (0, SDL_GetWindowDisplayIndex (sdlWindow)), &displayBounds) == 0 && splash->w > 0 && splash->h > 0)
	{
		scale = std::max (1, std::min (displayBounds.w / splash->w, displayBounds.h / splash->h) / 2);
	}

	SDL_SetWindowBordered (sdlWindow, SDL_FALSE);
	SDL_SetWindowSize (sdlWindow, splash->w * scale, splash->h * scale);
	SDL_SetWindowPosition (sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_SetWindowFullscreen (sdlWindow, 0);

	initializeBuffer (splash->w, splash->h);

	SDL_BlitSurface (splash.get(), nullptr, buffer, nullptr);

	draw();
}

//------------------------------------------------------------------------------
void cVideo::prepareGameScreen()
{
	gameScreenPrepared = true;
	SDL_SetWindowBordered (sdlWindow, SDL_TRUE);
	SDL_SetWindowResizable (sdlWindow, SDL_TRUE);
	SDL_SetWindowMinimumSize (sdlWindow, MINWIDTH, MINHEIGHT);

	const auto maximalSize = getMaximalWindowSize();
	auto size = windowedSize.value_or (getDefaultWindowSize());
	size.x() = std::clamp (size.x(), MINWIDTH, std::max (MINWIDTH, maximalSize.x()));
	size.y() = std::clamp (size.y(), MINHEIGHT, std::max (MINHEIGHT, maximalSize.y()));
	windowedSize = size;

	SDL_SetWindowSize (sdlWindow, size.x(), size.y());
	SDL_SetWindowPosition (sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	if (windowMaximized)
	{
		SDL_MaximizeWindow (sdlWindow);
	}
	applyWindowMode();
	storeWindowGeometry (cSettings::getInstance().getVideoSettings());

	// the game picture: window size / scale of the user interface
	applyPictureSize (true);
	pictureSizeOutdated = false;
	lastPictureSizeUpdate = SDL_GetTicks();

	draw();
}

//------------------------------------------------------------------------------
void cVideo::setUiScale (int percent)
{
	uiScale = std::max (0, percent);
	// a changed setting is applied with the next frame, without waiting
	pictureSizeOutdated = true;
	lastPictureSizeUpdate = SDL_GetTicks() - pictureSizeUpdateInterval;
}

//------------------------------------------------------------------------------
cPosition cVideo::getOutputSize() const
{
	int width = 0;
	int height = 0;
	if (sdlRenderer) SDL_GetRendererOutputSize (sdlRenderer, &width, &height);
	return cPosition (width, height);
}

//------------------------------------------------------------------------------
double cVideo::getDisplayScale() const
{
	const int index = sdlWindow ? SDL_GetWindowDisplayIndex (sdlWindow) : getDisplayIndex();
	float dpi = 0.f;
	if (SDL_GetDisplayDPI (std::max (0, index), nullptr, &dpi, nullptr) != 0 || !(dpi > 0.f))
	{
		return 1.; // unknown, e.g. no real display
	}
	return dpi / 96.;
}

//------------------------------------------------------------------------------
void cVideo::updatePictureSize()
{
	if (!pictureSizeOutdated || !gameScreenPrepared) return;

	const Uint32 now = SDL_GetTicks();
	if (now - lastPictureSizeUpdate < pictureSizeUpdateInterval) return;

	pictureSizeOutdated = false;
	lastPictureSizeUpdate = now;
	applyPictureSize (false);
}

//------------------------------------------------------------------------------
bool cVideo::applyPictureSize (bool force)
{
	const auto outputSize = getOutputSize();
	if (!force && (outputSize.x() <= 0 || outputSize.y() <= 0))
	{
		// e.g. minimized: keep the picture until the window is restored
		return false;
	}
	const auto displayScale = getDisplayScale();
	uiScaleFactor = computeUiScale (uiScale, outputSize, displayScale);
	const auto size = computePictureSize (outputSize, uiScaleFactor);

	if (!force && buffer != nullptr && buffer->w == size.x() && buffer->h == size.y())
	{
		return false;
	}
	Log.info ("cVideo: Picture " + std::to_string (size.x()) + "x" + std::to_string (size.y()) + " for window " + std::to_string (outputSize.x()) + "x" + std::to_string (outputSize.y()) + ", user interface scale " + std::to_string (uiScaleFactor) + " (setting " + std::to_string (uiScale) + "%, display " + std::to_string (displayScale) + ")");

	resolutionX = size.x();
	resolutionY = size.y();
	initializeBuffer (size.x(), size.y());

	resolutionChanged();
	return true;
}

//------------------------------------------------------------------------------
void cVideo::initializeBuffer (int width, int height)
{
	if (buffer) SDL_FreeSurface (buffer);
	buffer = SDL_CreateRGBSurface (0, width, height, getColDepth(), 0, 0, 0, 0);

	if (cUnicodeFont::font != nullptr)
		cUnicodeFont::font->setTargetSurface (buffer);

	if (sdlTexture) SDL_DestroyTexture (sdlTexture);
	sdlTexture = SDL_CreateTexture (sdlRenderer,
	                                SDL_PIXELFORMAT_ARGB8888,
	                                SDL_TEXTUREACCESS_STREAMING,
	                                width,
	                                height);
	// the buffer is opaque: its alpha byte must not be used
	SDL_SetTextureBlendMode (sdlTexture, SDL_BLENDMODE_NONE);

	viewArea.reset();
}

//------------------------------------------------------------------------------
void cVideo::setColDepth (unsigned int iDepth)
{
	// TODO: Implement other colourdepths beside 32 & add sanity checks.
	//       validate new color depth
	if (iDepth != 32)
	{
		Log.warn ("cVideo: TODO: Implement other colourdepths beside 32. Desired " + std::to_string (iDepth) + "bpp ignored.");
	}
	else
	{
		// TODO: [SDL2] : sanitycheck
		colorDepth = iDepth;
	}
}

//------------------------------------------------------------------------------
void cVideo::setDisplayIndex (int index)
{
	displayIndex = index;
	// TODO: apply the display index (reassign window to new display)
}

//------------------------------------------------------------------------------
void cVideo::setWindowMode (bool bWindowMode, bool bApply)
{
	windowMode = bWindowMode;
	Log.debug ("cVideo: Window mode settings changed to " + std::string (getWindowMode() ? "windowmode" : "fullscreen"));

	if (bApply)
	{
		applyWindowMode();
	}
}

//------------------------------------------------------------------------------
void cVideo::draw()
{
	// SDL2: Some SDL2 functions must be called from the main thread only
	// at least in some platform/configuration.
	// Check for all configurations.
	assert (is_main_thread());

	// TODO: add sanity check to redraw function

	SDL_UpdateTexture (sdlTexture, nullptr, buffer->pixels, buffer->pitch);

	updatePresentation();

	SDL_Texture* texture = sdlTexture;
	SDL_Rect source = presentation.source;
	if (presentation.prescale > 1)
	{
		// sharp bilinear: enlarge by an integer factor without filtering, then scale smoothly to the window
		const int width = source.w * presentation.prescale;
		const int height = source.h * presentation.prescale;
		if (auto* scaled = getScaledTexture (width, height))
		{
			SDL_SetTextureScaleMode (sdlTexture, SDL_ScaleModeNearest);
			SDL_SetRenderTarget (sdlRenderer, scaled);
			SDL_RenderCopy (sdlRenderer, sdlTexture, &source, nullptr);
			SDL_SetRenderTarget (sdlRenderer, nullptr);
			texture = scaled;
			source = {0, 0, width, height};
		}
	}
	SDL_SetTextureScaleMode (texture, presentation.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
	SDL_SetRenderDrawColor (sdlRenderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear (sdlRenderer);
	SDL_RenderCopy (sdlRenderer, texture, &source, &presentation.destination);
	SDL_RenderPresent (sdlRenderer);
}

//------------------------------------------------------------------------------
void cVideo::applyWindowMode()
{
	// Fullscreen covers the desktop without changing the video mode of the display:
	// switching the video mode messes up other windows and makes switching to other applications fragile.
	const auto result = SDL_SetWindowFullscreen (sdlWindow, getWindowMode() ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
	if (result == -1)
	{
		throw std::runtime_error (std::string ("Could not apply window mode: ") + SDL_GetError() + "'");
	}
}

//------------------------------------------------------------------------------
void cVideo::setWindowGeometry (std::optional<cPosition> size, bool maximized)
{
	windowedSize = size;
	windowMaximized = maximized;
}

//------------------------------------------------------------------------------
void cVideo::storeWindowGeometry (sVideoSettings& videoSettings) const
{
	videoSettings.windowSize = windowedSize;
	videoSettings.windowMaximized = windowMaximized;
}

//------------------------------------------------------------------------------
void cVideo::handleWindowEvent (const SDL_WindowEvent& event)
{
	if (sdlWindow == nullptr || event.windowID != SDL_GetWindowID (sdlWindow)) return;

	switch (event.event)
	{
		case SDL_WINDOWEVENT_SIZE_CHANGED:
		case SDL_WINDOWEVENT_MAXIMIZED:
		case SDL_WINDOWEVENT_RESTORED:
		{
			// the splash screen is not the game window
			if (!gameScreenPrepared) break;
			// the game picture follows the window size (also in fullscreen)
			pictureSizeOutdated = true;
			const auto flags = SDL_GetWindowFlags (sdlWindow);
			if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) != 0) break;

			windowMaximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
			if (!windowMaximized)
			{
				int width = 0;
				int height = 0;
				SDL_GetWindowSize (sdlWindow, &width, &height);
				if (width > 0 && height > 0) windowedSize = cPosition (width, height);
			}
			// remembered in the settings, saved with them
			storeWindowGeometry (cSettings::getInstance().getVideoSettings());
			break;
		}
#if SDL_VERSION_ATLEAST(2, 0, 18)
		case SDL_WINDOWEVENT_DISPLAY_CHANGED:
			// another display may have another scale
			pictureSizeOutdated = true;
			break;
#endif
		default:
			break;
	}
}

//------------------------------------------------------------------------------
cPosition cVideo::getMaximalWindowSize() const
{
	const int index = sdlWindow ? SDL_GetWindowDisplayIndex (sdlWindow) : getDisplayIndex();
	SDL_Rect usableBounds;
	if (SDL_GetDisplayUsableBounds (std::max (0, index), &usableBounds) != 0)
	{
		return cPosition (getResolutionX(), getResolutionY());
	}
	int top = 0;
	int left = 0;
	int bottom = 0;
	int right = 0;
	if (sdlWindow == nullptr || SDL_GetWindowBordersSize (sdlWindow, &top, &left, &bottom, &right) != 0)
	{
		top = left = bottom = right = 0;
	}
	return cPosition (usableBounds.w - left - right, usableBounds.h - top - bottom);
}

//------------------------------------------------------------------------------
cPosition cVideo::getDefaultWindowSize() const
{
	// the biggest integer multiple of 1280x720, that leaves some space on the display
	const auto maximalSize = getMaximalWindowSize();
	const cPosition limit (maximalSize.x() * 9 / 10, maximalSize.y() * 9 / 10);
	const int width = defaultWindowUnitWidth;
	const int height = defaultWindowUnitHeight;
	const int factor = std::min (limit.x() / width, limit.y() / height);
	if (factor >= 1)
	{
		return cPosition (width * factor, height * factor);
	}
	const double scale = std::min (static_cast<double> (limit.x()) / width, static_cast<double> (limit.y()) / height);
	return cPosition (std::max (MINWIDTH, static_cast<int> (width * scale)), std::max (MINHEIGHT, static_cast<int> (height * scale)));
}

//------------------------------------------------------------------------------
void cVideo::setViewArea (const cBox<cPosition>& area)
{
	if (buffer == nullptr) return;

	const SDL_Rect bufferRect{0, 0, buffer->w, buffer->h};
	const SDL_Rect rect = computeViewArea (toSdlRect (area), cPosition (buffer->w, buffer->h), cPosition (MINWIDTH, MINHEIGHT));

	if (rect.w <= 0 || rect.h <= 0 || SDL_RectEquals (&rect, &bufferRect))
		viewArea.reset();
	else
		viewArea = rect;
}

//------------------------------------------------------------------------------
cPosition cVideo::windowToBuffer (const cPosition& position) const
{
	return windowToPicturePosition (position, presentation.source, presentation.destination, presentation.windowSize, presentation.outputSize);
}

//------------------------------------------------------------------------------
void cVideo::updatePresentation()
{
	if (buffer == nullptr || sdlRenderer == nullptr || sdlWindow == nullptr) return;

	sPresentation next;
	next.source = viewArea.value_or (SDL_Rect{0, 0, buffer->w, buffer->h});

	int outputWidth = 0;
	int outputHeight = 0;
	SDL_GetRendererOutputSize (sdlRenderer, &outputWidth, &outputHeight);
	int windowWidth = 0;
	int windowHeight = 0;
	SDL_GetWindowSize (sdlWindow, &windowWidth, &windowHeight);
	if (outputWidth <= 0 || outputHeight <= 0 || windowWidth <= 0 || windowHeight <= 0 || next.source.w <= 0 || next.source.h <= 0)
	{
		// e.g. minimized: keep the last presentation
		return;
	}
	next.outputSize = cPosition (outputWidth, outputHeight);
	next.windowSize = cPosition (windowWidth, windowHeight);

	const auto scaling = computeViewScaling (cPosition (next.source.w, next.source.h), next.outputSize, sharpScalingAvailable, maximalTextureSize);
	next.destination = scaling.destination;
	next.prescale = scaling.prescale;
	next.smooth = scaling.smooth;

	if (!SDL_RectEquals (&next.source, &presentation.source) || !SDL_RectEquals (&next.destination, &presentation.destination) || next.windowSize != presentation.windowSize || next.outputSize != presentation.outputSize)
	{
		++presentationVersion;
	}
	presentation = next;

	const int newCursorScale = std::max (1, static_cast<int> (std::lround (scaling.scale * windowWidth / outputWidth)));
	if (newCursorScale != cursorScale)
	{
		cursorScale = newCursorScale;
		cursorScaleChanged();
	}
}

//------------------------------------------------------------------------------
SDL_Texture* cVideo::getScaledTexture (int width, int height)
{
	if (scaledTexture)
	{
		int textureWidth = 0;
		int textureHeight = 0;
		SDL_QueryTexture (scaledTexture, nullptr, nullptr, &textureWidth, &textureHeight);
		if (textureWidth == width && textureHeight == height) return scaledTexture;
		SDL_DestroyTexture (scaledTexture);
		scaledTexture = nullptr;
	}
	scaledTexture = SDL_CreateTexture (sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, width, height);
	if (scaledTexture == nullptr)
	{
		Log.warn (std::string ("cVideo: Could not create texture for scaling, using simple smooth scaling: ") + SDL_GetError());
		sharpScalingAvailable = false;
		return nullptr;
	}
	SDL_SetTextureBlendMode (scaledTexture, SDL_BLENDMODE_NONE);
	return scaledTexture;
}

//------------------------------------------------------------------------------
void cVideo::clearBuffer()
{
	SDL_FillRect (buffer, nullptr, toSdlColor (cRgbColor::black(), *buffer));
}

//------------------------------------------------------------------------------
int cVideo::getMinW() const
{
	return MINWIDTH;
}

//------------------------------------------------------------------------------
int cVideo::getMinH() const
{
	return MINHEIGHT;
}

//------------------------------------------------------------------------------
void cVideo::takeScreenShot (const std::filesystem::path& filename) const
{
	SDL_SaveBMP (buffer, utf8::to_string (filename).c_str());
}

//------------------------------------------------------------------------------
void cVideo::keyPressed (cKeyboard& keyboard, SDL_Keycode key)
{
	if (keyboard.isAnyModifierActive (toEnumFlag (eKeyModifierType::AltLeft) | toEnumFlag (eKeyModifierType::AltRight)))
	{
		if (key == SDLK_RETURN)
		{
			setWindowMode (!getWindowMode(), true);

			auto& settings = cSettings::getInstance();
			settings.getVideoSettings().windowMode = getWindowMode();
			settings.saveInFile();
		}
		else if (key == SDLK_c)
		{
			const auto timestr = os::formattedNow ("screenie_%Y-%m-%d_%H%M%S_");
			std::filesystem::path screenshotfile;
			int counter = 0;
			const auto screenshotDir = cSettings::getInstance().getUserScreenshotsDir();
			do
			{
				counter += 1;
				screenshotfile = screenshotDir / (timestr + std::to_string (counter) + ".bmp");
			} while (std::filesystem::exists (screenshotfile));
			std::filesystem::create_directories (screenshotDir);
			Log.info ("Screenshot saved to " + utf8::to_string (screenshotfile));
			takeScreenShot (screenshotfile);

			screenShotTaken (screenshotfile);
		}
	}
}

//------------------------------------------------------------------------------
void cVideo::applyShadow (const SDL_Rect* rect, SDL_Surface& destination)
{
	const SDL_Rect fullscreen = {0, 0, getResolutionX(), getResolutionY()};
	if (rect == nullptr) rect = &fullscreen;
	SDL_Rect dest = {rect->x, rect->y, rect->w, rect->h};
	const UniqueRenderer renderer{SDL_CreateSoftwareRenderer (&destination)};
	const UniqueTexture texture{SDL_CreateTextureFromSurface (renderer.get(), GraphicsData.gfx_shadow.get())};
	SDL_RenderCopy (renderer.get(), texture.get(), nullptr, &dest);
}

//------------------------------------------------------------------------------
void blittPerSurfaceAlphaToAlphaChannel (SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect)
{
	SDL_Rect temp1, temp2;

	if (!dst || !src) return;

	// check surface formats
	if (!dst->format->Amask || src->format->Amask) return;
	if (SDL_GetSurfaceAlphaMod (src, nullptr) != 0) return;

	if (srcrect == nullptr)
	{
		srcrect = &temp1;
		srcrect->x = 0;
		srcrect->y = 0;
		srcrect->h = src->h;
		srcrect->w = src->w;
	}

	if (dstrect == nullptr)
	{
		dstrect = &temp2;
		dstrect->x = 0;
		dstrect->y = 0;
	}

	int width = srcrect->w;
	int height = srcrect->h;

	//clip source rect to the source surface
	if (srcrect->x < 0)
	{
		width += srcrect->x;
		srcrect->x = 0;
	}
	if (srcrect->y < 0)
	{
		height += srcrect->y;
		srcrect->y = 0;
	}
	width = std::min (src->w - srcrect->x, width);
	height = std::min (src->h - srcrect->y, height);

	// clip the dest rect to the destination clip rect
	if (dstrect->x < dst->clip_rect.x)
	{
		width -= dst->clip_rect.x - dstrect->x;
		srcrect->x += dst->clip_rect.x - dstrect->x;
		dstrect->x = dst->clip_rect.x;
	}
	if (dstrect->y < dst->clip_rect.y)
	{
		height -= dst->clip_rect.y - dstrect->y;
		srcrect->y += dst->clip_rect.y - dstrect->y;
		dstrect->y = dst->clip_rect.y;
	}
	width = std::min (dst->clip_rect.x + dst->clip_rect.w - dstrect->x, width);
	height = std::min (dst->clip_rect.y + dst->clip_rect.h - dstrect->y, height);

	if (width <= 0 || height <= 0)
	{
		dstrect->w = 0;
		dstrect->h = 0;
		return;
	}

	if (SDL_MUSTLOCK (src)) SDL_LockSurface (src);
	if (SDL_MUSTLOCK (dst)) SDL_LockSurface (dst);

	//setup needed variables
	Uint8 srcAlpha = 0;
	SDL_GetSurfaceAlphaMod (src, &srcAlpha);
	const int srmask = src->format->Rmask;
	const int sgmask = src->format->Gmask;
	const int sbmask = src->format->Bmask;
	const int damask = dst->format->Amask;
	const int drmask = dst->format->Rmask;
	const int dgmask = dst->format->Gmask;
	const int dbmask = dst->format->Bmask;
	const int rshift = src->format->Rshift - dst->format->Rshift;
	const int gshift = src->format->Gshift - dst->format->Gshift;
	const int bshift = src->format->Bshift - dst->format->Bshift;
	const int ashift = dst->format->Ashift;
	Uint32 colorKey = 0;
	const bool useColorKey = SDL_GetColorKey (src, &colorKey) == 0;

	Uint32* dstPixel = static_cast<Uint32*> (dst->pixels) + dstrect->x + dstrect->y * dst->w;
	Uint32* srcPixel = static_cast<Uint32*> (src->pixels) + srcrect->x + srcrect->y * src->w;

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			const Uint32 dcolor = *dstPixel;
			const Uint32 scolor = *srcPixel;

			if (useColorKey && scolor == colorKey)
			{
				dstPixel++;
				srcPixel++;
				continue;
			}

			Uint32 r = (scolor & srmask) >> rshift;
			Uint32 g = (scolor & sgmask) >> gshift;
			Uint32 b = (scolor & sbmask) >> bshift;
			Uint32 dalpha = (dcolor & damask) >> ashift;

			r = (((dcolor & drmask) >> 8) * (255 - srcAlpha) * dalpha) + r * srcAlpha;
			g = (((dcolor & dgmask) * (255 - srcAlpha) * dalpha) >> 8) + g * srcAlpha;
			b = (((dcolor & dbmask) * (255 - srcAlpha) * dalpha) >> 8) + b * srcAlpha;

			const Uint8 a = narrow_cast<Uint8> (srcAlpha + dalpha - (srcAlpha * dalpha) / 255);

			if (a > 0)
			{
				r /= a;
				g /= a;
				b /= a;
			}

			r = r & drmask;
			g = g & dgmask;
			b = b & dbmask;

			*dstPixel = r | g | b | a << ashift;

			dstPixel++;
			srcPixel++;
		}

		dstPixel += dst->pitch / 4 - width;
		srcPixel += src->pitch / 4 - width;
	}

	if (SDL_MUSTLOCK (src)) SDL_UnlockSurface (src);
	if (SDL_MUSTLOCK (dst)) SDL_UnlockSurface (dst);
}

//------------------------------------------------------------------------------
void blittAlphaSurface (SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect)
{
	// TODO: [SDL2] special blitSurface seems useless.
	if (dst->format->Amask && SDL_GetSurfaceAlphaMod (src, nullptr) == 0)
		blittPerSurfaceAlphaToAlphaChannel (src, srcrect, dst, dstrect);
	else
		SDL_BlitSurface (src, srcrect, dst, dstrect);
}

/**
 * draws one line of the source surface scaled to the destination surface.
 * @author alzi alias DoctorDeath
 * @param srcPixelData pointer to the first byte of the line in the pixeldate
 *        of the source surface.
 * @param srcWidth width of the line in the sourcesurface.
 * @param destPixelData pointer to the first byte where in the pixeldate
 *        of the source surface the line should be drawn.
 * @param destWidth Directory width of the line how it should be drawn
 *        to the destination surface.
 */
template <typename Type>
static void drawStetchedLine (Type* srcPixelData, int srcWidth, Type* destPixelData, int destWidth)
{
	int i = 0;
	int width = destWidth;
	Type pixel = 0;
	Type* srcEnd = srcPixelData + srcWidth;
	// go trough all pixel in this line
	while (srcPixelData < srcEnd)
	{
		pixel = *srcPixelData;
drawpixel:
		// copy the pixel
		*destPixelData++ = pixel;
		width--;
		i += srcWidth;
		if (!width) break;
		// draw pixel once more if necessary
		if (i < destWidth) goto drawpixel;
		// skip pixels when necessary
		do
		{
			i -= destWidth;
			srcPixelData++;
		} while (i >= destWidth);
	}
}

//------------------------------------------------------------------------------
SDL_Surface* scaleSurface (SDL_Surface* scr, SDL_Surface* dest, int width, int height)
{
	if (width <= 0 || height <= 0 || !scr) return nullptr;

	// can not enlage an existing surface
	if (width > scr->w && dest) width = scr->w;
	if (height > scr->h && dest) height = scr->h;

	SDL_Surface* surface = nullptr;
	// generate new surface if necessary
	if (dest == nullptr)
		surface = SDL_CreateRGBSurface (0, width, height, scr->format->BitsPerPixel, scr->format->Rmask, scr->format->Gmask, scr->format->Bmask, scr->format->Amask);
	else
	{
		// else set the size of the old one
		surface = dest;
		surface->w = width;
		surface->h = height;
	}

	if (SDL_MUSTLOCK (scr)) SDL_LockSurface (scr);
	if (SDL_MUSTLOCK (surface)) SDL_LockSurface (surface);

#if 0
	// just blit the surface when the new size is identic to the old one
	if (scr->w == width && scr->h == height)
	{
		SDL_BlitSurface (scr, nullptr, surface, nullptr);
		return surface;
	}
#endif
	// copy palette when necessary
	if (scr->format->BitsPerPixel == 8 && !dest)
	{
		for (int i = 0; i < 256; i++)
		{
			surface->format->palette->colors[i].r = scr->format->palette->colors[i].r;
			surface->format->palette->colors[i].g = scr->format->palette->colors[i].g;
			surface->format->palette->colors[i].b = scr->format->palette->colors[i].b;
		}
	}

	int srcRow = 0;
	int destRow = 0;
	int i = 0;
	// go trough all rows
	while (srcRow < scr->h)
	{
		Uint8* srcPixelData = static_cast<Uint8*> (scr->pixels) + (srcRow * scr->pitch);
		// draw the complete line
drawline:
		Uint8* destPixelData = static_cast<Uint8*> (surface->pixels) + (destRow * surface->pitch);

		// pay attention to different surface formats
		switch (scr->format->BytesPerPixel)
		{
			case 1:
				drawStetchedLine<Uint8> (srcPixelData, scr->w, destPixelData, surface->w);
				break;
			case 2:
				drawStetchedLine<Uint16> (reinterpret_cast<Uint16*> (srcPixelData), scr->w, (Uint16*) destPixelData, surface->w);
				break;
			case 3:
				// not yet supported
				break;
			case 4:
				drawStetchedLine<Uint32> (reinterpret_cast<Uint32*> (srcPixelData), scr->w, (Uint32*) destPixelData, surface->w);
				break;
		}
		destRow++;
		i += scr->h;
		// break when we have already finished
		if (destRow == surface->h) break;
		// draw the line once more when the destiniation surface has
		// a bigger height than the source surface
		if (i < surface->h) goto drawline;
		// skip lines in the source surface when the destination surface has
		// a smaller height than the source surface
		do
		{
			i -= surface->h;
			srcRow++;
		} while (i >= surface->h);
	}

	if (SDL_MUSTLOCK (scr)) SDL_UnlockSurface (scr);
	if (SDL_MUSTLOCK (surface)) SDL_UnlockSurface (surface);

	return surface;
}

//------------------------------------------------------------------------------
static void line (int x1, int y1, int x2, int y2, unsigned int color, SDL_Surface& sf)
{
	if (x2 < x1)
	{
		std::swap (x1, x2);
		std::swap (y1, y2);
	}
	int dx = x2 - x1;
	int dy = y2 - y1;
	int dir = 1;
	if (dy < 0)
	{
		dy = -dy;
		dir = -1;
	}
	int error = 0;
	Uint32* ptr = static_cast<Uint32*> (sf.pixels);
	if (dx > dy)
	{
		for (; x1 != x2; x1++, error += dy)
		{
			if (error > dx)
			{
				error -= dx;
				y1 += dir;
			}
			if (x1 < sf.w && x1 >= 0 && y1 >= 0 && y1 < sf.h)
				ptr[x1 + y1 * sf.w] = color;
		}
		return;
	}
	for (; y1 != y2; y1 += dir, error += dx)
	{
		if (error > dy)
		{
			error -= dy;
			x1++;
		}
		if (x1 < sf.w && x1 >= 0 && y1 >= 0 && y1 < sf.h)
			ptr[x1 + y1 * sf.w] = color;
	}
}

//------------------------------------------------------------------------------
// CreatePfeil ////////////////////////////////////////////////////////////////
// Erzeigt ein Pfeil-Surface:
UniqueSurface CreatePfeil (int p1x, int p1y, int p2x, int p2y, int p3x, int p3y, unsigned int color, int size)
{
	UniqueSurface sf (SDL_CreateRGBSurface (0, size, size, Video.getColDepth(), 0, 0, 0, 0));
	SDL_SetColorKey (sf.get(), SDL_TRUE, 0x00FF00FF);
	SDL_FillRect (sf.get(), nullptr, 0x00FF00FF);
	SDL_LockSurface (sf.get());

	const float fak = size / 64.0f;
	p1x = Round (p1x * fak);
	p1y = Round (p1y * fak);
	p2x = Round (p2x * fak);
	p2y = Round (p2y * fak);
	p3x = Round (p3x * fak);
	p3y = Round (p3y * fak);
	line (p1x, p1y, p2x, p2y, color, *sf);
	line (p2x, p2y, p3x, p3y, color, *sf);
	line (p3x, p3y, p1x, p1y, color, *sf);

	SDL_UnlockSurface (sf.get());
	return sf;
}

//------------------------------------------------------------------------------
static void setPixel (SDL_Surface& surface, int x, int y, int iColor)
{
	// check the surface size
	if (x < 0 || x >= surface.w || y < 0 || y >= surface.h) return;
	// check the clip rect
	if (x < surface.clip_rect.x || x >= surface.clip_rect.x + surface.clip_rect.w || y < surface.clip_rect.y || y >= surface.clip_rect.y + surface.clip_rect.h) return;

	static_cast<Uint32*> (surface.pixels)[x + y * surface.w] = iColor;
}

//------------------------------------------------------------------------------
void drawCircle (int iX, int iY, int iRadius, int iColor, SDL_Surface& surface)
{
	if (iX + iRadius < 0 || iX - iRadius > Video.getResolutionX() || iY + iRadius < 0 || iY - iRadius > Video.getResolutionY()) return;
	SDL_LockSurface (&surface);

	int d = 0;
	int xx = 0;
	int yy = iRadius;
	int bry = Round (0.70710678f * iRadius);
	while (yy > bry)
	{
		int da = d + (xx << 1) + 1;
		int db = da - (yy << 1) + 1;
		if (abs (da) < abs (db))
		{
			d = da;
			xx++;
		}
		else
		{
			d = db;
			xx++;
			yy--;
		}
		setPixel (surface, iX + xx, iY + yy, iColor);
		setPixel (surface, iX + yy, iY + xx, iColor);
		setPixel (surface, iX + yy, iY - xx, iColor);
		setPixel (surface, iX + xx, iY - yy, iColor);
		setPixel (surface, iX - xx, iY + yy, iColor);
		setPixel (surface, iX - yy, iY + xx, iColor);
		setPixel (surface, iX - yy, iY - xx, iColor);
		setPixel (surface, iX - xx, iY - yy, iColor);
	}
	SDL_UnlockSurface (&surface);
}

//------------------------------------------------------------------------------
sViewScaling computeViewScaling (const cPosition& pictureSize, const cPosition& outputSize, bool prescaleAvailable, const cPosition& maximalTextureSize)
{
	sViewScaling result;
	if (pictureSize.x() <= 0 || pictureSize.y() <= 0 || outputSize.x() <= 0 || outputSize.y() <= 0) return result;

	const double fittingScale = std::min (static_cast<double> (outputSize.x()) / pictureSize.x(), static_cast<double> (outputSize.y()) / pictureSize.y());
	const double integerScale = std::floor (fittingScale);
	if (integerScale >= 1 && integerScale >= fittingScale * integerScaleCoverage)
	{
		result.scale = integerScale;
		result.smooth = false;
	}
	else
	{
		result.scale = fittingScale;
		result.smooth = true;
		if (fittingScale > 1 && prescaleAvailable)
		{
			int prescale = static_cast<int> (std::ceil (fittingScale));
			if (maximalTextureSize.x() > 0 && maximalTextureSize.y() > 0)
			{
				prescale = std::min ({prescale, maximalTextureSize.x() / pictureSize.x(), maximalTextureSize.y() / pictureSize.y()});
			}
			result.prescale = prescale >= 2 ? prescale : 1;
		}
	}
	const int width = std::clamp (static_cast<int> (std::lround (pictureSize.x() * result.scale)), 1, outputSize.x());
	const int height = std::clamp (static_cast<int> (std::lround (pictureSize.y() * result.scale)), 1, outputSize.y());
	result.destination = {(outputSize.x() - width) / 2, (outputSize.y() - height) / 2, width, height};
	return result;
}

//------------------------------------------------------------------------------
SDL_Rect computeViewArea (const SDL_Rect& windowsArea, const cPosition& bufferSize, const cPosition& minimalSize)
{
	const SDL_Rect bufferRect{0, 0, bufferSize.x(), bufferSize.y()};
	SDL_Rect rect;
	if (!SDL_IntersectRect (&windowsArea, &bufferRect, &rect))
	{
		return SDL_Rect{0, 0, 0, 0};
	}
	// small windows (dialogs) are shown in the size of a menu
	const int width = std::min (std::max (rect.w, minimalSize.x()), bufferRect.w);
	const int height = std::min (std::max (rect.h, minimalSize.y()), bufferRect.h);
	rect.x = std::clamp (rect.x + rect.w / 2 - width / 2, 0, bufferRect.w - width);
	rect.y = std::clamp (rect.y + rect.h / 2 - height / 2, 0, bufferRect.h - height);
	rect.w = width;
	rect.h = height;
	return rect;
}

//------------------------------------------------------------------------------
cPosition windowToPicturePosition (const cPosition& windowPosition, const SDL_Rect& source, const SDL_Rect& destination, const cPosition& windowSize, const cPosition& outputSize)
{
	if (destination.w <= 0 || destination.h <= 0 || windowSize.x() <= 0 || windowSize.y() <= 0)
	{
		return windowPosition;
	}
	// mouse positions are in window units, the picture is drawn in pixels: use the center of the pixel
	const double pixelX = (windowPosition.x() + 0.5) * outputSize.x() / windowSize.x();
	const double pixelY = (windowPosition.y() + 0.5) * outputSize.y() / windowSize.y();
	const double x = source.x + (pixelX - destination.x) * source.w / destination.w;
	const double y = source.y + (pixelY - destination.y) * source.h / destination.h;
	return cPosition (static_cast<int> (std::floor (x)), static_cast<int> (std::floor (y)));
}

//------------------------------------------------------------------------------
double computeUiScale (int uiScalePercent, const cPosition& outputSize, double displayScale)
{
	const auto pictureSize = [&] (double scale) {
		return cPosition (static_cast<int> (std::floor (outputSize.x() / scale)), static_cast<int> (std::floor (outputSize.y() / scale)));
	};
	double scale = 1.;
	if (uiScalePercent <= 0)
	{
		if (!(displayScale > 0.)) displayScale = 1.; // unknown (or not a number)
		scale = std::max (1., std::round (displayScale * autoUiScalePerDisplayScale / uiScaleStep) * uiScaleStep);
		while (scale > 1. && !isPictureAtLeast (pictureSize (scale), autoUiScaleMinimalWidth * autoUiScaleTolerance, autoUiScaleMinimalHeight * autoUiScaleTolerance))
		{
			scale -= uiScaleStep;
		}
	}
	else
	{
		scale = uiScalePercent / 100.;
	}
	while (scale > 1. && !isPictureAtLeast (pictureSize (scale), MINWIDTH, MINHEIGHT))
	{
		scale -= uiScaleStep;
	}
	return std::max (1., scale);
}

//------------------------------------------------------------------------------
cPosition computePictureSize (const cPosition& outputSize, double uiScale)
{
	const double scale = std::max (1., uiScale);
	const int width = static_cast<int> (std::floor (outputSize.x() / scale));
	const int height = static_cast<int> (std::floor (outputSize.y() / scale));
	return cPosition (std::max (MINWIDTH, width), std::max (MINHEIGHT, height));
}

//------------------------------------------------------------------------------
bool isUiScaleFitting (int uiScalePercent, const cPosition& outputSize)
{
	if (uiScalePercent <= 0) return true; // the automatic scale always fits
	const double scale = uiScalePercent / 100.;
	return computeUiScale (uiScalePercent, outputSize, 1.) == scale || scale <= 1.;
}

//------------------------------------------------------------------------------
void applySettings (cVideo& video, const sVideoSettings& videoSettings)
{
	// the size of the game picture follows the window and the scale of the user interface
	// (videoSettings.resolution is not used any more)
	video.setUiScale (videoSettings.uiScale);
	video.setColDepth (videoSettings.colourDepth);
	video.setWindowMode (videoSettings.windowMode);
	video.setDisplayIndex (videoSettings.displayIndex);
	video.setWindowGeometry (videoSettings.windowSize, videoSettings.windowMaximized);
}
