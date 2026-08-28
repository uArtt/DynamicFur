#include "PCH.h"

#include "DynamicFur/Game/SkinnedPose.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace DynamicFur::Game
{
	namespace
	{
		constexpr std::uint32_t kMaximumSaneBoneCount = 512;

		[[nodiscard]] float HalfToFloat(std::uint16_t a_half) noexcept
		{
			const auto sign = static_cast<std::uint32_t>(a_half & 0x8000U) << 16U;
			auto exponent = static_cast<std::uint32_t>((a_half >> 10U) & 0x1FU);
			auto mantissa = static_cast<std::uint32_t>(a_half & 0x03FFU);
			std::uint32_t bits{};
			if (exponent == 0) {
				if (mantissa == 0) {
					bits = sign;
				} else {
					std::int32_t adjustedExponent = -14;
					while ((mantissa & 0x0400U) == 0) {
						mantissa <<= 1U;
						--adjustedExponent;
					}
					mantissa &= 0x03FFU;
					bits = sign |
						(static_cast<std::uint32_t>(adjustedExponent + 127) << 23U) |
						(mantissa << 13U);
				}
			} else if (exponent == 0x1FU) {
				bits = sign | 0x7F800000U | (mantissa << 13U);
			} else {
				exponent += 127U - 15U;
				bits = sign | (exponent << 23U) | (mantissa << 13U);
			}
			return std::bit_cast<float>(bits);
		}

		[[nodiscard]] bool IsFinite(const RE::NiTransform& a_transform) noexcept
		{
			if (!std::isfinite(a_transform.scale) ||
				std::abs(a_transform.scale) <= 1.0e-8F ||
				!std::isfinite(a_transform.translate.x) ||
				!std::isfinite(a_transform.translate.y) ||
				!std::isfinite(a_transform.translate.z)) {
				return false;
			}
			for (const auto& row : a_transform.rotate.entry) {
				for (const auto value : row) {
					if (!std::isfinite(value)) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] Core::Vec3 TransformPoint(
			const RE::NiTransform& a_transform,
			const Core::Vec3& a_point) noexcept
		{
			const auto transformed = a_transform * RE::NiPoint3{
				a_point.x, a_point.y, a_point.z
			};
			return { transformed.x, transformed.y, transformed.z };
		}
	}

	std::expected<std::vector<Core::CarrierVertex>, std::string> PoseVertices(
		const PosedVertexRequest& a_request)
	{
		if (!a_request.geometry || !a_request.skeletonRoot ||
			a_request.referenceVertices.empty() || a_request.vertexStride == 0 ||
			a_request.packedVertices.size() !=
				a_request.referenceVertices.size() * a_request.vertexStride) {
			return std::unexpected("invalid posed-vertex request");
		}
		for (const auto index : a_request.sourceIndices) {
			if (index >= a_request.referenceVertices.size()) {
				return std::unexpected("posed-vertex topology is out of range");
			}
		}

		const auto rootInverse = a_request.skeletonRoot->world.Invert();
		if (!IsFinite(rootInverse)) {
			return std::unexpected("skeleton root transform is not invertible");
		}

		std::vector<bool> active(a_request.referenceVertices.size());
		for (const auto index : a_request.sourceIndices) {
			active[index] = true;
		}
		std::vector<Core::CarrierVertex> result(
			a_request.referenceVertices.begin(), a_request.referenceVertices.end());

		auto* skin = a_request.geometry->GetGeometryRuntimeData().skinInstance.get();
		if (!skin) {
			const auto objectToRoot = rootInverse * a_request.geometry->world;
			if (!IsFinite(objectToRoot)) {
				return std::unexpected("rigid armor transform is invalid");
			}
			for (std::size_t index = 0; index < result.size(); ++index) {
				const auto end = a_request.referenceVertices[index].position +
					a_request.referenceVertices[index].extrusionNormal;
				result[index].position = TransformPoint(
					objectToRoot, a_request.referenceVertices[index].position);
				result[index].extrusionNormal = Core::Normalize(
					TransformPoint(objectToRoot, end) - result[index].position);
			}
			return result;
		}

		if (!a_request.partition || !skin->skinData || !skin->bones ||
			a_request.skinningOffset > a_request.vertexStride ||
			12U > a_request.vertexStride - a_request.skinningOffset) {
			return std::unexpected("skinned pose metadata is incomplete");
		}
		const auto boneCount = skin->skinData->GetBoneCount();
		const auto bonesPerVertex = a_request.partition->bonesPerVertex;
		if (boneCount == 0 || boneCount > kMaximumSaneBoneCount ||
			bonesPerVertex == 0 || bonesPerVertex > 4) {
			return std::unexpected("skinned pose has invalid bone metadata");
		}
		const auto weightsPerVertex = static_cast<std::uint32_t>(bonesPerVertex);

		std::vector<RE::NiTransform> boneTransforms(boneCount);
		for (std::uint32_t bone = 0; bone < boneCount; ++bone) {
			if (!skin->bones[bone]) {
				return std::unexpected("skinned pose references a missing bone");
			}
			boneTransforms[bone] = rootInverse * skin->bones[bone]->world *
				skin->skinData->GetBoneDataSkinToBone(bone);
			if (!IsFinite(boneTransforms[bone])) {
				return std::unexpected("skinned pose produced an invalid bone transform");
			}
		}

		for (std::size_t vertex = 0; vertex < result.size(); ++vertex) {
			if (!active[vertex]) {
				continue;
			}
			const auto* skinBytes = a_request.packedVertices.data() +
				vertex * a_request.vertexStride + a_request.skinningOffset;
			std::array<std::uint16_t, 4> packedWeights{};
			std::array<std::uint8_t, 4> localBones{};
			std::memcpy(packedWeights.data(), skinBytes, sizeof(packedWeights));
			std::memcpy(localBones.data(), skinBytes + sizeof(packedWeights),
				sizeof(localBones));

			Core::Vec3 posedPosition{};
			Core::Vec3 posedNormalEnd{};
			float totalWeight{};
			const auto normalEnd = a_request.referenceVertices[vertex].position +
				a_request.referenceVertices[vertex].extrusionNormal;
			for (std::uint32_t influence = 0; influence < weightsPerVertex; ++influence) {
				const auto weight = HalfToFloat(packedWeights[influence]);
				if (!std::isfinite(weight) || weight < 0.0F) {
					return std::unexpected("skinned pose contains an invalid weight");
				}
				if (weight <= 1.0e-7F) {
					continue;
				}

				// Skyrim SE stream 100 stores these four indices in the global
				// NiSkinInstance::bones space. NiSkinPartition::bonePalette belongs
				// to the legacy partition representation and must not remap the live
				// GPU stream a second time.
				const std::uint32_t globalBone = localBones[influence];
				if (globalBone >= boneTransforms.size()) {
					return std::unexpected("skinned pose bone index is out of range");
				}

				posedPosition += TransformPoint(
					boneTransforms[globalBone],
					a_request.referenceVertices[vertex].position) * weight;
				posedNormalEnd += TransformPoint(
					boneTransforms[globalBone], normalEnd) * weight;
				totalWeight += weight;
			}
			if (!std::isfinite(totalWeight) || totalWeight <= 1.0e-6F) {
				return std::unexpected("skinned pose active vertex has zero total weight");
			}

			// Match Skyrim's GPU skinning exactly: packed weights are summed as-is.
			// Renormalizing here would move the CPU coverage surface away from the
			// body and armor actually rendered by the game.
			result[vertex].position = posedPosition;
			result[vertex].extrusionNormal = Core::Normalize(
				posedNormalEnd - posedPosition);
			if (!Core::IsFinite(result[vertex].position) ||
				!Core::IsFinite(result[vertex].extrusionNormal) ||
				Core::LengthSquared(result[vertex].extrusionNormal) <= 1.0e-12F) {
				return std::unexpected("skinned pose produced a degenerate vertex");
			}
		}
		return result;
	}
}
