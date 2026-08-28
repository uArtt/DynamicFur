#pragma once

#include "DynamicFur/Core/ShellCoverage.h"

#include "RE/N/NiSkinPartition.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
	class NiNode;
}

namespace DynamicFur::Game
{
	struct PosedVertexRequest
	{
		RE::BSGeometry* geometry{};
		RE::NiNode* skeletonRoot{};
		const RE::NiSkinPartition::Partition* partition{};
		std::span<const std::byte> packedVertices;
		std::span<const std::uint16_t> sourceIndices;
		std::span<const Core::CarrierVertex> referenceVertices;
		std::uint32_t vertexStride{};
		std::uint32_t skinningOffset{};
	};

	// CPU snapshot of the same four-weight skinning deformation Skyrim applies
	// on the GPU. Results are expressed in skeleton-root space, allowing carrier
	// rays and every equipped ARMA to share one coordinate system.
	[[nodiscard]] std::expected<std::vector<Core::CarrierVertex>, std::string>
		PoseVertices(const PosedVertexRequest& a_request);
}
