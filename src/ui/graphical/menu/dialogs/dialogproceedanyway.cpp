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

#include "dialogproceedanyway.h"

#include "resources/pcx.h"
#include "ui/graphical/menu/widgets/pushbutton.h"
#include "ui/uidefines.h"
#include "ui/widgets/label.h"
#include "utility/language.h"

//------------------------------------------------------------------------------
cDialogProceedAnyway::cDialogProceedAnyway (const std::string& text, const std::string& proceedText) :
	cWindow (LoadPCX (GFXOD_DIALOG2), eWindowBackgrounds::Alpha)
{
	auto textLabel = emplaceChild<cLabel> (cBox<cPosition> (getPosition() + cPosition (35, 33), getPosition() + cPosition (267, 132)), text, eUnicodeFontType::LatinNormal, toEnumFlag (eAlignmentType::CenterHorizontal) | eAlignmentType::Top);
	textLabel->setWordWrap (true);

	// the long text does not fit on the small buttons of the frame: a wide button below the text
	proceedButton = emplaceChild<cPushButton> (getPosition() + cPosition (75, 139), ePushButtonType::StandardSmall, proceedText, eUnicodeFontType::LatinNormal);
	signalConnectionManager.connect (proceedButton->clicked, [this]() {
		proceedClicked();
		close();
	});

	backButton = emplaceChild<cPushButton> (getPosition() + cPosition (111, 185), ePushButtonType::Angular, lngPack.i18n ("Others~Back"), eUnicodeFontType::LatinNormal);
	backButton->addClickShortcut (cKeySequence (cKeyCombination (SDLK_RETURN)));
	backButton->addClickShortcut (cKeySequence (cKeyCombination (SDLK_ESCAPE)));
	signalConnectionManager.connect (backButton->clicked, [this]() {
		backClicked();
		close();
	});
}

//------------------------------------------------------------------------------
cDialogProceedAnyway::~cDialogProceedAnyway()
{}

//------------------------------------------------------------------------------
void cDialogProceedAnyway::retranslate()
{
	cWindow::retranslate();

	backButton->setText (lngPack.i18n ("Others~Back"));
}
