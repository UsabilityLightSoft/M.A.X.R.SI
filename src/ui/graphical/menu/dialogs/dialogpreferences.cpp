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

#include "dialogpreferences.h"

#include "output/sound/sounddevice.h"
#include "output/video/video.h"
#include "resources/pcx.h"
#include "settings.h"
#include "ui/graphical/menu/widgets/checkbox.h"
#include "ui/graphical/menu/widgets/colorselector.h"
#include "ui/graphical/menu/widgets/combobox.h"
#include "ui/graphical/menu/widgets/pushbutton.h"
#include "ui/graphical/menu/widgets/slider.h"
#include "ui/uidefines.h"
#include "ui/widgets/application.h"
#include "ui/widgets/label.h"
#include "ui/widgets/lineedit.h"
#include "utility/language.h"
#include "utility/string/iequals.h"

#include <algorithm>
#include <cmath>
#include <utility>

//------------------------------------------------------------------------------
cDialogPreferences::cDialogPreferences() :
	cWindow (LoadPCX (GFXOD_DIALOG5), eWindowBackgrounds::Alpha)
{
	// blit black titlebar behind textfield for playername
	SDL_Rect src = {108, 12, 186, 18};
	SDL_Rect dest = {140, 154, 0, 0};
	SDL_BlitSurface (getSurface(), &src, getSurface(), &dest);

	titleLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (0, 15), getPosition() + cPosition (getArea().getMaxCorner().x(), 25)), lngPack.i18n ("Settings~Preferences"), eUnicodeFontType::LatinNormal, eAlignmentType::CenterHorizontal);

	volumeLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 56), getPosition() + cPosition (135, 66)), lngPack.i18n ("Settings~Volume"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);

	musicLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 56 + 20), getPosition() + cPosition (135, 66 + 20)), lngPack.i18n ("Settings~Music"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);
	musicVolumeSlider = emplaceChild<cSlider> (cBox<cPosition> (getPosition() + cPosition (140, 53 + 20), getPosition() + cPosition (240, 70 + 20)), 0, 128, eOrientationType::Horizontal);
	signalConnectionManager.connect (musicVolumeSlider->valueChanged, [this]() { musicVolumeChanged(); });
	disableMusicCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (245, 73), lngPack.i18n ("Settings~Disable"));
	signalConnectionManager.connect (disableMusicCheckBox->toggled, [this]() { musicMuteChanged(); });

	effectsLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 56 + 20 * 2), getPosition() + cPosition (135, 66 + 20 * 2)), lngPack.i18n ("Settings~Effects"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);
	effectsVolumeSlider = emplaceChild<cSlider> (cBox<cPosition> (getPosition() + cPosition (140, 53 + 20 * 2), getPosition() + cPosition (240, 70 + 20 * 2)), 0, 128, eOrientationType::Horizontal);
	signalConnectionManager.connect (effectsVolumeSlider->valueChanged, [this]() { effectsVolumeChanged(); });
	disableEffectsCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (245, 73 + 20), lngPack.i18n ("Settings~Disable"));
	signalConnectionManager.connect (disableEffectsCheckBox->toggled, [this]() { effectsMuteChanged(); });
	effects3DCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (340, 73 + 20), "3D");

	voicesLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 56 + 20 * 3), getPosition() + cPosition (135, 66 + 20 * 3)), lngPack.i18n ("Settings~Voices"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);
	voicesVolumeSlider = emplaceChild<cSlider> (cBox<cPosition> (getPosition() + cPosition (140, 53 + 20 * 3), getPosition() + cPosition (240, 70 + 20 * 3)), 0, 128, eOrientationType::Horizontal);
	signalConnectionManager.connect (voicesVolumeSlider->valueChanged, [this]() { voicesVolumeChanged(); });
	disableVoicesCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (245, 73 + 20 * 2), lngPack.i18n ("Settings~Disable"));
	signalConnectionManager.connect (disableVoicesCheckBox->toggled, [this]() { voicesMuteChanged(); });

	playerNameLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 158), getPosition() + cPosition (138, 168)), lngPack.i18n ("Title~Player_Name"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);
	nameEdit = emplaceChild<cLineEdit> (cBox<cPosition> (getPosition() + cPosition (140, 154), getPosition() + cPosition (140 + 185, 154 + 18)), eLineEditFrameType::Box);

	colorLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 175), getPosition() + cPosition (135, 185)), lngPack.i18n ("Title~Color"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);
	colorSelector = emplaceChild<cColorSelector> (getPosition() + cPosition (160, 172), cRgbColor::black());

	scrollSpeedLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (25, 257), getPosition() + cPosition (135, 267)), lngPack.i18n ("Settings~ScrollSpeed"), eUnicodeFontType::LatinNormal, eAlignmentType::Left);
	scrollSpeedSlider = emplaceChild<cSlider> (cBox<cPosition> (getPosition() + cPosition (140, 254), getPosition() + cPosition (240, 271)), 0, 128, eOrientationType::Horizontal);

	animationCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (25, 193), lngPack.i18n ("Settings~Animation"));
	shadowsCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (25, 193 + 20), lngPack.i18n ("Settings~Shadows"));
	alphaCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (25, 193 + 20 * 2), lngPack.i18n ("Settings~AlphaEffects"));
	damageBuildingsCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (210, 193), lngPack.i18n ("Settings~ShowDamage"));
	damageVehiclesCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (210, 193 + 20), lngPack.i18n ("Settings~ShowDamageVehicle"));
	tracksCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (210, 193 + 20 * 2), lngPack.i18n ("Settings~Tracks"));

	autosaveCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (25, 294), lngPack.i18n ("Settings~Autosave"));
	introCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (25, 294 + 20), lngPack.i18n ("Settings~Intro"));
	windowCheckBox = emplaceChild<cCheckBox> (getPosition() + cPosition (25, 294 + 20 * 2), lngPack.i18n ("Settings~Window"));

	doneButton = emplaceChild<cPushButton> (getPosition() + cPosition (208, 383), ePushButtonType::Angular, lngPack.i18n ("Others~Done"), eUnicodeFontType::LatinNormal);
	doneButton->addClickShortcut (cKeySequence (cKeyCombination (SDLK_RETURN)));
	signalConnectionManager.connect (doneButton->clicked, [this]() { doneClicked(); });

	cancelButton = emplaceChild<cPushButton> (getPosition() + cPosition (118, 383), ePushButtonType::Angular, lngPack.i18n ("Others~Cancel"), eUnicodeFontType::LatinNormal);
	cancelButton->addClickShortcut (cKeySequence (cKeyCombination (SDLK_ESCAPE)));
	signalConnectionManager.connect (cancelButton->clicked, [this]() { cancelClicked(); });

	languageLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (140, 298 + 20 * 2), getPosition() + cPosition (140 + 95, 298 + 20 * 2 + 10)), lngPack.i18n ("Settings~Language") + lngPack.i18n ("Punctuation~Colon"), eUnicodeFontType::LatinNormal, eAlignmentType::Right);
	languagesComboBox = emplaceChild<cComboBox> (cBox<cPosition> (getPosition() + cPosition (240, 294 + 20 * 2), getPosition() + cPosition (240 + 100, 294 + 20 * 2 + 17)));
	signalConnectionManager.connect (languagesComboBox->onItemChanged, [this] (const std::string& selectedLanguage) {
		auto& settings = cSettings::getInstance();
		if (iequals (selectedLanguage, settings.getLanguage()))
		{
			return;
		}
		settings.setLanguage (selectedLanguage.c_str());
		lngPack.setCurrentLanguage (settings.getLanguage());

		auto application = getActiveApplication();
		if (application)
		{
			application->retranslate();
		}
	});

	// why a scale of the user interface is not used (below the lists: created before them, the open lists are above it)
	uiScaleHintLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (150, 358), getPosition() + cPosition (340, 368)), "", eUnicodeFontType::LatinNormal, eAlignmentType::Right);

	// the size of the mouse cursor: applied at once
	// (created before the scale: the open list of scales is drawn over it)
	cursorSizeLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (140, 298 + 20), getPosition() + cPosition (140 + 95, 298 + 20 + 10)), lngPack.i18n ("Settings~CursorSize") + lngPack.i18n ("Punctuation~Colon"), eUnicodeFontType::LatinNormal, eAlignmentType::Right);
	cursorSizeComboBox = emplaceChild<cComboBox> (cBox<cPosition> (getPosition() + cPosition (240, 294 + 20), getPosition() + cPosition (240 + 100, 294 + 20 + 17)));
	cursorSizeComboBox->setMaxVisibleItems (cursorSizePercents.size() + 1);
	signalConnectionManager.connect (cursorSizeComboBox->onItemChanged, [this] (const std::string&) { cursorSizeChanged(); });

	// the scale of the user interface (instead of the screen resolution): applied at once
	uiScaleLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (140, 298), getPosition() + cPosition (140 + 95, 298 + 10)), lngPack.i18n ("Settings~UiScale") + lngPack.i18n ("Punctuation~Colon"), eUnicodeFontType::LatinNormal, eAlignmentType::Right);
	uiScaleComboBox = emplaceChild<cComboBox> (cBox<cPosition> (getPosition() + cPosition (240, 294), getPosition() + cPosition (240 + 100, 294 + 17)));
	uiScaleComboBox->setMaxVisibleItems (uiScalePercents.size() + 1);
	signalConnectionManager.connect (uiScaleComboBox->onItemChanged, [this] (const std::string&) { uiScaleChanged(); });
	signalConnectionManager.connect (Video.resolutionChanged, [this]() {
		updateUiScaleItems();
		// the automatic cursor size follows the scale of the user interface
		updateCursorSizeItems();
	});

	loadValues();
}

