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

#ifndef output_video_videoH
#define output_video_videoH

#include "SDLutility/sdlcomponent.h"
#include "SDLutility/uniquesurface.h"
#include "settings.h"
#include "utility/box.h"
#include "utility/position.h"
#include "utility/signal/signal.h"
#include "utility/signal/signalconnectionmanager.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

class cKeyboard;
struct sVideoSettings;

/**
 * For which kind of screen the mouse cursor is scaled (see computeCursorScale).
 */
enum class eCursorScaleContext
{
	/** menus, lobbies, settings: the cursor in the size of the mouse pointer of the system */
	Menu,
	/** the map of the game (and the windows above it): the cursor not bigger than the pictures of the game */
	Map
};

/**
 * cVideo class.
 * Stores videosettings (and can hopefully operate 'em too one day'):-)
 *
 * @author Bernd "beko" Kosmahl
 */
class cVideo
{
public:
	cVideo();
	~cVideo();

	void clearMemory();

	void init (std::shared_ptr<SDLComponent>, const std::string& title, const std::filesystem::path& icon);

	/**
	 * Shows the splash screen
	 */
	void showSplashScreen (const std::filesystem::path&);

	/**
	 * Prepares the window, buffer and renderer to display the game screen.
	 * The window gets a frame, can be resized and maximized;
	 * the screen buffer gets the size of the window divided by the scale of the user interface
	 * and is scaled to the window.
	 */
	void prepareGameScreen();

	/**
	 * Sets the scale of the user interface in percent
	 * (100: a pixel of the game picture is a pixel of the window), 0: automatic (see @ref computeUiScale).
	 * The game picture (screen buffer) gets the size of the window divided by the scale
	 * with the next @ref updatePictureSize.
	 */
	void setUiScale (int percent);
	int getUiScale() const { return uiScale; }

	/**
	 * The scale of the user interface in use: pixels of the window per pixel of the game picture.
	 */
	double getUiScaleFactor() const { return uiScaleFactor; }

	/**
	 * The scale of the user interface that the automatic setting gives for the current window and display.
	 */
	double getAutoUiScaleFactor() const;

	/**
	 * Size of the window (client area) in pixels.
	 */
	cPosition getOutputSize() const;

	/**
	 * Recreates the game picture (screen buffer), if its size has to change:
	 * the window has been resized, maximized, switched to full screen or moved to another display,
	 * or the scale of the user interface has been changed.
	 * Not more often than every 150 ms, so that resizing the window with the mouse does not stutter;
	 * the last change is applied, when the resizing has ended.
	 * Emits @ref resolutionChanged when the size has changed.
	 * Is called once per frame by the application.
	 */
	void updatePictureSize();

	/**
	* Sets whether app should appear windowed or in fullscreen mode.
	* Fullscreen covers the desktop without changing the video mode of the display.
	*
	* @param bWindowMode pass true if app should work in windowed mode
	*                    pass false it app should start in fullscreen
	* @param bApply set to true if app should apply new windowMode too
	*/
	void setWindowMode (bool bWindowMode, bool bApply = false);

	/**
	 * Sets the size of the window (client area in pixels) and whether it is maximized,
	 * used by @ref prepareGameScreen. Without a size a default size is chosen.
	 */
	void setWindowGeometry (std::optional<cPosition> size, bool maximized);

	/**
	 * Writes the current window size and maximized state to the video settings.
	 */
	void storeWindowGeometry (sVideoSettings&) const;

	/**
	 * Sets the part of the screen buffer that is displayed.
	 * It is scaled to fill the window as much as possible while keeping the aspect ratio.
	 * Areas smaller than the menu size (640x480) are extended to the menu size.
	 * A box outside of the buffer (or the whole buffer) displays the whole buffer.
	 */
	void setViewArea (const cBox<cPosition>&);

	/**
	 * Converts a position in the window (as reported by SDL mouse events)
	 * to the position in the screen buffer that is displayed there.
	 * Positions outside of the displayed picture give positions outside of the view area.
	 */
	cPosition windowToBuffer (const cPosition&) const;

	/**
	 * Factor for the mouse cursor on the screen (pixels of the window per pixel of the cursor image),
	 * following the scale of the user interface, the size of the mouse pointer set in the system,
	 * the screen (menus or the map of the game) and the setting of the cursor size (see @ref computeCursorScale).
	 * @ref cursorScaleChanged is emitted, when it changes.
	 */
	double getCursorScale() const { return cursorScale; }

