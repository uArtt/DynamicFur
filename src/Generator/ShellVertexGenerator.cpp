#include "DynamicFur/Generator/ShellVertexGenerator.h"

#include "DynamicFur/Core/ShellMath.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace DynamicFur::Generator
{
	namespace
	{
		[[nodiscard]] std::array<float, 3> DecodePackedNormal(
			const std::byte* a_data) noexcept
		{
			constexpr float scale = 2.0F / 255.0F;
			std::array<float, 3> normal{
				static_cast<float>(std::to_integer<std::uint8_t>(a_data[0])) * scale - 1.0F,
				static_cast<float>(std::to_integer<std::uint8_t>(a_data[1])) * scale - 1.0F,
				static_cast<float>(std::to_integer<std::uint8_t>(a_data[2])) * scale - 1.0F
			};

			const auto lengthSquared =
				normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
			if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8F) {
				return {};
			}

			const auto inverseLength = 1.0F / std::sqrt(lengthSquared);
			for (auto& component : normal) {
				component *= inverseLength;
			}
			return normal;
		}

		[[nodiscard]] bool RangesOverlap(
			std::uint32_t a_firstOffset,
			std::uint32_t a_firstSize,
			std::uint32_t a_secondOffset,
			std::uint32_t a_secondSize) noexcept
		{
			return a_firstOffset < a_secondOffset + a_secondSize &&
				a_secondOffset < a_firstOffset + a_firstSize;
		}

		[[nodiscard]] float LengthSquared(const std::array<float, 3>& a_vector) noexcept
		{
			return a_vector[0] * a_vector[0] + a_vector[1] * a_vector[1] +
				a_vector[2] * a_vector[2];
		}

		[[nodiscard]] bool Normalize(std::array<float, 3>& a_vector) noexcept
		{
			const auto lengthSquared = LengthSquared(a_vector);
			if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) {
				return false;
			}
			const auto inverseLength = 1.0F / std::sqrt(lengthSquared);
			for (auto& component : a_vector) {
				component *= inverseLength;
			}
			return true;
		}

		[[nodiscard]] bool ReconstructNormals(
			std::span<const std::array<float, 3>> a_positions,
			std::span<const std::uint16_t> a_indices,
			std::span<std::array<float, 3>> a_normals) noexcept
		{
			if (a_positions.empty() || a_positions.size() != a_normals.size() ||
				a_indices.size() < 3 || (a_indices.size() % 3) != 0) {
				return false;
			}

			for (std::size_t triangle = 0; triangle < a_indices.size(); triangle += 3) {
				const auto i0 = static_cast<std::size_t>(a_indices[triangle]);
				const auto i1 = static_cast<std::size_t>(a_indices[triangle + 1]);
				const auto i2 = static_cast<std::size_t>(a_indices[triangle + 2]);
				if (i0 >= a_positions.size() || i1 >= a_positions.size() ||
					i2 >= a_positions.size()) {
					return false;
				}

				const auto& p0 = a_positions[i0];
				const auto& p1 = a_positions[i1];
				const auto& p2 = a_positions[i2];
				const std::array<float, 3> edge1{
					p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
				};
				const std::array<float, 3> edge2{
					p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
				};
				const std::array<float, 3> face{
					edge1[1] * edge2[2] - edge1[2] * edge2[1],
					edge1[2] * edge2[0] - edge1[0] * edge2[2],
					edge1[0] * edge2[1] - edge1[1] * edge2[0]
				};
				if (!std::isfinite(LengthSquared(face))) {
					return false;
				}
				for (const auto index : { i0, i1, i2 }) {
					for (std::size_t axis = 0; axis < 3; ++axis) {
						a_normals[index][axis] += face[axis];
					}
				}
			}

			// Triangle winding is consistent, but its sign is renderer-specific.
			// Pick the global orientation that points away from the mesh centroid.
			std::array<float, 3> centroid{};
			for (const auto& position : a_positions) {
				for (std::size_t axis = 0; axis < 3; ++axis) {
					centroid[axis] += position[axis];
				}
			}
			for (auto& component : centroid) {
				component /= static_cast<float>(a_positions.size());
			}

			float orientation{};
			for (std::size_t vertex = 0; vertex < a_normals.size(); ++vertex) {
				if (!Normalize(a_normals[vertex])) {
					// A skin partition may reference only a subset of a shared vertex
					// stream. Unreferenced vertices are not drawn by this sidecar, but
					// still need finite data in the expanded buffer.
					for (std::size_t axis = 0; axis < 3; ++axis) {
						a_normals[vertex][axis] =
							a_positions[vertex][axis] - centroid[axis];
					}
					if (!Normalize(a_normals[vertex])) {
						a_normals[vertex] = { 0.0F, 0.0F, 1.0F };
					}
					continue;
				}
				for (std::size_t axis = 0; axis < 3; ++axis) {
					orientation += a_normals[vertex][axis] *
						(a_positions[vertex][axis] - centroid[axis]);
				}
			}
			if (orientation < 0.0F) {
				for (auto& normal : a_normals) {
					for (auto& component : normal) {
						component = -component;
					}
				}
			}
			return true;
		}
	}

	PackedCarrierVertexDecodeResult DecodeCarrierVertices(
		const PackedCarrierVertexDecodeRequest& a_request)
	{
		if (a_request.vertexCount == 0 || a_request.vertexStride == 0) {
			return std::unexpected(ShellVertexGenerationError::InvalidArgument);
		}

		constexpr std::uint32_t positionSize = sizeof(float) * 3U;
		constexpr std::uint32_t normalSize = 4U;
		if (a_request.positionOffset > a_request.vertexStride ||
			positionSize > a_request.vertexStride - a_request.positionOffset) {
			return std::unexpected(ShellVertexGenerationError::InvalidLayout);
		}
		if (a_request.packedNormalOffset &&
			(*a_request.packedNormalOffset > a_request.vertexStride ||
			 normalSize > a_request.vertexStride - *a_request.packedNormalOffset ||
			 RangesOverlap(
				a_request.positionOffset, positionSize,
				*a_request.packedNormalOffset, normalSize))) {
			return std::unexpected(ShellVertexGenerationError::InvalidLayout);
		}

		const auto sourceBytes =
			static_cast<std::uint64_t>(a_request.vertexCount) * a_request.vertexStride;
		if (sourceBytes != a_request.sourceVertices.size()) {
			return std::unexpected(ShellVertexGenerationError::InvalidArgument);
		}

		std::vector<std::array<float, 3>> positions(a_request.vertexCount);
		std::vector<std::array<float, 3>> normalizedNormals(a_request.vertexCount);
		for (std::uint32_t vertex = 0; vertex < a_request.vertexCount; ++vertex) {
			const auto* source = a_request.sourceVertices.data() +
				static_cast<std::size_t>(vertex) * a_request.vertexStride;
			auto& position = positions[vertex];
			std::memcpy(
				position.data(), source + a_request.positionOffset,
				sizeof(position));
			if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
				!std::isfinite(position[2])) {
				return std::unexpected(ShellVertexGenerationError::InvalidVertex);
			}

			if (a_request.packedNormalOffset) {
				auto& normal = normalizedNormals[vertex];
				normal = DecodePackedNormal(source + *a_request.packedNormalOffset);
				if (normal[0] == 0.0F && normal[1] == 0.0F && normal[2] == 0.0F) {
					return std::unexpected(ShellVertexGenerationError::InvalidVertex);
				}
			}
		}
		if (!a_request.packedNormalOffset &&
			!ReconstructNormals(positions, a_request.sourceIndices, normalizedNormals)) {
			return std::unexpected(ShellVertexGenerationError::InvalidTopology);
		}

		std::vector<Core::CarrierVertex> result(a_request.vertexCount);
		for (std::size_t vertex = 0; vertex < result.size(); ++vertex) {
			result[vertex] = {
				{ positions[vertex][0], positions[vertex][1], positions[vertex][2] },
				{ normalizedNormals[vertex][0], normalizedNormals[vertex][1],
					normalizedNormals[vertex][2] }
			};
		}
		return result;
	}

	ShellVertexGenerationResult GenerateShellVertices(
		const ShellVertexGenerationRequest& a_request)
	{
		if (!a_request.profile || a_request.vertexCount == 0 ||
			a_request.vertexStride == 0) {
			return std::unexpected(ShellVertexGenerationError::InvalidArgument);
		}
		if (!a_request.profile->Validate().empty()) {
			return std::unexpected(ShellVertexGenerationError::InvalidProfile);
		}

		constexpr std::uint32_t positionSize = sizeof(float) * 3U;
		constexpr std::uint32_t normalSize = 4U;
		if (a_request.positionOffset > a_request.vertexStride ||
			positionSize > a_request.vertexStride - a_request.positionOffset) {
			return std::unexpected(ShellVertexGenerationError::InvalidLayout);
		}
		if (a_request.packedNormalOffset &&
			(*a_request.packedNormalOffset > a_request.vertexStride ||
			 normalSize > a_request.vertexStride - *a_request.packedNormalOffset ||
			 RangesOverlap(
				a_request.positionOffset, positionSize,
				*a_request.packedNormalOffset, normalSize))) {
			return std::unexpected(ShellVertexGenerationError::InvalidLayout);
		}

		const auto sourceBytes =
			static_cast<std::uint64_t>(a_request.vertexCount) * a_request.vertexStride;
		if (sourceBytes != a_request.sourceVertices.size()) {
			return std::unexpected(ShellVertexGenerationError::InvalidArgument);
		}

		const auto shellCount = static_cast<std::uint64_t>(a_request.profile->shellCount);
		const auto maximumOutputBytes = std::min(
			static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()),
			static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()));
		if (sourceBytes > maximumOutputBytes / shellCount) {
			return std::unexpected(ShellVertexGenerationError::ResourceTooLarge);
		}
		const auto expandedBytes = sourceBytes * shellCount;

		std::vector<Core::CarrierVertex> decodedStorage;
		std::span<const Core::CarrierVertex> decoded;
		if (!a_request.predecodedVertices.empty()) {
			if (a_request.predecodedVertices.size() != a_request.vertexCount) {
				return std::unexpected(ShellVertexGenerationError::InvalidArgument);
			}
			for (const auto& vertex : a_request.predecodedVertices) {
				if (!Core::IsFinite(vertex.position) ||
					!Core::IsFinite(vertex.extrusionNormal) ||
					Core::LengthSquared(vertex.extrusionNormal) <= 1.0e-12F) {
					return std::unexpected(ShellVertexGenerationError::InvalidVertex);
				}
			}
			decoded = a_request.predecodedVertices;
		} else {
			auto generatedDecode = DecodeCarrierVertices({
				.sourceVertices = a_request.sourceVertices,
				.vertexCount = a_request.vertexCount,
				.vertexStride = a_request.vertexStride,
				.positionOffset = a_request.positionOffset,
				.packedNormalOffset = a_request.packedNormalOffset,
				.sourceIndices = a_request.sourceIndices
			});
			if (!generatedDecode) {
				return std::unexpected(generatedDecode.error());
			}
			decodedStorage = std::move(*generatedDecode);
			decoded = decodedStorage;
		}

		std::vector<std::byte> expandedData(static_cast<std::size_t>(expandedBytes));
		for (std::uint32_t layer = 0; layer < a_request.profile->shellCount; ++layer) {
			auto* destinationLayer = expandedData.data() + sourceBytes * layer;
			std::memcpy(
				destinationLayer, a_request.sourceVertices.data(),
				static_cast<std::size_t>(sourceBytes));

			const auto sample = Core::SampleShell(
				*a_request.profile, layer, a_request.profile->shellCount);
			for (std::uint32_t vertex = 0; vertex < a_request.vertexCount; ++vertex) {
				auto* destination = destinationLayer +
					static_cast<std::size_t>(vertex) * a_request.vertexStride;
				const auto* source = a_request.sourceVertices.data() +
					static_cast<std::size_t>(vertex) * a_request.vertexStride;

				std::array<float, 3> position{};
				std::memcpy(
					position.data(), source + a_request.positionOffset,
					sizeof(position));
				const auto& normal = decoded[vertex].extrusionNormal;

				position[0] += normal.x * sample.offset;
				position[1] += normal.y * sample.offset;
				position[2] += normal.z * sample.offset;
				std::memcpy(
					destination + a_request.positionOffset,
					position.data(), sizeof(position));
			}
		}

		return expandedData;
	}
}
