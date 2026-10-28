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

#include "game/data/player/player.h"
#include "game/data/player/playerbasicdata.h"
#include "game/data/units/unitdata.h"

#include <doctest.h>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace
{
	// " name=crc name=crc ..." -> name -> crc
	std::map<std::string, std::string> parse (const std::string& report)
	{
		std::map<std::string, std::string> parts;
		std::istringstream in (report);
		std::string pair;
		while (in >> pair)
		{
			const auto eq = pair.find ('=');
			REQUIRE (eq != std::string::npos);
			parts[pair.substr (0, eq)] = pair.substr (eq + 1);
		}
		return parts;
	}
} // namespace

//------------------------------------------------------------------------------
// When a client is out of sync, the parts of the checksum are written to the net log
// (cModel::getChecksumReport): comparing the client and the server line has to name the diverging data.
TEST_CASE ("PlayerChecksumReportNamesTheDivergingPart")
{
	const cUnitsData unitsData;
	const cPlayerBasicData basicData ({"Player", cRgbColor::red()}, 3, false);
	cPlayer server (basicData, unitsData);
	cPlayer client (basicData, unitsData);

	auto serverParts = parse (server.getChecksumReport());
	CHECK (serverParts == parse (client.getChecksumReport()));
	CHECK (serverParts["p3.total"] == std::to_string (server.getChecksum (0)));
	CHECK (serverParts.count ("p3.credits") == 1);

	client.setCredits (client.getCredits() + 1);
	const auto clientParts = parse (client.getChecksumReport());

	std::set<std::string> diverging;
	for (const auto& [name, crc] : clientParts)
	{
		if (serverParts[name] != crc) diverging.insert (name);
	}
	CHECK (diverging == std::set<std::string>{"p3.credits", "p3.total"});
	CHECK (clientParts.at ("p3.total") == std::to_string (client.getChecksum (0)));
}
