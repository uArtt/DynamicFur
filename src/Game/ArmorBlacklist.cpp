#include "PCH.h"

#include "DynamicFur/Game/ArmorBlacklist.h"

#include "DynamicFur/Core/JsonStringArray.h"

#include "REL/Module.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESObjectARMO.h"

#include <charconv>
#include <expected>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace DynamicFur::Game
{
	namespace
	{
		inline constexpr std::uintmax_t kMaximumConfigurationBytes = 1024U * 1024U;

		struct LocalFormSpec
		{
			std::string plugin;
			std::uint32_t localFormID{};
		};

		[[nodiscard]] std::string_view Trim(std::string_view a_value) noexcept
		{
			constexpr std::string_view whitespace{ " \t\r\n" };
			const auto first = a_value.find_first_not_of(whitespace);
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_value.find_last_not_of(whitespace);
			return a_value.substr(first, last - first + 1U);
		}

		[[nodiscard]] std::expected<LocalFormSpec, std::string> ParseFormSpec(
			std::string_view a_value)
		{
			a_value = Trim(a_value);
			const auto separator = a_value.rfind('|');
			if (separator == std::string_view::npos) {
				return std::unexpected("expected Plugin.esp|0xLocalFormID");
			}
			const auto plugin = Trim(a_value.substr(0, separator));
			auto formText = Trim(a_value.substr(separator + 1U));
			if (plugin.empty() || formText.empty()) {
				return std::unexpected("plugin name and local FormID must not be empty");
			}
			if (formText.starts_with("0x") || formText.starts_with("0X")) {
				formText.remove_prefix(2);
			}
			if (formText.empty()) {
				return std::unexpected("local FormID contains no hexadecimal digits");
			}

			std::uint32_t localFormID{};
			const auto parsed = std::from_chars(
				formText.data(), formText.data() + formText.size(), localFormID, 16);
			if (parsed.ec != std::errc{} || parsed.ptr != formText.data() + formText.size() ||
				localFormID == 0 || localFormID > 0x00FFFFFFU) {
				return std::unexpected("local FormID must be hexadecimal and in [0x1, 0xFFFFFF]");
			}
			return LocalFormSpec{ std::string{ plugin }, localFormID };
		}

		[[nodiscard]] std::filesystem::path ResolveFromGameRoot(
			const std::filesystem::path& a_relativePath)
		{
			if (a_relativePath.is_absolute()) {
				return a_relativePath.lexically_normal();
			}
			const auto executable = std::filesystem::path{
				REL::Module::get().filePath().data()
			};
			const auto root = executable.has_parent_path() ?
				executable.parent_path() : std::filesystem::current_path();
			return (root / a_relativePath).lexically_normal();
		}

		[[nodiscard]] std::expected<std::string, std::string> ReadConfiguration(
			const std::filesystem::path& a_path)
		{
			std::error_code error;
			const auto size = std::filesystem::file_size(a_path, error);
			if (error) {
				return std::unexpected("could not query file size: " + error.message());
			}
			if (size > kMaximumConfigurationBytes ||
				size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)())) {
				return std::unexpected("configuration exceeds the 1 MiB safety limit");
			}

			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) {
				return std::unexpected("could not open the file");
			}
			std::string result(static_cast<std::size_t>(size), '\0');
			if (!result.empty() &&
				!stream.read(result.data(), static_cast<std::streamsize>(result.size()))) {
				return std::unexpected("could not read the complete file");
			}
			return result;
		}
	}

	ArmorBlacklist::ArmorBlacklist(std::filesystem::path a_relativePath) :
		relativePath_(std::move(a_relativePath))
	{}

	void ArmorBlacklist::Load() noexcept
	{
		runtimeFormIDs_.clear();
		try {
			resolvedPath_ = ResolveFromGameRoot(relativePath_);
			std::error_code existenceError;
			const auto exists = std::filesystem::exists(resolvedPath_, existenceError);
			if (existenceError) {
				SKSE::log::error(
					"Could not inspect optional armor blacklist '{}': {}",
					resolvedPath_.string(), existenceError.message());
				return;
			}
			if (!exists) {
				SKSE::log::info(
					"Optional armor blacklist not found at '{}'; no ARMO records are excluded",
					resolvedPath_.string());
				return;
			}

			auto document = ReadConfiguration(resolvedPath_);
			if (!document) {
				SKSE::log::error(
					"Could not read armor blacklist '{}': {}",
					resolvedPath_.string(), document.error());
				return;
			}
			auto entries = Core::ParseJsonStringArray(*document);
			if (!entries) {
				SKSE::log::error(
					"Armor blacklist '{}' is invalid: {}",
					resolvedPath_.string(), entries.error());
				return;
			}

			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				SKSE::log::error("Armor blacklist could not resolve Forms because TESDataHandler is unavailable");
				return;
			}

			std::size_t rejected{};
			for (std::size_t index = 0; index < entries->size(); ++index) {
				auto spec = ParseFormSpec((*entries)[index]);
				if (!spec) {
					++rejected;
					SKSE::log::warn(
						"Ignoring armor blacklist entry {} ('{}'): {}",
						index, (*entries)[index], spec.error());
					continue;
				}
				auto* armor = dataHandler->LookupForm<RE::TESObjectARMO>(
					spec->localFormID, spec->plugin);
				if (!armor) {
					++rejected;
					SKSE::log::warn(
						"Ignoring unresolved armor blacklist entry {}: {}|0x{:06X}",
						index, spec->plugin, spec->localFormID);
					continue;
				}
				runtimeFormIDs_.insert(armor->GetFormID());
			}

			SKSE::log::info(
				"Armor blacklist ready: {} ARMO record(s) resolved from '{}' ({} rejected)",
				runtimeFormIDs_.size(), resolvedPath_.string(), rejected);
		} catch (const std::exception& error) {
			SKSE::log::error("Armor blacklist loading failed safely: {}", error.what());
			runtimeFormIDs_.clear();
		} catch (...) {
			SKSE::log::error("Armor blacklist loading failed safely with an unknown exception");
			runtimeFormIDs_.clear();
		}
	}

	bool ArmorBlacklist::Contains(const RE::TESObjectARMO* a_armor) const noexcept
	{
		return a_armor && runtimeFormIDs_.contains(a_armor->GetFormID());
	}

	std::size_t ArmorBlacklist::Size() const noexcept { return runtimeFormIDs_.size(); }
	const std::filesystem::path& ArmorBlacklist::Path() const noexcept { return resolvedPath_; }
}
