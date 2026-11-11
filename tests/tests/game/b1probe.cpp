// Local probe of B1 (not part of the pull request): one length of a short map file per run.
#include "mapdownloader/mapdownload.h"
#include "testutility.h"

#include <cstdlib>
#include <doctest.h>
#include <iostream>
#include <vector>

TEST_CASE ("B1ProbeShortFile")
{
	const char* env = std::getenv ("B1_LENGTH");
	if (env == nullptr) return;
	const int length = std::atoi (env);
	const cTemporaryFile file (".wrl");
	const auto bytes = makeGroundMapFile (16);
	file.write (std::vector<unsigned char> (bytes.begin(), bytes.begin() + length));
	std::cout << "length " << length << ": checksum " << MapDownload::calculateCheckSum (file.getPath()) << std::endl;
}

TEST_CASE ("B1ProbeBigSize")
{
	const char* env = std::getenv ("B1_SIZE");
	if (env == nullptr) return;
	const int size = std::atoi (env);
	const cTemporaryFile file (".wrl");
	auto bytes = makeGroundMapFile (255);
	bytes[5] = bytes[7] = static_cast<unsigned char> (size & 0xFF);
	bytes[6] = bytes[8] = static_cast<unsigned char> (size >> 8);
	file.write (bytes);
	std::cout << "size " << size << ": checksum " << MapDownload::calculateCheckSum (file.getPath()) << std::endl;
}