//------------------------------------------------------------------------------
cDialogPreferences::~cDialogPreferences()
{}

//------------------------------------------------------------------------------
void cDialogPreferences::retranslate()
{
	titleLabel->setText (lngPack.i18n ("Settings~Preferences"));
	volumeLabel->setText (lngPack.i18n ("Settings~Volume"));
	musicLabel->setText (lngPack.i18n ("Settings~Music"));
	effectsLabel->setText (lngPack.i18n ("Settings~Effects"));
	voicesLabel->setText (lngPack.i18n ("Settings~Voices"));
	playerNameLabel->setText (lngPack.i18n ("Title~Player_Name"));
	colorLabel->setText (lngPack.i18n ("Title~Color"));
	scrollSpeedLabel->setText (lngPack.i18n ("Settings~ScrollSpeed"));
	languageLabel->setText (lngPack.i18n ("Settings~Language") + lngPack.i18n ("Punctuation~Colon"));
	uiScaleLabel->setText (lngPack.i18n ("Settings~UiScale") + lngPack.i18n ("Punctuation~Colon"));
	updateUiScaleItems();
	cursorSizeLabel->setText (lngPack.i18n ("Settings~CursorSize") + lngPack.i18n ("Punctuation~Colon"));
	updateCursorSizeItems();

	doneButton->setText (lngPack.i18n ("Others~Done"));
	cancelButton->setText (lngPack.i18n ("Others~Cancel"));

	disableMusicCheckBox->setText (lngPack.i18n ("Settings~Disable"));
	disableEffectsCheckBox->setText (lngPack.i18n ("Settings~Disable"));
	effects3DCheckBox->setText (lngPack.i18n ("Settings~3D"));
	disableVoicesCheckBox->setText (lngPack.i18n ("Settings~Disable"));
	animationCheckBox->setText (lngPack.i18n ("Settings~Animation"));
	shadowsCheckBox->setText (lngPack.i18n ("Settings~Shadows"));
	alphaCheckBox->setText (lngPack.i18n ("Settings~AlphaEffects"));
	damageBuildingsCheckBox->setText (lngPack.i18n ("Settings~ShowDamage"));
	damageVehiclesCheckBox->setText (lngPack.i18n ("Settings~ShowDamageVehicle"));
	tracksCheckBox->setText (lngPack.i18n ("Settings~Tracks"));
	autosaveCheckBox->setText (lngPack.i18n ("Settings~Autosave"));
	introCheckBox->setText (lngPack.i18n ("Settings~Intro"));
	windowCheckBox->setText (lngPack.i18n ("Settings~Window"));
}

