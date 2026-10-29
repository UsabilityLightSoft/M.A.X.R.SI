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

#include "input/mouse/cursor/mousecursorsimple.h"

#include "resources/uidata.h"
#include "utility/position.h"

#include <stdexcept>

//------------------------------------------------------------------------------
cMouseCursorSimple::cMouseCursorSimple (eMouseCursorSimpleType type_) :
	type (type_)
{}

//------------------------------------------------------------------------------
SDL_Surface* cMouseCursorSimple::getSurface() const
{
	switch (type)
	{
		case eMouseCursorSimpleType::Hand: return GraphicsData.gfx_Chand.get();
		case eMouseCursorSimpleType::No: return GraphicsData.gfx_Cno.get();
		case eMouseCursorSimpleType::Select: return GraphicsData.gfx_Cselect.get();
		case eMouseCursorSimpleType::Move: return GraphicsData.gfx_Cmove.get();
		case eMouseCursorSimpleType::ArrowLeftDown: return GraphicsData.gfx_Cpfeil1.get();
		case eMouseCursorSimpleType::ArrowDown: return GraphicsData.gfx_Cpfeil2.get();
		case eMouseCursorSimpleType::ArrowRightDown: return GraphicsData.gfx_Cpfeil3.get();
		case eMouseCursorSimpleType::ArrowLeft: return GraphicsData.gfx_Cpfeil4.get();
		case eMouseCursorSimpleType::ArrowRight: return GraphicsData.gfx_Cpfeil6.get();
		case eMouseCursorSimpleType::ArrowLeftUp: return GraphicsData.gfx_Cpfeil7.get();
		case eMouseCursorSimpleType::ArrowUp: return GraphicsData.gfx_Cpfeil8.get();
		case eMouseCursorSimpleType::ArrowRightUp: return GraphicsData.gfx_Cpfeil9.get();
		case eMouseCursorSimpleType::Help: return GraphicsData.gfx_Chelp.get();
		case eMouseCursorSimpleType::Band: return GraphicsData.gfx_Cband.get();
		case eMouseCursorSimpleType::Transfer: return GraphicsData.gfx_Ctransf.get();
		case eMouseCursorSimpleType::Load: return GraphicsData.gfx_Cload.get();
		case eMouseCursorSimpleType::Muni: return GraphicsData.gfx_Cmuni.get();
		case eMouseCursorSimpleType::Repair: return GraphicsData.gfx_Crepair.get();
		case eMouseCursorSimpleType::Activate: return GraphicsData.gfx_Cactivate.get();
		case eMouseCursorSimpleType::MoveDraft: return GraphicsData.gfx_Cmove_draft.get();
	}
	throw std::runtime_error ("unreachable");
}

//------------------------------------------------------------------------------
cPosition cMouseCursorSimple::getHotPoint() const
{
	switch (type)
	{
		case eMouseCursorSimpleType::Select:
		case eMouseCursorSimpleType::Help:
		case eMouseCursorSimpleType::Move:
		case eMouseCursorSimpleType::MoveDraft:
		case eMouseCursorSimpleType::No:
		case eMouseCursorSimpleType::Transfer:
		case eMouseCursorSimpleType::Band:
		case eMouseCursorSimpleType::Load:
		case eMouseCursorSimpleType::Muni:
		case eMouseCursorSimpleType::Repair:
		case eMouseCursorSimpleType::Activate:
			return cPosition (12, 12);
		case eMouseCursorSimpleType::Hand:
			return cPosition (0, 0);
		default:
			break;
	}
	// scroll arrows: at the tip, so that the whole arrow is visible at the edge of the screen
	const auto* surface = getSurface();
	const int last = surface ? surface->w - 1 : 17;
	const int lastY = surface ? surface->h - 1 : 17;
	switch (type)
	{
		case eMouseCursorSimpleType::ArrowLeftDown: return cPosition (0, lastY);
		case eMouseCursorSimpleType::ArrowDown: return cPosition (last / 2, lastY);
		case eMouseCursorSimpleType::ArrowRightDown: return cPosition (last, lastY);
		case eMouseCursorSimpleType::ArrowLeft: return cPosition (0, lastY / 2);
		case eMouseCursorSimpleType::ArrowRight: return cPosition (last, lastY / 2);
		case eMouseCursorSimpleType::ArrowUp: return cPosition (last / 2, 0);
		case eMouseCursorSimpleType::ArrowRightUp: return cPosition (last, 0);
		default: return cPosition (0, 0);
	}
}

//------------------------------------------------------------------------------
bool cMouseCursorSimple::equal (const cMouseCursor& other) const
{
	auto other2 = dynamic_cast<const cMouseCursorSimple*> (&other);
	return other2 && other2->type == type;
}
