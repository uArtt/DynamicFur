#include "DynamicFur/Core/ShellProfile.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace DynamicFur::Core
{
	ShellProfile ShellProfile::Legacy17() noexcept { return {}; }

	std::string ShellProfile::Validate() const
	{
		if (shellCount == 0 || shellCount > kMaximumShellCount) {
			return std::format("shellCount must be in [1, {}]", kMaximumShellCount);
		}
		if (!std::isfinite(furLength) || furLength <= 0.0F) {
			return "furLength must be finite and greater than zero";
		}
		if (!std::isfinite(distributionExponent) || distributionExponent <= 0.0F) {
			return "distributionExponent must be finite and greater than zero";
		}
		if (!std::isfinite(baseAlphaReference) ||
			baseAlphaReference < 0.0F || baseAlphaReference > 1.0F) {
			return "baseAlphaReference must be in [0, 1]";
		}

		float previousThreshold = 0.0F;
		for (std::uint32_t index = 0; index < shellCount; ++index) {
			const auto threshold = alphaThresholds[index];
			if (!std::isfinite(threshold) || threshold < baseAlphaReference || threshold > 1.0F) {
				return std::format("alphaThresholds[{}] must be in [baseAlphaReference, 1]", index);
			}
			if (index != 0 && threshold < previousThreshold) {
				return "alphaThresholds must be monotonic";
			}
			previousThreshold = threshold;
		}

		float previousDistance = 0.0F;
		std::uint32_t previousShellCount = shellCount;
		for (std::size_t index = 0; index < lodBands.size(); ++index) {
			const auto& band = lodBands[index];
			if (band.shellCount == 0 || band.shellCount > shellCount) {
				return std::format("lodBands[{}].shellCount is outside the profile", index);
			}
			if (std::isnan(band.maximumDistance) || band.maximumDistance <= previousDistance) {
				return "LOD distances must be strictly increasing";
			}
			if (band.shellCount > previousShellCount) {
				return "LOD shell counts must be non-increasing";
			}
			previousDistance = band.maximumDistance;
			previousShellCount = band.shellCount;
		}
		if (!std::isinf(lodBands.back().maximumDistance)) {
			return "the final LOD band must have an infinite maximum distance";
		}
		if (!std::isfinite(lodHysteresis) || lodHysteresis < 0.0F) {
			return "lodHysteresis must be finite and non-negative";
		}
		if (lodHysteresis >= lodBands.front().maximumDistance) {
			return "lodHysteresis must be smaller than the first LOD distance";
		}
		if (shadowShellCount == 0 || shadowShellCount > shellCount) {
			return "shadowShellCount is outside the profile";
		}
		if (!std::isfinite(armorMargin) || armorMargin < 0.0F) {
			return "armorMargin must be finite and non-negative";
		}
		if (!std::isfinite(armorUnderSkinDepth) || armorUnderSkinDepth < 0.0F) {
			return "armorUnderSkinDepth must be finite and non-negative";
		}
		if (!std::isfinite(furLength + std::max(armorMargin, armorUnderSkinDepth))) {
			return "furLength plus the coverage backtrack must be finite";
		}
		return {};
	}
}
