#include "DynamicFur/Core/JsonStringArray.h"

#include <cctype>
#include <cstdint>
#include <format>

namespace DynamicFur::Core
{
	namespace
	{
		class Parser final
		{
		public:
			explicit Parser(std::string_view a_document) noexcept : document_(a_document) {}

			[[nodiscard]] std::expected<std::vector<std::string>, std::string> Parse()
			{
				SkipWhitespace();
				if (!Consume('[')) {
					return Fail("expected '[' at the root");
				}

				std::vector<std::string> result;
				SkipWhitespace();
				if (Consume(']')) {
					SkipWhitespace();
					if (!AtEnd()) {
						return Fail("unexpected data after ']'");
					}
					return result;
				}

				while (true) {
					auto value = ParseString();
					if (!value) {
						return std::unexpected(std::move(value.error()));
					}
					result.push_back(std::move(*value));
					SkipWhitespace();
					if (Consume(']')) {
						break;
					}
					if (!Consume(',')) {
						return Fail("expected ',' or ']' after a string");
					}
					SkipWhitespace();
				}

				SkipWhitespace();
				if (!AtEnd()) {
					return Fail("unexpected data after ']'");
				}
				return result;
			}

		private:
			[[nodiscard]] bool AtEnd() const noexcept { return position_ == document_.size(); }

			void SkipWhitespace() noexcept
			{
				while (!AtEnd() && std::isspace(static_cast<unsigned char>(document_[position_]))) {
					++position_;
				}
			}

			[[nodiscard]] bool Consume(char a_character) noexcept
			{
				if (AtEnd() || document_[position_] != a_character) {
					return false;
				}
				++position_;
				return true;
			}

			[[nodiscard]] std::unexpected<std::string> Fail(std::string_view a_message) const
			{
				return std::unexpected(std::format("JSON byte {}: {}", position_, a_message));
			}

			[[nodiscard]] static int HexDigit(char a_character) noexcept
			{
				if (a_character >= '0' && a_character <= '9') {
					return a_character - '0';
				}
				if (a_character >= 'a' && a_character <= 'f') {
					return a_character - 'a' + 10;
				}
				if (a_character >= 'A' && a_character <= 'F') {
					return a_character - 'A' + 10;
				}
				return -1;
			}

			[[nodiscard]] std::expected<std::uint32_t, std::string> ParseHexQuad()
			{
				if (document_.size() - position_ < 4) {
					return Fail("incomplete Unicode escape");
				}
				std::uint32_t value{};
				for (std::uint32_t digit = 0; digit < 4; ++digit) {
					const auto decoded = HexDigit(document_[position_++]);
					if (decoded < 0) {
						return Fail("invalid hexadecimal digit in Unicode escape");
					}
					value = value * 16U + static_cast<std::uint32_t>(decoded);
				}
				return value;
			}

			static void AppendUtf8(std::string& a_output, std::uint32_t a_codePoint)
			{
				if (a_codePoint <= 0x7FU) {
					a_output.push_back(static_cast<char>(a_codePoint));
				} else if (a_codePoint <= 0x7FFU) {
					a_output.push_back(static_cast<char>(0xC0U | (a_codePoint >> 6U)));
					a_output.push_back(static_cast<char>(0x80U | (a_codePoint & 0x3FU)));
				} else if (a_codePoint <= 0xFFFFU) {
					a_output.push_back(static_cast<char>(0xE0U | (a_codePoint >> 12U)));
					a_output.push_back(static_cast<char>(0x80U | ((a_codePoint >> 6U) & 0x3FU)));
					a_output.push_back(static_cast<char>(0x80U | (a_codePoint & 0x3FU)));
				} else {
					a_output.push_back(static_cast<char>(0xF0U | (a_codePoint >> 18U)));
					a_output.push_back(static_cast<char>(0x80U | ((a_codePoint >> 12U) & 0x3FU)));
					a_output.push_back(static_cast<char>(0x80U | ((a_codePoint >> 6U) & 0x3FU)));
					a_output.push_back(static_cast<char>(0x80U | (a_codePoint & 0x3FU)));
				}
			}

			[[nodiscard]] std::expected<std::string, std::string> ParseString()
			{
				if (!Consume('"')) {
					return Fail("array elements must be strings");
				}
				std::string result;
				while (!AtEnd()) {
					const auto character = document_[position_++];
					if (character == '"') {
						return result;
					}
					if (static_cast<unsigned char>(character) < 0x20U) {
						return Fail("unescaped control character in string");
					}
					if (character != '\\') {
						result.push_back(character);
						continue;
					}
					if (AtEnd()) {
						return Fail("incomplete string escape");
					}
					switch (document_[position_++]) {
					case '"': result.push_back('"'); break;
					case '\\': result.push_back('\\'); break;
					case '/': result.push_back('/'); break;
					case 'b': result.push_back('\b'); break;
					case 'f': result.push_back('\f'); break;
					case 'n': result.push_back('\n'); break;
					case 'r': result.push_back('\r'); break;
					case 't': result.push_back('\t'); break;
					case 'u': {
						auto first = ParseHexQuad();
						if (!first) {
							return std::unexpected(std::move(first.error()));
						}
						std::uint32_t codePoint = *first;
						if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
							if (!Consume('\\') || !Consume('u')) {
								return Fail("high surrogate must be followed by a low surrogate");
							}
							auto second = ParseHexQuad();
							if (!second) {
								return std::unexpected(std::move(second.error()));
							}
							if (*second < 0xDC00U || *second > 0xDFFFU) {
								return Fail("invalid low surrogate");
							}
							codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (*second - 0xDC00U);
						} else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
							return Fail("unexpected low surrogate");
						}
						AppendUtf8(result, codePoint);
						break;
					}
					default: return Fail("unsupported string escape");
					}
				}
				return Fail("unterminated string");
			}

			std::string_view document_;
			std::size_t position_{};
		};
	}

	std::expected<std::vector<std::string>, std::string>
		ParseJsonStringArray(std::string_view a_document)
	{
		// A UTF-8 BOM is accepted because several Windows editors add it by default.
		if (a_document.starts_with("\xEF\xBB\xBF")) {
			a_document.remove_prefix(3);
		}
		return Parser{ a_document }.Parse();
	}
}
