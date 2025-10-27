/* ---------------------------------------------------------------------------
** C++98 Compatibility Layer
**
** Provides modern C++ conveniences for older toolchains (GCC 4.5.2, etc.)
** Usage: Replace std:: with compat:: for features introduced after C++98
** -------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <sstream>
#include <iterator>

namespace compat {

// C++11 std::to_string replacement
template<typename T>
inline std::string to_string(T value)
{
	std::ostringstream oss;
	oss << value;
	return oss.str();
}

// C++11 std::make_pair helper that works with C++98 (avoids brace-init issues)
template<typename InputIterator>
inline std::string string_from_istreambuf(InputIterator first, InputIterator last)
{
	return std::string(first, last);
}

// Helper to create string from istreambuf_iterator with C++98-compatible syntax
inline std::string read_stream_to_string(std::istream& is)
{
	return std::string((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
}

} // namespace compat
