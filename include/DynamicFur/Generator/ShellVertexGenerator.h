#pragma once

#include "DynamicFur/Core/ShellCoverage.h"
#include "DynamicFur/Core/ShellProfile.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace DynamicFur::Generator
{
	enum class ShellVertexGenerationError : std::uint8_t
	{
		InvalidArgument,
		InvalidProfile,
		InvalidLayout,
		InvalidVertex,
		InvalidTopology,
		ResourceTooLarge
	};

	struct ShellVertexGenerationRequest
	{
		std::span<const std::byte> sourceVertices;
		const Core::ShellProfile* profile{};
		std::uint32_t vertexCount{};
		std::uint32_t vertexStride{};
		std::uint32_t positionOffset{};
		// Some valid Skyrim skinned streams omit normals entirely. When this is
		// empty, sourceIndices are used to reconstruct smooth extrusion normals.
		std::optional<std::uint32_t> packedNormalOffset;
		std::span<const std::uint16_t> sourceIndices;
		// Optional semantic stream supplied by adaptive topology generation. It
		// preserves the carrier's original smooth extrusion field at old vertices
		// and barycentrically interpolates it at new vertices. Reconstructing from
		// the refined triangles would change area weighting and create seams.
		std::span<const Core::CarrierVertex> predecodedVertices;
	};

	struct PackedCarrierVertexDecodeRequest
	{
		std::span<const std::byte> sourceVertices;
		std::uint32_t vertexCount{};
		std::uint32_t vertexStride{};
		std::uint32_t positionOffset{};
		std::optional<std::uint32_t> packedNormalOffset;
		std::span<const std::uint16_t> sourceIndices;
	};

	using PackedCarrierVertexDecodeResult =
		std::expected<std::vector<Core::CarrierVertex>, ShellVertexGenerationError>;

	// Decodes the exact positions/normals used by shell extrusion. Runtime
	// coverage consumes this same output so collision rays cannot disagree with
	// the generated fur direction.
	[[nodiscard]] PackedCarrierVertexDecodeResult DecodeCarrierVertices(
		const PackedCarrierVertexDecodeRequest& a_request);

	using ShellVertexGenerationResult =
		std::expected<std::vector<std::byte>, ShellVertexGenerationError>;

	// Pure CPU module: it knows nothing about Skyrim scene graphs, D3D resources
	// or hooks. The caller provides one packed skinned vertex stream and receives
	// one contiguous stream containing every canonical shell.
	[[nodiscard]] ShellVertexGenerationResult GenerateShellVertices(
		const ShellVertexGenerationRequest& a_request);
}