//------------------------------------------------------------------------------
void cDialogPreferences::loadValues()
{
	const auto& settings = cSettings::getInstance();
	musicVolumeSlider->setValue (settings.getMusicVol());
	effectsVolumeSlider->setValue (settings.getSoundVol());
	voicesVolumeSlider->setValue (settings.getVoiceVol());

	disableMusicCheckBox->setChecked (settings.isMusicMute());
	disableEffectsCheckBox->setChecked (settings.isSoundMute());
	disableVoicesCheckBox->setChecked (settings.isVoiceMute());

	effects3DCheckBox->setChecked (settings.is3DSound());

	scrollSpeedSlider->setValue (settings.getScrollSpeed());

	nameEdit->setText (settings.getPlayerSettings().name);
	colorSelector->setColor (settings.getPlayerSettings().color);

	animationCheckBox->setChecked (settings.isAnimations());
	shadowsCheckBox->setChecked (settings.isShadows());
	alphaCheckBox->setChecked (settings.isAlphaEffects());
	damageBuildingsCheckBox->setChecked (settings.isDamageEffects());
	damageVehiclesCheckBox->setChecked (settings.isDamageEffectsVehicles());
	tracksCheckBox->setChecked (settings.isMakeTracks());

	autosaveCheckBox->setChecked (settings.shouldAutosave());
	introCheckBox->setChecked (settings.shouldShowIntro());
	windowCheckBox->setChecked (Video.getWindowMode());

	languagesComboBox->clearItems();
	const auto availableLanguages = lngPack.getAvailableLanguages();
	size_t selectedLanguageIndex = 0;
	for (size_t i = 0; i < availableLanguages.size(); ++i)
	{
		languagesComboBox->addItem (availableLanguages[i]);
		if (iequals (availableLanguages[i], settings.getLanguage()))
		{
			selectedLanguageIndex = i;
		}
	}
	if (!availableLanguages.empty())
	{
		languagesComboBox->setSelectedIndex (selectedLanguageIndex);
	}

	updateUiScaleItems();
	updateCursorSizeItems();

	storePreviewValues();
}

