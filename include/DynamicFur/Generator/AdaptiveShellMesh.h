#pragma once

#include "DynamicFur/Core/ShellCoverage.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace DynamicFur::Generator
{
	// The first adaptive implementation deliberately targets the live SSE stream
	// observed on the 3BA carrier. Keeping the packed-layout contract explicit
	// lets callers fall back to the conservative binary mask for every other
	// layout instead of guessing at renderer data.
	struct AdaptiveShellMeshOptions
	{
		std::uint32_t maxDepth{ 3 };
		std::uint32_t maxVertices{ 65535 };
		std::uint32_t maxTriangles{ 262144 };
	};

	struct AdaptiveShellMeshRequest
	{
		std::span<const std::byte> sourceVertices;
		std::span<const std::uint16_t> sourceIndices;
		std::span<const Core::CarrierVertex> referenceVertices;
		std::span<const Core::CarrierVertex> posedVertices;
		const Core::ArmorCoverageQuery* query{};

		std::uint32_t vertexStride{};
		std::uint32_t positionOffset{};
		std::uint32_t uvOffset{};
		std::uint32_t skinningOffset{};
		std::uint32_t influencesPerVertex{};
		AdaptiveShellMeshOptions options{};
	};

	enum class AdaptiveShellMeshError : std::uint8_t
	{
		InvalidArgument,
		UnsupportedLayout,
		InvalidVertex,
		InvalidTopology,
		InvalidSkinning,
		CoverageFailure,
		BudgetExceeded,
		ResourceTooLarge
	};

	struct AdaptiveShellMeshStats
	{
		std::uint32_t refinedSourceTriangleCount{};
		std::uint32_t coarseHiddenSourceTriangleCount{};
		std::uint32_t addedVertexCount{};
		std::uint32_t candidateTriangleCount{};
		std::uint32_t sampleCount{};
		std::uint32_t usedDepth{};
		bool topologyChanged{};
	};

	struct AdaptiveShellMeshResult
	{
		// Original vertices remain at the beginning, byte-for-byte and in their
		// original order. New R16 indices may therefore address both original and
		// interpolated vertices through one packed base stream.
		std::vector<std::byte> packedVertices;
		std::vector<std::uint16_t> indices;
		std::vector<Core::CarrierVertex> referenceVertices;
		AdaptiveShellMeshStats stats;
	};

	using AdaptiveShellMeshGenerationResult =
		std::expected<AdaptiveShellMeshResult, AdaptiveShellMeshError>;

	// Performs only the coarse discovery and deterministic candidate tessellation.
	// Generated vertices still need to be posed and sampled by the caller before
	// covered micro-triangles are compacted from the final shell index buffer.
	[[nodiscard]] AdaptiveShellMeshGenerationResult GenerateAdaptiveShellMesh(
		const AdaptiveShellMeshRequest& a_request);
}
