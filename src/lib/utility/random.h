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

#ifndef utility_randomH
#define utility_randomH

#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <type_traits>

/**
 * Fixed default seed used when setRandomSeed() is never called.
 *
 * Previously every run pulled a seed from std::random_device (a different
 * value each launch) and, worse, each instantiation of random<T>() held its
 * OWN static std::mt19937. That meant multiple independent generators and a
 * non-reproducible run. Now there is ONE shared generator (see
 * getRandomGenerator below); this constant makes even unseeded runs
 * reproducible.
 */
constexpr uint32_t kDefaultRandomSeed = 0x9E3779B9u;

/**
 * The single shared PRNG for the whole game.
 *
 * Every random<T>(...) instantiation draws from this ONE generator, so all
 * calls consume one common number stream and a single seed fully determines
 * the sequence. (Previously each template instantiation T had its own static
 * std::mt19937 seeded from std::random_device — several independent,
 * non-reproducible streams.)
 */
inline std::mt19937& getRandomGenerator()
{
	static std::mt19937 gen (kDefaultRandomSeed);
	return gen;
}

/**
 * Reseed the shared PRNG. Call BEFORE any random() use (i.e. before the game
 * starts) to make a run reproducible. If never called, the generator uses
 * kDefaultRandomSeed, so unseeded runs are still deterministic.
 */
inline void setRandomSeed (uint32_t seed)
{
	getRandomGenerator().seed (seed);
}

/**
 * Creates a uniform distributed random number in the interval [min, max).
 *
 * It is required that min < max.
 *
 * @tparam T Has to be an arithmetic type (integral or floating point).
 *           NOTE: char is not yet supported since it is not an IntType by the definition of the C++ standard
 *                 and therefor it is not supported by std::uniform_int_distribution.
 * @param min Lower bound of the interval.
 * @param max Upper bound of the interval.
 * @return Randomly selected value.
 */
template <typename T>
T random (const T min, const T max)
{
	assert (max > min);

	static_assert (std::is_arithmetic<T>::value, "Can not generate random number for non-arithmetic types");
	using DistributionType = std::conditional_t<
		std::is_integral<T>::value,
		std::uniform_int_distribution<T>,
		std::uniform_real_distribution<T>>;

	std::mt19937& gen = getRandomGenerator();

	DistributionType distribution (min, std::is_integral<T>::value ? max - 1 : max);

	return distribution (gen);
}

/**
 * Creates a uniform distributed random number in the interval [0, max).
 *
 * @tparam T Has to be an arithmetic type (integral or floating point).
 *           NOTE: char is not yet supported since it is not an IntType by the definition of the C++ standard
 *                 and therefor it is not supported by std::uniform_int_distribution (see http://open-std.org/JTC1/SC22/WG21/docs/lwg-active.html#2326).
 * @param max Upper bound of the interval.
 * @return Randomly selected value.
 */
template <typename T>
T random (const T max)
{
	return random (T (0), max);
}

inline bool randomBernoulli()
{
	return random (2) == 1;
}

/**
 * Select a random element from a std::array
 */
template <typename T, size_t N>
typename std::array<T, N>::reference getRandom (std::array<T, N>& data)
{
	static_assert (N > 0, "Getting random element from empty array is not allowed");
	return data[random (N)];
}

/**
 * Select a random element from a std::array
 */
template <typename T, size_t N>
typename std::array<T, N>::const_reference getRandom (const std::array<T, N>& data)
{
	static_assert (N > 0, "Getting random element from empty array is not allowed");
	return data[random (N)];
}

/**
 * Select a random element from a std::vector
 */
template <typename T>
const T& getRandom (const std::vector<T>& v)
{
	if (v.empty())
	{
		throw std::runtime_error ("Empty vector");
	}
	return v[random (v.size())];
}

#endif // utility_randomH
