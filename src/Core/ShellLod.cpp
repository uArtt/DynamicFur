#include "DynamicFur/Core/ShellLod.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace DynamicFur::Core
{
	namespace
	{
		std::uint32_t FindBand(const ShellProfile& a_profile, float a_distance) noexcept
		{
			const auto distance = std::isfinite(a_distance) ? std::max(a_distance, 0.0F) :
				std::numeric_limits<float>::infinity();
			for (std::uint32_t index = 0; index < a_profile.lodBands.size(); ++index) {
				if (distance <= a_profile.lodBands[index].maximumDistance) {
					return index;
				}
			}
			return static_cast<std::uint32_t>(a_profile.lodBands.size() - 1);
		}
	}

	std::uint32_t SelectShellCount(
		const ShellProfile& a_profile,
		float a_distance) noexcept
	{
		return a_profile.lodBands[FindBand(a_profile, a_distance)].shellCount;
	}

	std::uint32_t LodSelector::Update(
		const ShellProfile& a_profile,
		float a_distance) noexcept
	{
		const auto distance = std::isfinite(a_distance) ? std::max(a_distance, 0.0F) :
			std::numeric_limits<float>::infinity();
		if (!initialized_) {
			bandIndex_ = FindBand(a_profile, distance);
			currentShellCount_ = a_profile.lodBands[bandIndex_].shellCount;
			initialized_ = true;
			return currentShellCount_;
		}

		while (bandIndex_ + 1 < a_profile.lodBands.size() &&
			distance > a_profile.lodBands[bandIndex_].maximumDistance + a_profile.lodHysteresis) {
			++bandIndex_;
		}
		while (bandIndex_ > 0 &&
			distance < a_profile.lodBands[bandIndex_ - 1].maximumDistance - a_profile.lodHysteresis) {
			--bandIndex_;
		}
		currentShellCount_ = a_profile.lodBands[bandIndex_].shellCount;
		return currentShellCount_;
	}

	void LodSelector::Reset() noexcept
	{
		bandIndex_ = 0;
		currentShellCount_ = 0;
		initialized_ = false;
	}

	std::uint32_t LodSelector::CurrentShellCount() const noexcept
	{
		return currentShellCount_;
	}
}