//------------------------------------------------------------------------------
void cDialogPreferences::saveValues()
{
	cSettings& settings = cSettings::getInstance();

	settings.setPlayerSettings ({nameEdit->getText(), colorSelector->getColor()});

	settings.set3DSound (effects3DCheckBox->isChecked());

	settings.setAnimations (animationCheckBox->isChecked());
	settings.setShadows (shadowsCheckBox->isChecked());
	settings.setAlphaEffects (alphaCheckBox->isChecked());
	settings.setDamageEffects (damageBuildingsCheckBox->isChecked());
	settings.setDamageEffectsVehicles (damageVehiclesCheckBox->isChecked());
	settings.setMakeTracks (tracksCheckBox->isChecked());

	settings.setAutosave (autosaveCheckBox->isChecked());
	settings.setShowIntro (introCheckBox->isChecked());

	if (settings.getVideoSettings().windowMode != windowCheckBox->isChecked())
	{
		// window or fullscreen without changing the video mode: no restart needed
		settings.getVideoSettings().windowMode = windowCheckBox->isChecked();
		Video.setWindowMode (windowCheckBox->isChecked(), true);
	}

	settings.setScrollSpeed (scrollSpeedSlider->getValue());

	// the scale and the cursor size have been applied when they were chosen
	settings.getVideoSettings().uiScale = Video.getUiScale();
	settings.getVideoSettings().cursorSize = Video.getCursorSize();

	applySettings (Video, settings.getVideoSettings());
	settings.saveInFile();
}

//------------------------------------------------------------------------------
void cDialogPreferences::storePreviewValues()
{
	storedLanguage = cSettings::getInstance().getLanguage();

	storedMusicVolume = cSettings::getInstance().getMusicVol();
	storedEffectsVolume = cSettings::getInstance().getSoundVol();
	storedVoicesVolume = cSettings::getInstance().getVoiceVol();

	storedMusicMute = cSettings::getInstance().isMusicMute();
	storedEffectsMute = cSettings::getInstance().isSoundMute();
	storedVoicesMute = cSettings::getInstance().isVoiceMute();

	storedUiScale = cSettings::getInstance().getVideoSettings().uiScale;
	storedCursorSize = cSettings::getInstance().getVideoSettings().cursorSize;
}