	/**
	 * Sets the size of the mouse cursor in percent (see @ref computeCursorScale), 0: automatic.
	 * Applied at once.
	 */
	void setCursorSize (int percent);
	int getCursorSize() const { return cursorSize; }

	/**
	 * The size of the cursor in the game in percent of the game picture, that the automatic setting gives
	 * for the current scale of the user interface and pointer size of the system (see @ref computeAutoCursorSize).
	 */
	int getAutoCursorSize() const;

	/**
	 * Sets, whether the active screen shows the map of the game (the cursor in the scale of the map)
	 * or is a menu. Applied at once.
	 */
	void setCursorScaleContext (eCursorScaleContext);
	eCursorScaleContext getCursorScaleContext() const { return cursorScaleContext; }

	/**
	 * The size of the mouse pointer set in the system (1 = normal), read the last time the cursor scale has been computed.
	 */
	double getSystemPointerScale() const { return systemPointerScale; }

	/**
	 * Increased each time the mapping from window positions to buffer positions changes.
	 */
	unsigned int getPresentationVersion() const { return presentationVersion; }

	/**
	 * Handles window events (size changes, maximization, ...)
	 */
	void handleWindowEvent (const SDL_WindowEvent&);

	/**
	* Get whether app should appear windowed or in fullscreen mode
	* @return  true if app should work in windowed mode
	*          false it app should start in fullscreen
	*/
	bool getWindowMode() const { return windowMode; }

	/**
	* @return width of the game picture (screen buffer)
	*/
	int getResolutionX() const { return resolutionX; }

	/**
	* @return height of the game picture (screen buffer)
	*/
	int getResolutionY() const { return resolutionY; }

	/**
	* Sets colordepth
	* @param iDepth colordepth to set. e.g. 32 (bpp)
	*/
	void setColDepth (unsigned int iDepth);

	/**
	* Gets colordepth
	* @return colordepth
	*/
	int getColDepth() const { return colorDepth; }

	/**
	 * Sets an new display (monitor) as the currently active one.
	 * @param index The index of the display to be used.
	 */
	void setDisplayIndex (int index);

	/**
	 * Returns the index of the display that is currently used.
	 * @return The display index.
	 */
	int getDisplayIndex() const { return displayIndex; }

	/**
	* @return Minimal needed screen resolution width
	*/
	int getMinW() const;

	/**
	* @return Minimal needed screen resolution height
	*/
	int getMinH() const;

	/**
	* clears buffer (black)
	*/
	void clearBuffer();

	/**
	* Blits or updates buffer to screen depending on windowmode
	*/
	void draw();

	void takeScreenShot (const std::filesystem::path&) const;

	void applyShadow (const SDL_Rect* rect, SDL_Surface& destination);

	mutable cSignal<void (const std::filesystem::path&)> screenShotTaken;

	mutable cSignal<void()> resolutionChanged;

	mutable cSignal<void()> cursorScaleChanged;

	// Screenbuffers ///////////////////////////////////
	static SDL_Surface* buffer; // Der Bildschirm-Buffer

private:
	void keyPressed (cKeyboard& keyboard, SDL_Keycode key);

	void applyWindowMode();

	/** Recreates the screen buffer in the size for the current window and scale; returns whether the size has changed. */
	bool applyPictureSize (bool force);
	/** Computes the scale of the cursor again (and reads the pointer size of the system); emits cursorScaleChanged, if it has changed. */
	void updateCursorScale();
	/** Scale of the display set in the system (DPI / 96) or 1, if unknown. */
	double getDisplayScale() const;

	void initializeBuffer (int width, int height);

	/** Size of the window (client area) when it is neither maximized nor fullscreen and no size has been stored. */
	cPosition getDefaultWindowSize() const;
	/** Size of the client area of a window that fills the usable area of the display. */
	cPosition getMaximalWindowSize() const;

	/** Computes how the view area is scaled to the current window size. */
	void updatePresentation();
	/** Gets the texture for the intermediate integer upscaling of the view area. */
	SDL_Texture* getScaledTexture (int width, int height);

private:
	std::shared_ptr<SDLComponent> sdlComponent; // to ensure order of construction/destruction
	SDL_Window* sdlWindow = nullptr;
	SDL_Renderer* sdlRenderer = nullptr;
	SDL_Texture* sdlTexture = nullptr;
	SDL_Texture* scaledTexture = nullptr;

	int resolutionX;
	int resolutionY;

	int displayIndex;

	int colorDepth;

	bool windowMode = false;

