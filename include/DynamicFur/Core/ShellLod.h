#pragma once

#include "DynamicFur/Core/ShellProfile.h"

#include <cstdint>

namespace DynamicFur::Core
{
	[[nodiscard]] std::uint32_t SelectShellCount(
		const ShellProfile& a_profile,
		float a_distance) noexcept;

	class LodSelector final
	{
	public:
		[[nodiscard]] std::uint32_t Update(
			const ShellProfile& a_profile,
			float a_distance) noexcept;

		void Reset() noexcept;

		[[nodiscard]] std::uint32_t CurrentShellCount() const noexcept;

	private:
		std::uint32_t bandIndex_{};
		std::uint32_t currentShellCount_{};
		bool initialized_{};
	};
}