//------------------------------------------------------------------------------
void cDialogPreferences::restorePreviewValues()
{
	if (storedLanguage != cSettings::getInstance().getLanguage())
	{
		cSettings::getInstance().setLanguage (storedLanguage.c_str());
		lngPack.setCurrentLanguage (storedLanguage);
		auto application = getActiveApplication();
		if (application)
		{
			application->retranslate();
		}
	}

	cSettings::getInstance().setMusicVol (storedMusicVolume);
	cSettings::getInstance().setSoundVol (storedEffectsVolume);
	cSettings::getInstance().setVoiceVol (storedVoicesVolume);
	if (cSettings::getInstance().isSoundEnabled())
	{
		cSoundDevice::getInstance().setMusicVolume (cSettings::getInstance().getMusicVol());
		cSoundDevice::getInstance().setSoundEffectVolume (cSettings::getInstance().getSoundVol());
		cSoundDevice::getInstance().setVoiceVolume (cSettings::getInstance().getVoiceVol());
	}

	bool wasMusicMute = cSettings::getInstance().isMusicMute();
	cSettings::getInstance().setMusicMute (storedMusicMute);
	if (wasMusicMute && !storedMusicMute) cSoundDevice::getInstance().startRandomMusic();
	cSettings::getInstance().setSoundMute (storedEffectsMute);
	cSettings::getInstance().setVoiceMute (storedVoicesMute);
	cSettings::getInstance().saveInFile();

	if (Video.getUiScale() != storedUiScale) Video.setUiScale (storedUiScale);
	if (Video.getCursorSize() != storedCursorSize) Video.setCursorSize (storedCursorSize);
}

//------------------------------------------------------------------------------
void cDialogPreferences::doneClicked()
{
	saveValues();
	close();
}

//------------------------------------------------------------------------------
void cDialogPreferences::cancelClicked()
{
	restorePreviewValues();
	close();
}

//------------------------------------------------------------------------------
void cDialogPreferences::musicVolumeChanged()
{
	cSettings::getInstance().setMusicVol (musicVolumeSlider->getValue());
	if (cSettings::getInstance().isSoundEnabled()) cSoundDevice::getInstance().setMusicVolume (cSettings::getInstance().getMusicVol());
	cSettings::getInstance().saveInFile();
}

//------------------------------------------------------------------------------
void cDialogPreferences::effectsVolumeChanged()
{
	cSettings::getInstance().setSoundVol (effectsVolumeSlider->getValue());
	if (cSettings::getInstance().isSoundEnabled()) cSoundDevice::getInstance().setSoundEffectVolume (cSettings::getInstance().getSoundVol());
	cSettings::getInstance().saveInFile();
}

//------------------------------------------------------------------------------
void cDialogPreferences::voicesVolumeChanged()
{
	cSettings::getInstance().setVoiceVol (voicesVolumeSlider->getValue());
	if (cSettings::getInstance().isSoundEnabled()) cSoundDevice::getInstance().setVoiceVolume (cSettings::getInstance().getVoiceVol());
	cSettings::getInstance().saveInFile();
}

//------------------------------------------------------------------------------
void cDialogPreferences::musicMuteChanged()
{
	bool wasMute = cSettings::getInstance().isMusicMute();
	cSettings::getInstance().setMusicMute (disableMusicCheckBox->isChecked());
	if (cSettings::getInstance().isMusicMute()) cSoundDevice::getInstance().stopMusic();
	if (!cSettings::getInstance().isMusicMute() && wasMute) cSoundDevice::getInstance().startRandomMusic();
	cSettings::getInstance().saveInFile();
}

//------------------------------------------------------------------------------
void cDialogPreferences::effectsMuteChanged()
{
	cSettings::getInstance().setSoundMute (disableEffectsCheckBox->isChecked());
	cSettings::getInstance().saveInFile();
}

//------------------------------------------------------------------------------
void cDialogPreferences::voicesMuteChanged()
{
	cSettings::getInstance().setVoiceMute (disableVoicesCheckBox->isChecked());
	cSettings::getInstance().saveInFile();
}

namespace
{
	//--------------------------------------------------------------------------
	std::string toPercentText (double scale)
	{
		return std::to_string (std::lround (scale * 100)) + "%";
	}
} // namespace

