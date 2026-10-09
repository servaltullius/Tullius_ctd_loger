#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace skydiag::dump_tool {

// Which recommendation the report shows as NextAction. The checklist is ordered
// for reading, not for acting: it often starts with background such as
// "[Basics] ExceptionCode=0xC0000005 ..." or a generic signature line, so taking
// the first entry gave field reports a NextAction that was not an action.
// Tags are matched in priority order (modal dialog, actionable/top candidate,
// Crash Logger frame, object ref, conflict); without one, the first entry that
// is not background-only is used. Returns npos for an empty list.
std::size_t SelectNextActionIndex(const std::vector<std::wstring>& recommendations);

}  // namespace skydiag::dump_tool
