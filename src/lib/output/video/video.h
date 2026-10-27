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

#include <filesystem>
#include <memory>
#include <optional>

class cKeyboard;
struct sVideoSettings;

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
	 * the screen buffer is scaled to the window size.
	 */
	void prepareGameScreen();

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
	 * Integer factor for the mouse cursor, matching the current scale of the picture.
	 */
	int getCursorScale() const { return cursorScale; }

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
	* Set resolution/dimension of app window.
	* @param iWidth desired window width
	* @param iHeight desired window height
	* @param bApply  set to true if app should apply new resolution too
	*/
	void setResolution (int iWidth, int iHeight, bool bApply = false);

	/**
	* @deprecated for compat only - will be removed!
	* @return stored window width
	*/
	int getResolutionX() const { return resolutionX; }

	/**
	* @deprecated for compat only - will be removed!
	* @return stored window height
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
	 * Returns the available resolutions for the currently active display.
	 *
	 * @return A list with pairs of the supported X and Y resolution components.
	 */
	const std::vector<std::pair<int, int>>& getDetectedResolutions() const;

	/**
	* Check whether the provided mode is known to our video mode list
	* @param width Screenwidth to look for
	* @param height Screenheight to look for
	* @return iMode or -1 on unknown mode
	*/
	int validateResolution (int width, int height) const;

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
	/**
	* Checks whether our minimal needed videomode has been autodetected
	* @return true if mininal video mode looks valid
	*/
	bool haveMinMode() const;

	void keyPressed (cKeyboard& keyboard, SDL_Keycode key);

	void applyResolution();
	void applyWindowMode();

	void initializeBuffer (int width, int height);

	void detectResolutions();

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
	int cursorScale = 1;

	cSignalConnectionManager signalConnectionManager;

	std::vector<std::vector<std::pair<int, int>>> detectedResolutions;
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
