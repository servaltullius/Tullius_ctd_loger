// Workaround for MSVC 14.44 STL bug:
// The <regex> header references __std_regex_transform_primary_char but the
// shipped msvcp140 runtime library does not export this symbol yet.
// CommonLibSSE pulls in <regex>, causing LNK2001 at link time.
//
// This stub returns (size_t)-1, which the STL regex internals interpret as
// "no transformation available" and collapse to an empty string.  This is
// safe because CommonLibSSE does not use regex equivalence-class syntax.
//
// Remove this file once MSVC ships a runtime that includes the symbol.

#include <cstddef>

extern "C" std::size_t __stdcall __std_regex_transform_primary_char(
    char* /*first1*/, char* /*last1*/,
    const char* /*first2*/, const char* /*last2*/,
    const void* /*collvec*/) noexcept
{
    return static_cast<std::size_t>(-1);
}
