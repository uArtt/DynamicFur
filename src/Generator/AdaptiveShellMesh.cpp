#include "DynamicFur/Generator/AdaptiveShellMesh.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace DynamicFur::Generator
{
	namespace
	{
		constexpr std::uint32_t kSupportedStride = 32;
		constexpr std::uint32_t kSupportedPositionOffset = 0;
		constexpr std::uint32_t kSupportedUvOffset = 16;
		constexpr std::uint32_t kSupportedSkinningOffset = 20;
		constexpr std::uint32_t kMaximumDepth = 8;
		constexpr float kNormalEpsilonSquared = 1.0e-12F;
		constexpr float kWeightEpsilon = 1.0e-8F;
		constexpr std::uint16_t kInvalidIndex =
			(std::numeric_limits<std::uint16_t>::max)();

		enum class SourceTriangleAction : std::uint8_t
		{
			Preserve,
			Hide,
			Refine
		};

		struct SourceEdge
		{
			std::uint16_t first{};
			std::uint16_t second{};

			[[nodiscard]] bool operator==(const SourceEdge&) const noexcept = default;
		};

		struct SourceEdgeHash
		{
			[[nodiscard]] std::size_t operator()(const SourceEdge& a_edge) const noexcept
			{
				return (static_cast<std::size_t>(a_edge.first) << 16U) |
					a_edge.second;
			}
		};

		struct GridEdgePoint
		{
			std::uint16_t first{};
			std::uint16_t second{};
			std::uint16_t numeratorFromFirst{};

			[[nodiscard]] bool operator==(const GridEdgePoint&) const noexcept = default;
		};

		struct GridEdgePointHash
		{
			[[nodiscard]] std::size_t operator()(const GridEdgePoint& a_point) const noexcept
			{
				std::size_t hash = a_point.first;
				hash = hash * 65599U + a_point.second;
				hash = hash * 65599U + a_point.numeratorFromFirst;
				return hash;
			}
		};

		struct UniformTriangleProbes
		{
			std::size_t triangle{};
			std::array<std::size_t, 3> edges{};
			std::size_t centroid{};
			bool cornersCovered{};
		};

		struct Blend
		{
			std::array<std::uint16_t, 3> vertices{};
			std::array<float, 3> weights{};
			std::uint32_t count{};
		};

		[[nodiscard]] bool FiniteCarrierVertex(
			const Core::CarrierVertex& a_vertex) noexcept
		{
			return Core::IsFinite(a_vertex.position) &&
				Core::IsFinite(a_vertex.extrusionNormal) &&
				Core::LengthSquared(a_vertex.extrusionNormal) > kNormalEpsilonSquared;
		}

		[[nodiscard]] SourceEdge CanonicalEdge(
			std::uint16_t a_first,
			std::uint16_t a_second) noexcept
		{
			return a_first < a_second ? SourceEdge{ a_first, a_second } :
				SourceEdge{ a_second, a_first };
		}

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

		[[nodiscard]] bool FloatToHalf(float a_value, std::uint16_t& a_result) noexcept
		{
			if (!std::isfinite(a_value)) {
				return false;
			}

			const auto bits = std::bit_cast<std::uint32_t>(a_value);
			const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
			const auto sourceExponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU);
			const auto sourceMantissa = bits & 0x007FFFFFU;
			auto exponent = sourceExponent - 127 + 15;
			if (exponent >= 31) {
				return false;
			}

			if (exponent <= 0) {
				if (exponent < -10) {
					a_result = sign;
					return true;
				}
				const auto mantissa = sourceMantissa | 0x00800000U;
				const auto shift = static_cast<std::uint32_t>(14 - exponent);
				auto rounded = mantissa >> shift;
				const auto remainderMask = (1U << shift) - 1U;
				const auto remainder = mantissa & remainderMask;
				const auto halfway = 1U << (shift - 1U);
				if (remainder > halfway ||
					(remainder == halfway && (rounded & 1U) != 0)) {
					++rounded;
				}
				a_result = static_cast<std::uint16_t>(sign | rounded);
				return true;
			}

			auto roundedMantissa = sourceMantissa >> 13U;
			const auto remainder = sourceMantissa & 0x1FFFU;
			if (remainder > 0x1000U ||
				(remainder == 0x1000U && (roundedMantissa & 1U) != 0)) {
				++roundedMantissa;
				if (roundedMantissa == 0x400U) {
					roundedMantissa = 0;
					++exponent;
					if (exponent >= 31) {
						return false;
					}
				}
			}
			a_result = static_cast<std::uint16_t>(
				sign | (static_cast<std::uint16_t>(exponent) << 10U) |
				roundedMantissa);
			return true;
		}

		[[nodiscard]] Core::CarrierVertex InterpolateCarrierVertex(
			std::span<const Core::CarrierVertex> a_vertices,
			const Blend& a_blend) noexcept
		{
			Core::CarrierVertex result{};
			std::uint32_t donor{};
			for (std::uint32_t term = 0; term < a_blend.count; ++term) {
				const auto weight = a_blend.weights[term];
				const auto& source = a_vertices[a_blend.vertices[term]];
				result.position += source.position * weight;
				result.extrusionNormal += source.extrusionNormal * weight;
				if (a_blend.weights[term] > a_blend.weights[donor] ||
					(a_blend.weights[term] == a_blend.weights[donor] &&
					 a_blend.vertices[term] < a_blend.vertices[donor])) {
					donor = term;
				}
			}
			result.extrusionNormal = Core::Normalize(result.extrusionNormal);
			if (Core::LengthSquared(result.extrusionNormal) <= kNormalEpsilonSquared) {
				result.extrusionNormal = Core::Normalize(
					a_vertices[a_blend.vertices[donor]].extrusionNormal);
			}
			return result;
		}

		[[nodiscard]] Core::AxisAlignedBounds SweptTriangleBounds(
			std::span<const Core::CarrierVertex> a_posedVertices,
			std::uint16_t a_first,
			std::uint16_t a_second,
			std::uint16_t a_third,
			float a_furLength,
			float a_underSkinDepth) noexcept
		{
			const auto infinity = std::numeric_limits<float>::infinity();
			Core::AxisAlignedBounds result{
				{ infinity, infinity, infinity },
				{ -infinity, -infinity, -infinity }
			};
			for (const auto index : { a_first, a_second, a_third }) {
				const auto& vertex = a_posedVertices[index];
				const auto normal = Core::Normalize(vertex.extrusionNormal);
				const auto inner = vertex.position - normal * a_underSkinDepth;
				const auto outer = vertex.position + normal * a_furLength;
				result.minimum = Core::Min(result.minimum, Core::Min(inner, outer));
				result.maximum = Core::Max(result.maximum, Core::Max(inner, outer));
			}
			return result;
		}

		class PackedVertexBuilder final
		{
		public:
			PackedVertexBuilder(
				const AdaptiveShellMeshRequest& a_request,
				AdaptiveShellMeshResult& a_result) noexcept :
				request_(a_request), result_(a_result)
			{}

			[[nodiscard]] std::expected<std::uint16_t, AdaptiveShellMeshError> Add(
				const Blend& a_blend)
			{
				if (a_blend.count < 2 || a_blend.count > 3 ||
					result_.referenceVertices.size() >= request_.options.maxVertices) {
					return std::unexpected(AdaptiveShellMeshError::BudgetExceeded);
				}

				float blendSum{};
				std::uint32_t donor{};
				for (std::uint32_t term = 0; term < a_blend.count; ++term) {
					if (a_blend.vertices[term] >= request_.referenceVertices.size() ||
						!std::isfinite(a_blend.weights[term]) ||
						a_blend.weights[term] < 0.0F) {
						return std::unexpected(AdaptiveShellMeshError::InvalidArgument);
					}
					blendSum += a_blend.weights[term];
					if (a_blend.weights[term] > a_blend.weights[donor] ||
						(a_blend.weights[term] == a_blend.weights[donor] &&
						 a_blend.vertices[term] < a_blend.vertices[donor])) {
						donor = term;
					}
				}
				if (std::abs(blendSum - 1.0F) > 1.0e-5F) {
					return std::unexpected(AdaptiveShellMeshError::InvalidArgument);
				}

				const auto donorIndex = a_blend.vertices[donor];
				std::array<std::byte, kSupportedStride> packed{};
				std::memcpy(
					packed.data(),
					request_.sourceVertices.data() +
						static_cast<std::size_t>(donorIndex) * kSupportedStride,
					packed.size());

				std::array<double, 3> position{};
				std::array<double, 2> uv{};
				std::array<double, 256> boneWeights{};
				for (std::uint32_t term = 0; term < a_blend.count; ++term) {
					const auto sourceIndex = a_blend.vertices[term];
					const auto blendWeight = static_cast<double>(a_blend.weights[term]);
					const auto* source = request_.sourceVertices.data() +
						static_cast<std::size_t>(sourceIndex) * kSupportedStride;

					std::array<float, 3> sourcePosition{};
					std::array<std::uint16_t, 2> sourceUv{};
					std::array<std::uint16_t, 4> sourceWeights{};
					std::array<std::uint8_t, 4> sourceBones{};
					std::memcpy(
						sourcePosition.data(), source + kSupportedPositionOffset,
						sizeof(sourcePosition));
					std::memcpy(
						sourceUv.data(), source + kSupportedUvOffset, sizeof(sourceUv));
					std::memcpy(
						sourceWeights.data(), source + kSupportedSkinningOffset,
						sizeof(sourceWeights));
					std::memcpy(
						sourceBones.data(),
						source + kSupportedSkinningOffset + sizeof(sourceWeights),
						sizeof(sourceBones));

					for (std::size_t axis = 0; axis < position.size(); ++axis) {
						position[axis] += static_cast<double>(sourcePosition[axis]) *
							blendWeight;
					}
					for (std::size_t axis = 0; axis < uv.size(); ++axis) {
						const auto component = HalfToFloat(sourceUv[axis]);
						if (!std::isfinite(component)) {
							return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
						}
						uv[axis] += static_cast<double>(component) * blendWeight;
					}
					for (std::uint32_t influence = 0;
						 influence < request_.influencesPerVertex; ++influence) {
						const auto weight = HalfToFloat(sourceWeights[influence]);
						if (!std::isfinite(weight) || weight < 0.0F) {
							return std::unexpected(AdaptiveShellMeshError::InvalidSkinning);
						}
						boneWeights[sourceBones[influence]] +=
							static_cast<double>(weight) * blendWeight;
					}
				}

				std::array<float, 3> packedPosition{};
				for (std::size_t axis = 0; axis < packedPosition.size(); ++axis) {
					packedPosition[axis] = static_cast<float>(position[axis]);
					if (!std::isfinite(packedPosition[axis])) {
						return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
					}
				}
				std::memcpy(
					packed.data() + kSupportedPositionOffset,
					packedPosition.data(), sizeof(packedPosition));
				for (std::size_t axis = 0; axis < uv.size(); ++axis) {
					std::uint16_t component{};
					if (!FloatToHalf(static_cast<float>(uv[axis]), component)) {
						return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
					}
					std::memcpy(
						packed.data() + kSupportedUvOffset + axis * sizeof(component),
						std::addressof(component), sizeof(component));
				}

				std::array<std::pair<double, std::uint8_t>, 256> sortedBones{};
				std::size_t boneCount{};
				for (std::size_t bone = 0; bone < boneWeights.size(); ++bone) {
					if (boneWeights[bone] > kWeightEpsilon) {
						sortedBones[boneCount++] = {
							boneWeights[bone], static_cast<std::uint8_t>(bone)
						};
					}
				}
				if (boneCount == 0) {
					return std::unexpected(AdaptiveShellMeshError::InvalidSkinning);
				}
				std::sort(
					sortedBones.begin(), sortedBones.begin() + boneCount,
					[](const auto& a_lhs, const auto& a_rhs) {
						return a_lhs.first != a_rhs.first ?
							a_lhs.first > a_rhs.first : a_lhs.second < a_rhs.second;
					});
				boneCount = std::min<std::size_t>(
					boneCount, request_.influencesPerVertex);
				double selectedWeight{};
				for (std::size_t influence = 0; influence < boneCount; ++influence) {
					selectedWeight += sortedBones[influence].first;
				}
				if (!std::isfinite(selectedWeight) || selectedWeight <= kWeightEpsilon) {
					return std::unexpected(AdaptiveShellMeshError::InvalidSkinning);
				}

				std::array<std::uint16_t, 4> outputWeights{};
				std::array<std::uint8_t, 4> outputBones{};
				for (std::size_t influence = 0; influence < boneCount; ++influence) {
					const auto normalized = static_cast<float>(
						sortedBones[influence].first / selectedWeight);
					if (!FloatToHalf(normalized, outputWeights[influence])) {
						return std::unexpected(AdaptiveShellMeshError::InvalidSkinning);
					}
					outputBones[influence] = sortedBones[influence].second;
				}
				std::memcpy(
					packed.data() + kSupportedSkinningOffset,
					outputWeights.data(), sizeof(outputWeights));
				std::memcpy(
					packed.data() + kSupportedSkinningOffset + sizeof(outputWeights),
					outputBones.data(), sizeof(outputBones));

				const auto reference = InterpolateCarrierVertex(
					request_.referenceVertices, a_blend);
				if (!FiniteCarrierVertex(reference)) {
					return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
				}
				const auto index = static_cast<std::uint16_t>(
					result_.referenceVertices.size());
				result_.packedVertices.insert(
					result_.packedVertices.end(), packed.begin(), packed.end());
				result_.referenceVertices.push_back(reference);
				return index;
			}

		private:
			const AdaptiveShellMeshRequest& request_;
			AdaptiveShellMeshResult& result_;
		};

		[[nodiscard]] GridEdgePoint CanonicalGridEdgePoint(
			std::uint16_t a_start,
			std::uint16_t a_end,
			std::uint32_t a_stepFromStart,
			std::uint32_t a_divisions) noexcept
		{
			if (a_start < a_end) {
				return {
					a_start, a_end, static_cast<std::uint16_t>(a_stepFromStart)
				};
			}
			return {
				a_end, a_start,
				static_cast<std::uint16_t>(a_divisions - a_stepFromStart)
			};
		}

		[[nodiscard]] std::expected<std::uint16_t, AdaptiveShellMeshError>
			GetGridEdgePoint(
				std::uint16_t a_start,
				std::uint16_t a_end,
				std::uint32_t a_stepFromStart,
				std::uint32_t a_divisions,
				std::unordered_map<GridEdgePoint, std::uint16_t, GridEdgePointHash>& a_cache,
				PackedVertexBuilder& a_builder)
		{
			if (a_stepFromStart == 0) {
				return a_start;
			}
			if (a_stepFromStart == a_divisions) {
				return a_end;
			}
			const auto key = CanonicalGridEdgePoint(
				a_start, a_end, a_stepFromStart, a_divisions);
			if (const auto found = a_cache.find(key); found != a_cache.end()) {
				return found->second;
			}
			const auto denominator = static_cast<float>(a_divisions);
			const Blend blend{
				{ key.first, key.second, 0 },
				{ 1.0F - static_cast<float>(key.numeratorFromFirst) / denominator,
					static_cast<float>(key.numeratorFromFirst) / denominator, 0.0F },
				2
			};
			auto generated = a_builder.Add(blend);
			if (!generated) {
				return std::unexpected(generated.error());
			}
			a_cache.emplace(key, *generated);
			return *generated;
		}

		[[nodiscard]] std::expected<AdaptiveShellMeshResult, AdaptiveShellMeshError>
			BuildCandidate(
				const AdaptiveShellMeshRequest& a_request,
				std::span<const SourceTriangleAction> a_actions,
				std::uint32_t a_depth,
				const AdaptiveShellMeshStats& a_baseStats)
		{
			const auto divisions = 1U << a_depth;
			std::uint64_t expectedTriangles{};
			for (const auto action : a_actions) {
				if (action == SourceTriangleAction::Preserve) {
					++expectedTriangles;
				} else if (action == SourceTriangleAction::Refine) {
					expectedTriangles += static_cast<std::uint64_t>(divisions) * divisions;
				}
			}
			if (expectedTriangles > a_request.options.maxTriangles) {
				return std::unexpected(AdaptiveShellMeshError::BudgetExceeded);
			}

			AdaptiveShellMeshResult result;
			result.packedVertices.assign(
				a_request.sourceVertices.begin(), a_request.sourceVertices.end());
			result.referenceVertices.assign(
				a_request.referenceVertices.begin(), a_request.referenceVertices.end());
			result.indices.reserve(static_cast<std::size_t>(expectedTriangles) * 3U);
			result.packedVertices.reserve(
				static_cast<std::size_t>(a_request.options.maxVertices) * kSupportedStride);
			result.referenceVertices.reserve(a_request.options.maxVertices);

			PackedVertexBuilder builder(a_request, result);
			std::unordered_map<GridEdgePoint, std::uint16_t, GridEdgePointHash> edgeCache;
			edgeCache.reserve(std::min<std::size_t>(
				a_request.options.maxVertices - a_request.referenceVertices.size(),
				static_cast<std::size_t>(a_baseStats.refinedSourceTriangleCount) *
					3U * (divisions - 1U)));

			std::uint32_t candidateTriangles{};
			for (std::size_t triangle = 0; triangle < a_actions.size(); ++triangle) {
				const auto indexOffset = triangle * 3U;
				const auto a = a_request.sourceIndices[indexOffset];
				const auto b = a_request.sourceIndices[indexOffset + 1U];
				const auto c = a_request.sourceIndices[indexOffset + 2U];
				const auto action = a_actions[triangle];
				if (action == SourceTriangleAction::Hide) {
					continue;
				}
				if (action == SourceTriangleAction::Preserve) {
					result.indices.insert(result.indices.end(), { a, b, c });
					++candidateTriangles;
					continue;
				}

				const auto row = static_cast<std::size_t>(divisions) + 1U;
				std::vector<std::uint16_t> grid(row * row, kInvalidIndex);
				const auto gridAt = [&](std::uint32_t a_i, std::uint32_t a_j)
					-> std::uint16_t& {
					return grid[static_cast<std::size_t>(a_i) * row + a_j];
				};

				for (std::uint32_t i = 0; i <= divisions; ++i) {
					for (std::uint32_t j = 0; j + i <= divisions; ++j) {
						std::expected<std::uint16_t, AdaptiveShellMeshError> generated =
							std::unexpected(AdaptiveShellMeshError::InvalidArgument);
						if (j == 0) {
							generated = GetGridEdgePoint(
								a, b, i, divisions, edgeCache, builder);
						} else if (i == 0) {
							generated = GetGridEdgePoint(
								a, c, j, divisions, edgeCache, builder);
						} else if (i + j == divisions) {
							generated = GetGridEdgePoint(
								b, c, j, divisions, edgeCache, builder);
						} else {
							const auto denominator = static_cast<float>(divisions);
							generated = builder.Add({
								{ a, b, c },
								{ static_cast<float>(divisions - i - j) / denominator,
									static_cast<float>(i) / denominator,
									static_cast<float>(j) / denominator },
								3
							});
						}
						if (!generated) {
							return std::unexpected(generated.error());
						}
						gridAt(i, j) = *generated;
					}
				}

				for (std::uint32_t i = 0; i < divisions; ++i) {
					for (std::uint32_t j = 0; i + j < divisions; ++j) {
						result.indices.insert(result.indices.end(), {
							gridAt(i, j), gridAt(i + 1U, j), gridAt(i, j + 1U)
						});
						++candidateTriangles;
						if (i + j + 1U < divisions) {
							result.indices.insert(result.indices.end(), {
								gridAt(i + 1U, j), gridAt(i + 1U, j + 1U),
								gridAt(i, j + 1U)
							});
							++candidateTriangles;
						}
					}
				}
			}

			result.stats = a_baseStats;
			result.stats.addedVertexCount = static_cast<std::uint32_t>(
				result.referenceVertices.size() - a_request.referenceVertices.size());
			result.stats.candidateTriangleCount = candidateTriangles;
			result.stats.usedDepth =
				a_baseStats.refinedSourceTriangleCount == 0 ? 0 : a_depth;
			result.stats.topologyChanged =
				a_baseStats.refinedSourceTriangleCount != 0 ||
				a_baseStats.coarseHiddenSourceTriangleCount != 0;
			if (result.indices.empty()) {
				result.indices = { 0, 0, 0 };
			}
			return result;
		}

		[[nodiscard]] std::expected<AdaptiveShellMeshStats, AdaptiveShellMeshError>
			ClassifySourceTriangles(
				const AdaptiveShellMeshRequest& a_request,
				std::span<SourceTriangleAction> a_actions)
		{
			AdaptiveShellMeshStats stats;
			const auto initialCoverage = a_request.query->Sample(a_request.posedVertices);
			if (!initialCoverage) {
				return std::unexpected(AdaptiveShellMeshError::CoverageFailure);
			}
			if (a_request.posedVertices.size() >
				(std::numeric_limits<std::uint32_t>::max)()) {
				return std::unexpected(AdaptiveShellMeshError::ResourceTooLarge);
			}
			stats.sampleCount = static_cast<std::uint32_t>(a_request.posedVertices.size());
			const auto shellCount = a_request.query->ShellCount();
			const auto covered = [&](std::uint16_t a_index) {
				return (*initialCoverage)[a_index] < shellCount;
			};

			std::vector<Core::CarrierVertex> probes;
			std::vector<UniformTriangleProbes> uniformTriangles;
			std::unordered_map<SourceEdge, std::size_t, SourceEdgeHash> edgeProbes;
			edgeProbes.reserve(a_request.sourceIndices.size());
			uniformTriangles.reserve(a_actions.size());
			const auto midpointBlend = [](const SourceEdge& a_edge) {
				return Blend{ { a_edge.first, a_edge.second, 0 },
					{ 0.5F, 0.5F, 0.0F }, 2 };
			};
			for (std::size_t triangle = 0; triangle < a_actions.size(); ++triangle) {
				const auto offset = triangle * 3U;
				const auto a = a_request.sourceIndices[offset];
				const auto b = a_request.sourceIndices[offset + 1U];
				const auto c = a_request.sourceIndices[offset + 2U];
				const auto hitCount = static_cast<std::uint32_t>(covered(a)) +
					static_cast<std::uint32_t>(covered(b)) +
					static_cast<std::uint32_t>(covered(c));
				if (hitCount != 0 && hitCount != 3) {
					a_actions[triangle] = SourceTriangleAction::Refine;
					++stats.refinedSourceTriangleCount;
					continue;
				}

				UniformTriangleProbes record;
				record.triangle = triangle;
				record.cornersCovered = hitCount == 3;
				const std::array edges{
					CanonicalEdge(a, b), CanonicalEdge(b, c), CanonicalEdge(c, a)
				};
				for (std::size_t edge = 0; edge < edges.size(); ++edge) {
					const auto [found, inserted] = edgeProbes.try_emplace(
						edges[edge], probes.size());
					if (inserted) {
						probes.push_back(InterpolateCarrierVertex(
							a_request.posedVertices, midpointBlend(edges[edge])));
					}
					record.edges[edge] = found->second;
				}
				record.centroid = probes.size();
				probes.push_back(InterpolateCarrierVertex(
					a_request.posedVertices,
					{ { a, b, c }, { 1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F }, 3 }));
				uniformTriangles.push_back(record);
			}

			if (!probes.empty()) {
				for (const auto& probe : probes) {
					if (!FiniteCarrierVertex(probe)) {
						return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
					}
				}
				const auto probeCoverage = a_request.query->Sample(probes);
				if (!probeCoverage) {
					return std::unexpected(AdaptiveShellMeshError::CoverageFailure);
				}
				if (probes.size() >
					(std::numeric_limits<std::uint32_t>::max)() - stats.sampleCount) {
					return std::unexpected(AdaptiveShellMeshError::ResourceTooLarge);
				}
				stats.sampleCount += static_cast<std::uint32_t>(probes.size());
				for (const auto& record : uniformTriangles) {
					bool anyProbeCovered = (*probeCoverage)[record.centroid] < shellCount;
					bool allProbesCovered = anyProbeCovered;
					for (const auto edge : record.edges) {
						const bool edgeCovered = (*probeCoverage)[edge] < shellCount;
						anyProbeCovered = anyProbeCovered || edgeCovered;
						allProbesCovered = allProbesCovered && edgeCovered;
					}
					if (record.cornersCovered) {
						if (!allProbesCovered) {
							a_actions[record.triangle] = SourceTriangleAction::Refine;
							++stats.refinedSourceTriangleCount;
						} else {
							a_actions[record.triangle] = SourceTriangleAction::Hide;
							++stats.coarseHiddenSourceTriangleCount;
						}
						continue;
					}

					const auto offset = record.triangle * 3U;
					const auto broadPhaseHit = a_request.query->MayIntersect(
						SweptTriangleBounds(
							a_request.posedVertices,
							a_request.sourceIndices[offset],
							a_request.sourceIndices[offset + 1U],
							a_request.sourceIndices[offset + 2U],
							a_request.query->FurLength(),
							a_request.query->UnderSkinDepth()));
					if (anyProbeCovered || broadPhaseHit) {
						a_actions[record.triangle] = SourceTriangleAction::Refine;
						++stats.refinedSourceTriangleCount;
					} else {
						a_actions[record.triangle] = SourceTriangleAction::Preserve;
					}
				}
			}
			return stats;
		}

		[[nodiscard]] std::expected<void, AdaptiveShellMeshError> ValidateRequest(
			const AdaptiveShellMeshRequest& a_request)
		{
			if (!a_request.query || a_request.referenceVertices.empty() ||
				a_request.sourceIndices.empty() ||
				(a_request.sourceIndices.size() % 3U) != 0 ||
				a_request.referenceVertices.size() != a_request.posedVertices.size() ||
				a_request.sourceVertices.size() !=
					a_request.referenceVertices.size() * kSupportedStride ||
				a_request.options.maxDepth == 0 ||
				a_request.options.maxDepth > kMaximumDepth ||
				a_request.options.maxVertices == 0 ||
				a_request.options.maxVertices > 65535 ||
				a_request.options.maxTriangles == 0 ||
				a_request.influencesPerVertex == 0 ||
				a_request.influencesPerVertex > 4) {
				return std::unexpected(AdaptiveShellMeshError::InvalidArgument);
			}
			if (a_request.vertexStride != kSupportedStride ||
				a_request.positionOffset != kSupportedPositionOffset ||
				a_request.uvOffset != kSupportedUvOffset ||
				a_request.skinningOffset != kSupportedSkinningOffset) {
				return std::unexpected(AdaptiveShellMeshError::UnsupportedLayout);
			}
			if (a_request.query->ShellCount() == 0 ||
				!std::isfinite(a_request.query->FurLength()) ||
				a_request.query->FurLength() <= 0.0F) {
				return std::unexpected(AdaptiveShellMeshError::CoverageFailure);
			}
			if (a_request.referenceVertices.size() > a_request.options.maxVertices ||
				a_request.referenceVertices.size() > 65535) {
				return std::unexpected(AdaptiveShellMeshError::BudgetExceeded);
			}

			std::vector<bool> active(a_request.referenceVertices.size());
			for (const auto index : a_request.sourceIndices) {
				if (index >= a_request.referenceVertices.size()) {
					return std::unexpected(AdaptiveShellMeshError::InvalidTopology);
				}
				active[index] = true;
			}
			for (std::size_t vertex = 0;
				 vertex < a_request.referenceVertices.size(); ++vertex) {
				if (!FiniteCarrierVertex(a_request.referenceVertices[vertex]) ||
					!FiniteCarrierVertex(a_request.posedVertices[vertex])) {
					return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
				}
				if (!active[vertex]) {
					continue;
				}

				const auto* packed = a_request.sourceVertices.data() +
					vertex * kSupportedStride;
				std::array<float, 3> position{};
				std::array<std::uint16_t, 2> uv{};
				std::array<std::uint16_t, 4> weights{};
				std::memcpy(position.data(), packed, sizeof(position));
				std::memcpy(uv.data(), packed + kSupportedUvOffset, sizeof(uv));
				std::memcpy(
					weights.data(), packed + kSupportedSkinningOffset, sizeof(weights));
				if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
					!std::isfinite(position[2]) ||
					!std::isfinite(HalfToFloat(uv[0])) ||
					!std::isfinite(HalfToFloat(uv[1]))) {
					return std::unexpected(AdaptiveShellMeshError::InvalidVertex);
				}
				float totalWeight{};
				for (std::uint32_t influence = 0;
					 influence < a_request.influencesPerVertex; ++influence) {
					const auto weight = HalfToFloat(weights[influence]);
					if (!std::isfinite(weight) || weight < 0.0F) {
						return std::unexpected(AdaptiveShellMeshError::InvalidSkinning);
					}
					totalWeight += weight;
				}
				if (!std::isfinite(totalWeight) || totalWeight <= kWeightEpsilon) {
					return std::unexpected(AdaptiveShellMeshError::InvalidSkinning);
				}
			}
			return {};
		}
	}

	AdaptiveShellMeshGenerationResult GenerateAdaptiveShellMesh(
		const AdaptiveShellMeshRequest& a_request)
	{
		try {
			const auto validation = ValidateRequest(a_request);
			if (!validation) {
				return std::unexpected(validation.error());
			}

			const auto triangleCount = a_request.sourceIndices.size() / 3U;
			if (triangleCount > (std::numeric_limits<std::uint32_t>::max)()) {
				return std::unexpected(AdaptiveShellMeshError::ResourceTooLarge);
			}
			std::vector<SourceTriangleAction> actions(
				triangleCount, SourceTriangleAction::Preserve);
			const auto stats = ClassifySourceTriangles(a_request, actions);
			if (!stats) {
				return std::unexpected(stats.error());
			}

			if (stats->refinedSourceTriangleCount == 0) {
				return BuildCandidate(a_request, actions, 1, *stats);
			}
			for (auto depth = a_request.options.maxDepth; depth >= 1; --depth) {
				auto candidate = BuildCandidate(a_request, actions, depth, *stats);
				if (candidate) {
					return candidate;
				}
				if (candidate.error() != AdaptiveShellMeshError::BudgetExceeded) {
					return std::unexpected(candidate.error());
				}
			}
			return std::unexpected(AdaptiveShellMeshError::BudgetExceeded);
		} catch (const std::bad_alloc&) {
			return std::unexpected(AdaptiveShellMeshError::ResourceTooLarge);
		} catch (...) {
			return std::unexpected(AdaptiveShellMeshError::InvalidArgument);
		}
	}
}
