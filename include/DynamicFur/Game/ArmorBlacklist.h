#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_set>

namespace RE
{
	class TESObjectARMO;
}

namespace DynamicFur::Game
{
	// Optional, event-stable policy loaded once after Skyrim data is available.
	// Entries name ARMO records as "Plugin.esp|0xLocalFormID". Runtime load-order
	// IDs are deliberately not accepted, so one file works in every load order.
	class ArmorBlacklist final
	{
	public:
		explicit ArmorBlacklist(std::filesystem::path a_relativePath =
			std::filesystem::path{ "Data/SKSE/Plugins/DynamicFur/armor_blacklist.json" });

		void Load() noexcept;
		[[nodiscard]] bool Contains(const RE::TESObjectARMO* a_armor) const noexcept;
		[[nodiscard]] std::size_t Size() const noexcept;
		[[nodiscard]] const std::filesystem::path& Path() const noexcept;

	private:
		std::filesystem::path relativePath_;
		std::filesystem::path resolvedPath_;
		std::unordered_set<std::uint32_t> runtimeFormIDs_;
	};
}