//------------------------------------------------------------------------------
void cDialogPreferences::updateUiScaleItems()
{
	updatingUiScaleItems = true;

	const auto outputSize = Video.getOutputSize();
	const int current = Video.getUiScale();

	uiScaleComboBox->clearItems();
	uiScaleItemPercents.clear();
	const auto addItem = [&] (int percent, const std::string& text) {
		uiScaleComboBox->addItem (text);
		// values that would make the picture smaller than the menus are reduced: shown dimmed
		uiScaleComboBox->setItemDimmed (uiScaleItemPercents.size(), !isUiScaleFitting (percent, outputSize));
		uiScaleItemPercents.push_back (percent);
	};
	addItem (0, lngPack.i18n ("Settings~UiScale_Auto", toPercentText (Video.getAutoUiScaleFactor())));
	for (const int percent : uiScalePercents)
	{
		addItem (percent, std::to_string (percent) + "%");
	}
	if (std::ranges::find (uiScaleItemPercents, current) == uiScaleItemPercents.end())
	{
		// a value from the settings file, that is not in the list
		addItem (current, std::to_string (current) + "%");
	}
	selectUiScaleItem (current);
	uiScaleHintLabel->setText (isUiScaleFitting (current, outputSize) ? "" : lngPack.i18n ("Settings~UiScale_WindowTooSmall", std::to_string (current) + "%"));

	updatingUiScaleItems = false;
}

//------------------------------------------------------------------------------
void cDialogPreferences::selectUiScaleItem (int percent)
{
	const auto it = std::ranges::find (uiScaleItemPercents, percent);
	if (it == uiScaleItemPercents.end()) return;

	const bool wasUpdating = std::exchange (updatingUiScaleItems, true);
	uiScaleComboBox->setSelectedIndex (it - uiScaleItemPercents.begin());
	updatingUiScaleItems = wasUpdating;
}

//------------------------------------------------------------------------------
void cDialogPreferences::uiScaleChanged()
{
	if (updatingUiScaleItems) return;

	const auto index = uiScaleComboBox->getSelectedIndex();
	if (!index || *index >= uiScaleItemPercents.size()) return;

	const int percent = uiScaleItemPercents[*index];
	if (uiScaleComboBox->isItemDimmed (*index))
	{
		// the window is too small for this scale: keep the current one and tell why
		selectUiScaleItem (Video.getUiScale());
		uiScaleHintLabel->setText (lngPack.i18n ("Settings~UiScale_WindowTooSmall", std::to_string (percent) + "%"));
		return;
	}
	uiScaleHintLabel->setText ("");
	// applied with the next frame: the game (if any) is shown in the new scale at once
	Video.setUiScale (percent);
}

//------------------------------------------------------------------------------
void cDialogPreferences::updateCursorSizeItems()
{
	const bool wasUpdating = std::exchange (updatingCursorSizeItems, true);

	const int current = Video.getCursorSize();

	cursorSizeComboBox->clearItems();
	cursorSizeItemPercents.clear();
	const auto addItem = [&] (int percent, const std::string& text) {
		cursorSizeComboBox->addItem (text);
		cursorSizeItemPercents.push_back (percent);
	};
	addItem (0, lngPack.i18n ("Settings~CursorSize_Auto", std::to_string (Video.getAutoCursorSize()) + "%"));
	for (const int percent : cursorSizePercents)
	{
		addItem (percent, std::to_string (percent) + "%");
	}
	if (std::ranges::find (cursorSizeItemPercents, current) == cursorSizeItemPercents.end())
	{
		// a value from the settings file, that is not in the list
		addItem (current, std::to_string (current) + "%");
	}
	const auto it = std::ranges::find (cursorSizeItemPercents, current);
	cursorSizeComboBox->setSelectedIndex (it - cursorSizeItemPercents.begin());

	updatingCursorSizeItems = wasUpdating;
}

//------------------------------------------------------------------------------
void cDialogPreferences::cursorSizeChanged()
{
	if (updatingCursorSizeItems) return;

	const auto index = cursorSizeComboBox->getSelectedIndex();
	if (!index || *index >= cursorSizeItemPercents.size()) return;

	// the cursor is created again at once: its new size is seen over this dialog
	Video.setCursorSize (cursorSizeItemPercents[*index]);
}
