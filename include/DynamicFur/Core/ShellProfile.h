#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace DynamicFur::Core
{
	inline constexpr std::uint32_t kMaximumShellCount = 32;
	inline constexpr std::uint32_t kDefaultShellCount = 17;

	struct LodBand
	{
		float maximumDistance{};
		std::uint32_t shellCount{};
	};

	struct ShellProfile
	{
		std::uint32_t shellCount{ kDefaultShellCount };
		float furLength{ 0.868F };
		float distributionExponent{ 1.0F };
		float baseAlphaReference{ 0.0F };
		std::array<float, kMaximumShellCount> alphaThresholds{
			0.0F / 255.0F, 15.0F / 255.0F, 30.0F / 255.0F,
			45.0F / 255.0F, 60.0F / 255.0F, 75.0F / 255.0F,
			90.0F / 255.0F, 105.0F / 255.0F, 120.0F / 255.0F,
			135.0F / 255.0F, 150.0F / 255.0F, 165.0F / 255.0F,
			180.0F / 255.0F, 195.0F / 255.0F, 210.0F / 255.0F,
			225.0F / 255.0F, 240.0F / 255.0F
		};
		std::array<LodBand, 4> lodBands{
			LodBand{ 900.0F, 17 },
			LodBand{ 1600.0F, 12 },
			LodBand{ 2800.0F, 8 },
			LodBand{ std::numeric_limits<float>::infinity(), 4 }
		};
		float lodHysteresis{ 100.0F };
		std::uint32_t shadowShellCount{ 4 };
		float armorMargin{ 0.025F };
		// A small inward probe catches armor surfaces that clip just below the
		// carrier skin (most visibly undersized shoes around the feet).
		float armorUnderSkinDepth{ 0.5F };

		[[nodiscard]] static ShellProfile Legacy17() noexcept;
		[[nodiscard]] std::string Validate() const;
	};
}
