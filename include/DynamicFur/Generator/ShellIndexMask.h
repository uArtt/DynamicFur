#pragma once

#include "DynamicFur/Core/ShellCoverage.h"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace DynamicFur::Generator
{
	enum class ShellIndexMaskError : std::uint8_t
	{
		InvalidArgument,
		InvalidIndex
	};

	struct ShellIndexMaskRequest
	{
		std::span<const std::uint16_t> sourceIndices;
		std::span<const Core::FirstHiddenShell> coverage;
		std::uint32_t vertexCount{};
		std::uint32_t shellCount{};
	};

	struct ShellIndexMaskResult
	{
		// The output deliberately retains the source index count. Covered
		// triangles become degenerates, so Skyrim's original draw ranges remain
		// valid and no shader or input-layout substitution is required.
		std::vector<std::uint16_t> indices;
		std::uint32_t hiddenTriangleCount{};

		[[nodiscard]] bool HasCoverage() const noexcept
		{
			return hiddenTriangleCount != 0;
		}
	};

	using ShellIndexMaskGenerationResult =
		std::expected<ShellIndexMaskResult, ShellIndexMaskError>;

	// Binary armor policy: if any vertex of a triangle reaches an armor surface
	// anywhere inside the fur length, remove that triangle from every shell.
	// This is intentionally conservative; the removed edge remains beneath the
	// equipped piece and cannot leak one shell at a time through it.
	[[nodiscard]] ShellIndexMaskGenerationResult GenerateBinaryShellIndexMask(
		const ShellIndexMaskRequest& a_request);

	struct CompactShellIndexMaskRequest
	{
		std::span<const std::uint16_t> sourceIndices;
		std::span<const Core::FirstHiddenShell> vertexCoverage;
		// Optional extra sample at each triangle centroid. This detects an armor
		// feature that lies wholly inside a refined micro-triangle.
		std::span<const Core::FirstHiddenShell> triangleCoverage;
		std::uint32_t vertexCount{};
		std::uint32_t shellCount{};
	};

	struct CompactShellIndexMaskResult
	{
		std::vector<std::uint16_t> indices;
		std::uint32_t hiddenTriangleCount{};
		std::uint32_t visibleTriangleCount{};
		bool emittedSafetyDegenerate{};
	};

	using CompactShellIndexMaskGenerationResult =
		std::expected<CompactShellIndexMaskResult, ShellIndexMaskError>;

	// Refined topology no longer has to retain Skyrim's original draw count.
	// Covered micro-triangles are therefore removed instead of merely collapsed,
	// reducing both index memory and per-shell raster work. A single degenerate is
	// emitted for a completely covered carrier because D3D11 immutable buffers
	// cannot have a zero ByteWidth.
	[[nodiscard]] CompactShellIndexMaskGenerationResult GenerateCompactShellIndexMask(
		const CompactShellIndexMaskRequest& a_request);
}
