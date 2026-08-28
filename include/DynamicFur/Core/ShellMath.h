#pragma once

#include "DynamicFur/Core/ShellProfile.h"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace DynamicFur::Core
{
	struct ShellSample
	{
		std::uint32_t localIndex{};
		std::uint32_t canonicalIndex{};
		float normalizedHeight{};
		float offset{};
		float alphaThreshold{};
		float vertexAlphaScale{};
	};

	// Immutable, validated data derived from one profile. Build this once and
	// share it between coverage generation and render-data generation.
	class ShellLayout final
	{
	public:
		[[nodiscard]] static std::expected<ShellLayout, std::string> Create(
			const ShellProfile& a_profile);

		[[nodiscard]] const ShellProfile& Profile() const noexcept;
		[[nodiscard]] std::uint32_t ShellCount() const noexcept;
		[[nodiscard]] std::span<const ShellSample> CanonicalSamples() const noexcept;
		[[nodiscard]] ShellSample Sample(
			std::uint32_t a_localIndex,
			std::uint32_t a_activeShellCount) const noexcept;

		// Returns ShellCount() when no shell reaches the requested distance.
		[[nodiscard]] std::uint32_t FirstShellAtOrBeyond(float a_distance) const noexcept;

	private:
		explicit ShellLayout(const ShellProfile& a_profile) noexcept;

		ShellProfile profile_;
		std::array<ShellSample, kMaximumShellCount> canonicalSamples_{};
	};

	[[nodiscard]] std::uint32_t CanonicalLayerIndex(
		std::uint32_t a_localIndex,
		std::uint32_t a_activeShellCount,
		std::uint32_t a_profileShellCount) noexcept;

	[[nodiscard]] ShellSample SampleShell(
		const ShellProfile& a_profile,
		std::uint32_t a_localIndex,
		std::uint32_t a_activeShellCount) noexcept;

	[[nodiscard]] bool IsShellVisible(
		std::uint32_t a_canonicalIndex,
		std::uint8_t a_firstHiddenShell) noexcept;
}
