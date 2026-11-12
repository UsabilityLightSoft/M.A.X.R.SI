// std::uintmax_t (64 bits) to std::size_t (32 bits in an x86 build).
#include <cstddef>
#include <filesystem>

std::size_t fileSize (const std::filesystem::path& path)
{
	const std::size_t size = std::filesystem::file_size (path);
	return size;
}