	/** Size of the client area of the window when it is not maximized (and not fullscreen). */
	std::optional<cPosition> windowedSize;
	/** Scale of the user interface in percent, 0: automatic. */
	int uiScale = 0;
	/** The scale of the user interface in use (see computeUiScale). */
	double uiScaleFactor = 1.;
	/** The size of the screen buffer has to be checked (window resized, scale changed, ...). */
	bool pictureSizeOutdated = false;
	/** When the size of the screen buffer has been checked the last time (SDL ticks). */
	Uint32 lastPictureSizeUpdate = 0;
	bool windowMaximized = true;
	/** The window shows the game (and no longer the splash screen). */
	bool gameScreenPrepared = false;

	/** The part of the buffer which is displayed. The whole buffer, if not set. */
	std::optional<SDL_Rect> viewArea;
	/** False, if the renderer can not do the intermediate upscaling (no render targets). */
	bool sharpScalingAvailable = true;
	/** Maximal texture size of the renderer, 0 if unknown. */
	cPosition maximalTextureSize;

	/**
	 * How the view area is displayed in the window:
	 * The source rectangle of the buffer is drawn to the destination rectangle of the window.
	 * Integer scale factors draw the pixels as they are (nearest neighbour).
	 * Other scale factors first enlarge the picture by an integer factor (nearest neighbour)
	 * and then scale it smoothly to the destination size ("sharp bilinear"),
	 * so that the pixel graphic stays sharp without uneven pixel sizes.
	 */
	struct sPresentation
	{
		SDL_Rect source{0, 0, 0, 0};
		SDL_Rect destination{0, 0, 0, 0};
		cPosition windowSize; // in the units of mouse events
		cPosition outputSize; // in pixels
		int prescale = 1; // integer factor of the intermediate texture, 1 = no intermediate texture
		bool smooth = false;
	};
	sPresentation presentation;
	unsigned int presentationVersion = 0;
	double cursorScale = 1.;
	/** Size of the cursor in percent, 0: automatic. */
	int cursorSize = 0;
	eCursorScaleContext cursorScaleContext = eCursorScaleContext::Menu;
	double systemPointerScale = 1.;

	cSignalConnectionManager signalConnectionManager;
};

extern cVideo Video;

void applySettings (cVideo&, const sVideoSettings&);

/**
 * How a picture (a part of the screen buffer) is shown in a window:
 * as big as possible, keeping the aspect ratio, centered.
 * An integer scale factor (pixels as they are) is used,
 * if it covers at least 90% of the size of the fitting scale factor.
 * Otherwise the picture is scaled smoothly: first enlarged by an integer factor
 * without filtering ("prescale"), then smoothly to the window ("sharp bilinear").
 */
struct sViewScaling
{
	SDL_Rect destination{0, 0, 0, 0}; // in pixels of the window
	double scale = 1.;
	int prescale = 1; // 1 = no intermediate upscaling
	bool smooth = false;
};

/**
 * @param pictureSize size of the shown part of the screen buffer
 * @param outputSize size of the window in pixels
 * @param prescaleAvailable whether the renderer can do the intermediate upscaling (render targets)
 * @param maximalTextureSize maximal texture size of the renderer, 0 if unknown
 */
sViewScaling computeViewScaling (const cPosition& pictureSize, const cPosition& outputSize, bool prescaleAvailable, const cPosition& maximalTextureSize);

/**
 * The part of the screen buffer to show for the area of the visible windows:
 * inside of the buffer and at least of the minimal size (smaller windows are shown in the size of a menu).
 * An empty rectangle, if the area is outside of the buffer.
 */
SDL_Rect computeViewArea (const SDL_Rect& windowsArea, const cPosition& bufferSize, const cPosition& minimalSize);

/**
 * The position in the screen buffer, that is shown at a position of the window.
 * @param source shown part of the buffer
 * @param destination where it is drawn in the window (pixels)
 * @param windowSize size of the window in the units of the mouse positions
 * @param outputSize size of the window in pixels
 */
cPosition windowToPicturePosition (const cPosition& windowPosition, const SDL_Rect& source, const SDL_Rect& destination, const cPosition& windowSize, const cPosition& outputSize);

/**
 * The scale factors of the user interface offered in the settings, in percent.
 * 0 (not in the list) is the automatic scale.
 */
inline constexpr std::array<int, 6> uiScalePercents{100, 150, 200, 250, 300, 400};

