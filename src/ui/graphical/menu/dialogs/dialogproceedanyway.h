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

#ifndef ui_graphical_menu_dialogs_dialogproceedanywayH
#define ui_graphical_menu_dialogs_dialogproceedanywayH

#include "ui/widgets/window.h"
#include "utility/signal/signal.h"
#include "utility/signal/signalconnectionmanager.h"

#include <string>

class cPushButton;

/**
 * A warning with two answers: "Back" (the safe one, also Enter and Escape)
 * and a wide button to proceed anyway (its text says what happens).
 */
class cDialogProceedAnyway : public cWindow
{
public:
	cDialogProceedAnyway (const std::string& text, const std::string& proceedText);
	~cDialogProceedAnyway();

	void retranslate() override;

	cSignal<void()> proceedClicked;
	cSignal<void()> backClicked;

private:
	cSignalConnectionManager signalConnectionManager;

	cPushButton* proceedButton = nullptr;
	cPushButton* backButton = nullptr;
};

#endif // ui_graphical_menu_dialogs_dialogproceedanywayH
