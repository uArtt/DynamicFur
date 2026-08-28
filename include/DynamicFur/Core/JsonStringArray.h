#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace DynamicFur::Core
{
	// Parses the deliberately small configuration contract used by Dynamic Fur:
	// one JSON array whose elements are strings. Keeping this parser in Core makes
	// malformed user configuration testable without loading Skyrim or SKSE.
	[[nodiscard]] std::expected<std::vector<std::string>, std::string>
		ParseJsonStringArray(std::string_view a_document);
}