/**
 * The scale of the user interface: how many pixels of the window show one pixel of the game picture.
 *
 * Automatic (uiScalePercent 0): 2.4 times the scale of the display set in the system (DPI / 96),
 * rounded to 0.5 (a 4K display at 125% gets 3, a full HD display at 100% 2.5);
 * reduced in steps of 0.5, while the picture would be smaller than 1280x720
 * (up to a tenth less is accepted: the title bar and the task bar take some height of a maximized window).
 * Every scale is reduced in steps of 0.5, while the picture would be smaller than 640x480 (the size of the menus).
 * The scale is never smaller than 1.
 *
 * @param uiScalePercent the chosen scale in percent, 0 = automatic
 * @param outputSize size of the window in pixels
 * @param displayScale scale of the display (DPI / 96), 1 if unknown
 */
double computeUiScale (int uiScalePercent, const cPosition& outputSize, double displayScale);

/**
 * The size of the game picture (the screen buffer) for a window:
 * the window size divided by the scale of the user interface, but not smaller than 640x480.
 */
cPosition computePictureSize (const cPosition& outputSize, double uiScale);

/**
 * Whether a scale (in percent, 0 = automatic) keeps the game picture of the window at least 640x480,
 * so that it is used without being reduced.
 */
bool isUiScaleFitting (int uiScalePercent, const cPosition& outputSize);

/**
 * The sizes of the mouse cursor offered in the settings, in percent of the pictures of the game (see computeCursorScale).
 * 0 (not in the list) is the automatic size.
 */
inline constexpr std::array<int, 7> cursorSizePercents{50, 75, 100, 125, 150, 175, 200};

/**
 * The scale of the mouse cursor on the screen: pixels of the window per pixel of the cursor image.
 *
 * Automatic (cursorSizePercent 0):
 * - menus: two thirds of the scale of the user interface, but at least 1 (the automatic scale 3 of a 4K display
 *   at 125% gives 2: the hand is 36 pixels, about 1.5 standard arrows of the system),
 *   times the size of the mouse pointer set in the system, that enlarges the arrow of the system as well;
 * - the map of the game: the same, but not bigger than the pictures of the game (the scale of the user interface):
 *   with an enlarged pointer of the system the cursor would cover several tiles.
 *   The circle of the cursor (26 pixels) is about a tile of a far zoom (24 pixels).
 *
 * Chosen size (cursorSizePercent > 0): on the map this part of the pictures of the game
 * (100: a pixel of the cursor image is a pixel of the game picture), in the menus as many times bigger
 * than on the map as with the automatic size.
 *
 * @param uiScale scale of the user interface (pixels of the window per pixel of the game picture)
 * @param systemPointerScale size of the mouse pointer set in the system, 1 = normal
 */
double computeCursorScale (double uiScale, double systemPointerScale, eCursorScaleContext, int cursorSizePercent);

/**
 * The size of the cursor on the map in percent of the pictures of the game, that the automatic setting gives.
 */
int computeAutoCursorSize (double uiScale, double systemPointerScale);

/**
* Works like SDL_BlittSurface.
* But unlike SDL it respects the destination alpha channel of the surface.
* This function is only designed to blitt from a surface
* with per surface alpha value to a surface with alpha channel.
* A source color key is also supported.
*/
void blittPerSurfaceAlphaToAlphaChannel (SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect);
/**
* Works like SDL_BlittSurface.
* This function choses the right blitt function to use for blitting.
*/
void blittAlphaSurface (SDL_Surface* src, SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect);

/**
 * scale a surface to the passed size.
 * The scaled surface will be drawn to the destination surface.
 * If the destination surface is nullptr a new surface will be created.
 * @author alzi alias DoctorDeath
 * @param scr the surface to be scaled.
 * @param dest the surface that will receive the new pixel data.
 *        If it is NUll a new surface will be created.
 * @param width width of the new surface.
 * @param height height of the new surface.
 * @return returns the destination surface.
 */
SDL_Surface* scaleSurface (SDL_Surface* scr, SDL_Surface* dest, int width, int height);

/** this function checks, whether the surface has to be rescaled,
 * and scales it if necessary */
inline void CHECK_SCALING (SDL_Surface& surface, SDL_Surface& surface_org, float factor)
{
	if (!cSettings::getInstance().shouldDoPrescale() && (surface.w != (int) (surface_org.w * factor) || surface.h != (int) (surface_org.h * (factor))))
		scaleSurface (&surface_org, &surface, (int) (surface_org.w * factor), (int) (surface_org.h * factor));
}

UniqueSurface CreatePfeil (int p1x, int p1y, int p2x, int p2y, int p3x, int p3y, unsigned int color, int size);

/** Draws a circle on the surface */
void drawCircle (int iX, int iY, int iRadius, int iColor, SDL_Surface& surface);

#endif
