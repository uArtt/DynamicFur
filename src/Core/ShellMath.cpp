#include "DynamicFur/Core/ShellMath.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace DynamicFur::Core
{
	namespace
	{
		[[nodiscard]] std::uint32_t EffectiveProfileCount(
			std::uint32_t a_profileShellCount) noexcept
		{
			return std::clamp(a_profileShellCount, 1U, kMaximumShellCount);
		}

		[[nodiscard]] ShellSample ComputeSample(
			const ShellProfile& a_profile,
			std::uint32_t a_localIndex,
			std::uint32_t a_activeShellCount) noexcept
		{
			const auto profileCount = EffectiveProfileCount(a_profile.shellCount);
			const auto distributionExponent =
				std::isfinite(a_profile.distributionExponent) && a_profile.distributionExponent > 0.0F ?
					a_profile.distributionExponent : 1.0F;
			const auto furLength = std::isfinite(a_profile.furLength) && a_profile.furLength > 0.0F ?
				a_profile.furLength : 0.0F;
			const auto baseAlphaReference =
				std::isfinite(a_profile.baseAlphaReference) &&
					a_profile.baseAlphaReference >= 0.0F && a_profile.baseAlphaReference <= 1.0F ?
					a_profile.baseAlphaReference : 0.0F;

			ShellSample result;
			result.localIndex = a_localIndex;
			result.canonicalIndex = CanonicalLayerIndex(
				a_localIndex, a_activeShellCount, profileCount);

			const auto denominator = profileCount > 1 ? profileCount - 1 : 1;
			const auto linearHeight = static_cast<float>(result.canonicalIndex) /
				static_cast<float>(denominator);
			result.normalizedHeight = std::pow(
				std::clamp(linearHeight, 0.0F, 1.0F), distributionExponent);
			result.offset = furLength * result.normalizedHeight;

			const auto rawThreshold = a_profile.alphaThresholds[result.canonicalIndex];
			result.alphaThreshold = std::isfinite(rawThreshold) ?
				std::clamp(rawThreshold, baseAlphaReference, 1.0F) : baseAlphaReference;
			result.vertexAlphaScale = result.alphaThreshold > 0.0F ?
				std::clamp((1.0F / 255.0F) / result.alphaThreshold, 0.0F, 1.0F) : 1.0F;
			return result;
		}
	}

	std::uint32_t CanonicalLayerIndex(
		std::uint32_t a_localIndex,
		std::uint32_t a_activeShellCount,
		std::uint32_t a_profileShellCount) noexcept
	{
		const auto profileCount = EffectiveProfileCount(a_profileShellCount);
		if (profileCount <= 1 || a_activeShellCount <= 1) {
			return 0;
		}

		const auto clampedCount = std::min(a_activeShellCount, profileCount);
		const auto clampedIndex = std::min(a_localIndex, clampedCount - 1);
		const auto numerator = static_cast<std::uint64_t>(clampedIndex) * (profileCount - 1);
		const auto denominator = static_cast<std::uint64_t>(clampedCount - 1);
		return static_cast<std::uint32_t>((numerator + denominator / 2) / denominator);
	}

	ShellSample SampleShell(
		const ShellProfile& a_profile,
		std::uint32_t a_localIndex,
		std::uint32_t a_activeShellCount) noexcept
	{
		return ComputeSample(a_profile, a_localIndex, a_activeShellCount);
	}

	std::expected<ShellLayout, std::string> ShellLayout::Create(
		const ShellProfile& a_profile)
	{
		if (auto validation = a_profile.Validate(); !validation.empty()) {
			return std::unexpected(std::move(validation));
		}
		return ShellLayout(a_profile);
	}

	ShellLayout::ShellLayout(const ShellProfile& a_profile) noexcept : profile_(a_profile)
	{
		for (std::uint32_t index = 0; index < profile_.shellCount; ++index) {
			canonicalSamples_[index] = ComputeSample(profile_, index, profile_.shellCount);
		}
	}

	const ShellProfile& ShellLayout::Profile() const noexcept { return profile_; }

	std::uint32_t ShellLayout::ShellCount() const noexcept { return profile_.shellCount; }

	std::span<const ShellSample> ShellLayout::CanonicalSamples() const noexcept
	{
		return { canonicalSamples_.data(), profile_.shellCount };
	}

	ShellSample ShellLayout::Sample(
		std::uint32_t a_localIndex,
		std::uint32_t a_activeShellCount) const noexcept
	{
		const auto canonicalIndex = CanonicalLayerIndex(
			a_localIndex, a_activeShellCount, profile_.shellCount);
		auto result = canonicalSamples_[canonicalIndex];
		result.localIndex = a_localIndex;
		return result;
	}

	std::uint32_t ShellLayout::FirstShellAtOrBeyond(float a_distance) const noexcept
	{
		if (!std::isfinite(a_distance)) {
			return profile_.shellCount;
		}
		if (a_distance <= 0.0F) {
			return 0;
		}

		const auto samples = CanonicalSamples();
		const auto match = std::lower_bound(
			samples.begin(), samples.end(), a_distance,
			[](const ShellSample& a_sample, float a_value) {
				return a_sample.offset < a_value;
			});
		return static_cast<std::uint32_t>(std::distance(samples.begin(), match));
	}

	bool IsShellVisible(
		std::uint32_t a_canonicalIndex,
		std::uint8_t a_firstHiddenShell) noexcept
	{
		return a_canonicalIndex < static_cast<std::uint32_t>(a_firstHiddenShell);
	}
}
