#include "DynamicFur/Generator/AdaptiveShellMesh.h"
#include "DynamicFur/Generator/ShellVertexGenerator.h"
#include "DynamicFur/Generator/ShellIndexMask.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace
{
	using namespace DynamicFur;

	struct TestContext
	{
		void Check(
			bool a_condition,
			std::string_view a_description,
			const std::source_location& a_location = std::source_location::current())
		{
			if (!a_condition) {
				++failures;
				std::cerr << a_location.file_name() << ':' << a_location.line()
					<< ": check failed: " << a_description << '\n';
			}
		}

		int failures{};
	};

	constexpr std::uint32_t kVertexStride = 24;
	constexpr std::uint32_t kVertexCount = 2;
	constexpr std::uint32_t kPositionOffset = 0;
	constexpr std::uint32_t kNormalOffset = 16;

	void WritePosition(
		std::span<std::byte> a_vertices,
		std::uint32_t a_vertex,
		const std::array<float, 3>& a_position)
	{
		std::memcpy(
			a_vertices.data() + static_cast<std::size_t>(a_vertex) * kVertexStride +
				kPositionOffset,
			a_position.data(), sizeof(a_position));
	}

	[[nodiscard]] std::array<float, 3> ReadPosition(
		std::span<const std::byte> a_vertices,
		std::uint32_t a_shell,
		std::uint32_t a_vertex)
	{
		std::array<float, 3> result{};
		const auto shellBytes = static_cast<std::size_t>(kVertexStride) * kVertexCount;
		std::memcpy(
			result.data(),
			a_vertices.data() + static_cast<std::size_t>(a_shell) * shellBytes +
				static_cast<std::size_t>(a_vertex) * kVertexStride + kPositionOffset,
			sizeof(result));
		return result;
	}

	[[nodiscard]] std::vector<std::byte> MakeVertices()
	{
		std::vector<std::byte> vertices(
			static_cast<std::size_t>(kVertexStride) * kVertexCount, std::byte{ 0x5A });
		WritePosition(vertices, 0, { 1.0F, 2.0F, 3.0F });
		WritePosition(vertices, 1, { -2.0F, 4.0F, 0.5F });

		for (std::uint32_t vertex = 0; vertex < kVertexCount; ++vertex) {
			auto* normal = vertices.data() + static_cast<std::size_t>(vertex) * kVertexStride +
				kNormalOffset;
			normal[0] = std::byte{ 128 };
			normal[1] = std::byte{ 128 };
			normal[2] = std::byte{ 255 };
			normal[3] = std::byte{ 0xC3 };
		}
		return vertices;
	}

	[[nodiscard]] Generator::ShellVertexGenerationRequest MakeRequest(
		std::span<const std::byte> a_vertices,
		const Core::ShellProfile& a_profile)
	{
		return {
			.sourceVertices = a_vertices,
			.profile = std::addressof(a_profile),
			.vertexCount = kVertexCount,
			.vertexStride = kVertexStride,
			.positionOffset = kPositionOffset,
			.packedNormalOffset = kNormalOffset
		};
	}

	void TestCanonicalExpansion(TestContext& a_tests)
	{
		const auto profile = Core::ShellProfile::Legacy17();
		const auto source = MakeVertices();
		const auto generated = Generator::GenerateShellVertices(MakeRequest(source, profile));

		const auto sourceBytes = source.size();
		a_tests.Check(generated.has_value(), "valid packed vertices are expanded");
		if (!generated) {
			return;
		}
		a_tests.Check(
			generated->size() == sourceBytes * profile.shellCount,
			"the output contains every canonical shell");

		const auto base = ReadPosition(*generated, 0, 0);
		a_tests.Check(
			base[0] == 1.0F && base[1] == 2.0F && base[2] == 3.0F,
			"shell zero preserves the source position");

		const auto outer = ReadPosition(*generated, profile.shellCount - 1, 0);
		const auto displacement = std::sqrt(
			(outer[0] - base[0]) * (outer[0] - base[0]) +
			(outer[1] - base[1]) * (outer[1] - base[1]) +
			(outer[2] - base[2]) * (outer[2] - base[2]));
		a_tests.Check(
			std::abs(displacement - profile.furLength) < 1.0e-5F,
			"the outer shell reaches the configured fur length");
		a_tests.Check(outer[2] > base[2], "packed +Z normals extrude outward");

		for (std::uint32_t shell = 0; shell < profile.shellCount; ++shell) {
			const auto layerOffset = static_cast<std::size_t>(shell) * sourceBytes;
			for (std::uint32_t vertex = 0; vertex < kVertexCount; ++vertex) {
				const auto vertexOffset = static_cast<std::size_t>(vertex) * kVertexStride;
				for (std::uint32_t byte = 12; byte < kVertexStride; ++byte) {
					a_tests.Check(
						(*generated)[layerOffset + vertexOffset + byte] ==
							source[vertexOffset + byte],
						"non-position vertex attributes are preserved byte-for-byte");
				}
			}
		}
	}

	void TestValidation(TestContext& a_tests)
	{
		auto profile = Core::ShellProfile::Legacy17();
		auto vertices = MakeVertices();

		auto request = MakeRequest(vertices, profile);
		request.sourceVertices = std::span<const std::byte>(vertices).first(vertices.size() - 1);
		const auto wrongSize = Generator::GenerateShellVertices(request);
		a_tests.Check(
			!wrongSize && wrongSize.error() == Generator::ShellVertexGenerationError::InvalidArgument,
			"a mismatched source byte count is rejected");

		request = MakeRequest(vertices, profile);
		request.positionOffset = 8;
		const auto overlap = Generator::GenerateShellVertices(request);
		a_tests.Check(
			!overlap && overlap.error() == Generator::ShellVertexGenerationError::InvalidLayout,
			"overlapping position and normal fields are rejected");

		auto invalidProfile = profile;
		invalidProfile.shellCount = 0;
		const auto badProfile = Generator::GenerateShellVertices(
			MakeRequest(vertices, invalidProfile));
		a_tests.Check(
			!badProfile && badProfile.error() == Generator::ShellVertexGenerationError::InvalidProfile,
			"an invalid shell profile is rejected");

		const auto nan = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(vertices.data(), std::addressof(nan), sizeof(nan));
		const auto badVertex = Generator::GenerateShellVertices(MakeRequest(vertices, profile));
		a_tests.Check(
			!badVertex && badVertex.error() == Generator::ShellVertexGenerationError::InvalidVertex,
			"non-finite source positions are rejected");
	}

	void TestTopologyNormalReconstruction(TestContext& a_tests)
	{
		auto profile = Core::ShellProfile::Legacy17();
		profile.furLength = 1.0F;

		constexpr std::uint32_t stride = 16;
		constexpr std::uint32_t count = 3;
		std::vector<std::byte> vertices(stride * count);
		const std::array<std::array<float, 3>, count> positions{
			std::array{ 0.0F, 0.0F, 0.0F },
			std::array{ 1.0F, 0.0F, 0.0F },
			std::array{ 0.0F, 1.0F, 0.0F }
		};
		for (std::size_t vertex = 0; vertex < positions.size(); ++vertex) {
			std::memcpy(vertices.data() + vertex * stride, positions[vertex].data(), 12);
		}
		const std::array<std::uint16_t, 3> indices{ 0, 1, 2 };
		const auto generated = Generator::GenerateShellVertices({
			.sourceVertices = vertices,
			.profile = std::addressof(profile),
			.vertexCount = count,
			.vertexStride = stride,
			.positionOffset = 0,
			.packedNormalOffset = std::nullopt,
			.sourceIndices = indices
		});
		a_tests.Check(generated.has_value(), "topology reconstructs missing normals");
		if (!generated) {
			return;
		}
		std::array<float, 3> outer{};
		std::memcpy(
			outer.data(),
			generated->data() +
				static_cast<std::size_t>(stride) * count * (profile.shellCount - 1),
			sizeof(outer));
		a_tests.Check(
			std::abs(outer[0]) < 1.0e-6F && std::abs(outer[1]) < 1.0e-6F &&
			std::abs(std::abs(outer[2]) - 1.0F) < 1.0e-6F,
			"reconstructed normal extrudes perpendicular to the triangle");

		const std::array<std::uint16_t, 3> invalidIndices{ 0, 1, 3 };
		const auto invalid = Generator::GenerateShellVertices({
			.sourceVertices = vertices,
			.profile = std::addressof(profile),
			.vertexCount = count,
			.vertexStride = stride,
			.positionOffset = 0,
			.packedNormalOffset = std::nullopt,
			.sourceIndices = invalidIndices
		});
		a_tests.Check(
			!invalid && invalid.error() == Generator::ShellVertexGenerationError::InvalidTopology,
			"out-of-range topology is rejected");
	}

	void TestBinaryArmorIndexMask(TestContext& a_tests)
	{
		const std::array<std::uint16_t, 6> indices{ 0, 1, 2, 2, 1, 3 };
		const std::array<Core::FirstHiddenShell, 4> coverage{ 17, 17, 5, 17 };
		const auto masked = Generator::GenerateBinaryShellIndexMask({
			.sourceIndices = indices,
			.coverage = coverage,
			.vertexCount = 4,
			.shellCount = 17
		});
		a_tests.Check(masked.has_value(), "valid armor coverage produces an index mask");
		if (masked) {
			a_tests.Check(masked->hiddenTriangleCount == 2,
				"a covered boundary vertex conservatively removes adjacent triangles");
			a_tests.Check(masked->indices == std::vector<std::uint16_t>{ 0, 0, 0, 2, 2, 2 },
				"covered triangles retain count while becoming degenerates");
		}

		const std::array<Core::FirstHiddenShell, 4> visible{ 17, 17, 17, 17 };
		const auto unchanged = Generator::GenerateBinaryShellIndexMask({
			.sourceIndices = indices,
			.coverage = visible,
			.vertexCount = 4,
			.shellCount = 17
		});
		a_tests.Check(unchanged && !unchanged->HasCoverage() &&
			unchanged->indices == std::vector<std::uint16_t>(indices.begin(), indices.end()),
			"fully visible coverage preserves topology byte-for-byte");

		const std::array<std::uint16_t, 3> invalid{ 0, 1, 4 };
		const auto rejected = Generator::GenerateBinaryShellIndexMask({
			.sourceIndices = invalid,
			.coverage = visible,
			.vertexCount = 4,
			.shellCount = 17
		});
		a_tests.Check(!rejected && rejected.error() == Generator::ShellIndexMaskError::InvalidIndex,
			"out-of-range mask topology is rejected");
	}

	void TestCompactArmorIndexMask(TestContext& a_tests)
	{
		const std::array<std::uint16_t, 6> indices{ 0, 1, 2, 2, 1, 3 };
		const std::array<Core::FirstHiddenShell, 4> visible{ 17, 17, 17, 17 };
		const std::array<Core::FirstHiddenShell, 2> centroidCoverage{ 0, 17 };
		const auto compacted = Generator::GenerateCompactShellIndexMask({
			.sourceIndices = indices,
			.vertexCoverage = visible,
			.triangleCoverage = centroidCoverage,
			.vertexCount = 4,
			.shellCount = 17
		});
		a_tests.Check(compacted.has_value(),
			"valid refined coverage compacts the shell index stream");
		if (compacted) {
			a_tests.Check(
				compacted->indices == std::vector<std::uint16_t>{ 2, 1, 3 },
				"a centroid-only armor hit removes the containing micro-triangle");
			a_tests.Check(
				compacted->hiddenTriangleCount == 1 &&
					compacted->visibleTriangleCount == 1 &&
					!compacted->emittedSafetyDegenerate,
				"compaction reports visible and hidden triangle counts");
		}

		const std::array<Core::FirstHiddenShell, 4> covered{ 0, 0, 0, 0 };
		const auto fullyCovered = Generator::GenerateCompactShellIndexMask({
			.sourceIndices = indices,
			.vertexCoverage = covered,
			.vertexCount = 4,
			.shellCount = 17
		});
		a_tests.Check(
			fullyCovered && fullyCovered->indices ==
				std::vector<std::uint16_t>{ 0, 0, 0 } &&
				fullyCovered->emittedSafetyDegenerate,
			"fully covered refined topology emits one safe D3D11 degenerate");

		const std::array<Core::FirstHiddenShell, 1> wrongCentroidCount{ 17 };
		const auto invalid = Generator::GenerateCompactShellIndexMask({
			.sourceIndices = indices,
			.vertexCoverage = visible,
			.triangleCoverage = wrongCentroidCount,
			.vertexCount = 4,
			.shellCount = 17
		});
		a_tests.Check(
			!invalid && invalid.error() == Generator::ShellIndexMaskError::InvalidArgument,
			"centroid coverage must match the refined triangle count");
	}

	constexpr std::uint32_t kAdaptiveStride = 32;

	[[nodiscard]] float TestHalfToFloat(std::uint16_t a_half) noexcept
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
		} else {
			exponent += 127U - 15U;
			bits = sign | (exponent << 23U) | (mantissa << 13U);
		}
		return std::bit_cast<float>(bits);
	}

	struct AdaptiveFixture
	{
		std::vector<std::byte> packed;
		std::vector<Core::CarrierVertex> reference;
		std::vector<Core::CarrierVertex> posed;
	};

	[[nodiscard]] AdaptiveFixture MakeAdaptiveFixture(
		std::span<const std::array<float, 3>> a_positions)
	{
		AdaptiveFixture result;
		result.packed.resize(a_positions.size() * kAdaptiveStride);
		result.reference.reserve(a_positions.size());
		for (std::size_t vertex = 0; vertex < a_positions.size(); ++vertex) {
			auto* destination = result.packed.data() + vertex * kAdaptiveStride;
			std::memcpy(destination, a_positions[vertex].data(), sizeof(a_positions[vertex]));
			const auto donorW = static_cast<std::uint32_t>(0xABC00000U + vertex);
			std::memcpy(destination + 12, std::addressof(donorW), sizeof(donorW));
			const std::array<std::uint16_t, 2> uv{
				vertex == 1 ? std::uint16_t{ 0x3C00 } : std::uint16_t{},
				vertex == 2 ? std::uint16_t{ 0x3C00 } : std::uint16_t{}
			};
			const std::array<std::uint16_t, 4> weights{
				0x3C00, 0, 0, 0
			};
			const std::array<std::uint8_t, 4> bones{
				static_cast<std::uint8_t>(vertex), 0, 0, 0
			};
			std::memcpy(destination + 16, uv.data(), sizeof(uv));
			std::memcpy(destination + 20, weights.data(), sizeof(weights));
			std::memcpy(destination + 28, bones.data(), sizeof(bones));
			result.reference.push_back({
				{ a_positions[vertex][0], a_positions[vertex][1], a_positions[vertex][2] },
				{ 0.0F, 0.0F, 1.0F }
			});
		}
		result.posed = result.reference;
		return result;
	}

	[[nodiscard]] Core::Triangle HorizontalPatch(
		float a_x,
		float a_y,
		float a_radius,
		float a_z = 0.5F) noexcept
	{
		return {
			{ a_x - a_radius, a_y - a_radius, a_z },
			{ a_x + a_radius, a_y - a_radius, a_z },
			{ a_x, a_y + a_radius, a_z }
		};
	}

	[[nodiscard]] Generator::AdaptiveShellMeshRequest MakeAdaptiveRequest(
		const AdaptiveFixture& a_fixture,
		std::span<const std::uint16_t> a_indices,
		const Core::ArmorCoverageQuery& a_query,
		Generator::AdaptiveShellMeshOptions a_options = {})
	{
		return {
			.sourceVertices = a_fixture.packed,
			.sourceIndices = a_indices,
			.referenceVertices = a_fixture.reference,
			.posedVertices = a_fixture.posed,
			.query = std::addressof(a_query),
			.vertexStride = kAdaptiveStride,
			.positionOffset = 0,
			.uvOffset = 16,
			.skinningOffset = 20,
			.influencesPerVertex = 4,
			.options = a_options
		};
	}

	[[nodiscard]] std::size_t FindAdaptiveVertex(
		std::span<const std::byte> a_packed,
		float a_x,
		float a_y)
	{
		for (std::size_t vertex = 0; vertex < a_packed.size() / kAdaptiveStride; ++vertex) {
			std::array<float, 3> position{};
			std::memcpy(
				position.data(), a_packed.data() + vertex * kAdaptiveStride,
				sizeof(position));
			if (std::abs(position[0] - a_x) < 1.0e-6F &&
				std::abs(position[1] - a_y) < 1.0e-6F) {
				return vertex;
			}
		}
		return (std::numeric_limits<std::size_t>::max)();
	}

	void TestAdaptiveCenterProbeAndInterpolation(TestContext& a_tests)
	{
		const std::array positions{
			std::array{ 0.0F, 0.0F, 0.0F },
			std::array{ 1.0F, 0.0F, 0.0F },
			std::array{ 0.0F, 1.0F, 0.0F }
		};
		auto fixture = MakeAdaptiveFixture(positions);
		const std::array<std::uint16_t, 4> firstWeights{
			0x3800, 0x3400, 0x3000, 0x3000
		};
		const std::array<std::uint8_t, 4> firstBones{ 1, 2, 3, 4 };
		const std::array<std::uint16_t, 4> secondWeights{
			0x3400, 0x3800, 0x3000, 0x3000
		};
		const std::array<std::uint8_t, 4> secondBones{ 1, 5, 6, 7 };
		std::memcpy(fixture.packed.data() + 20, firstWeights.data(), sizeof(firstWeights));
		std::memcpy(fixture.packed.data() + 28, firstBones.data(), sizeof(firstBones));
		std::memcpy(
			fixture.packed.data() + kAdaptiveStride + 20,
			secondWeights.data(), sizeof(secondWeights));
		std::memcpy(
			fixture.packed.data() + kAdaptiveStride + 28,
			secondBones.data(), sizeof(secondBones));

		auto profile = Core::ShellProfile::Legacy17();
		profile.furLength = 1.0F;
		const std::array armor{ HorizontalPatch(1.0F / 3.0F, 1.0F / 3.0F, 0.08F) };
		const auto query = Core::ArmorCoverageQuery::Create(profile, armor);
		a_tests.Check(query.has_value(), "adaptive center-probe query builds");
		if (!query) {
			return;
		}
		const std::array<std::uint16_t, 3> indices{ 0, 1, 2 };
		auto request = MakeAdaptiveRequest(
			fixture, indices, *query,
			{ .maxDepth = 1, .maxVertices = 64, .maxTriangles = 16 });
		const auto generated = Generator::GenerateAdaptiveShellMesh(request);
		a_tests.Check(generated.has_value(), "center-only armor hit refines a coarse triangle");
		if (!generated) {
			return;
		}
		a_tests.Check(generated->stats.refinedSourceTriangleCount == 1,
			"center probe detects armor despite three visible corners");
		a_tests.Check(generated->stats.sampleCount == 7,
			"one triangle samples vertices, shared edge midpoints and centroid");
		a_tests.Check(generated->stats.candidateTriangleCount == 4 &&
			generated->stats.addedVertexCount == 3,
			"depth-one triangular grid contains four candidates and three midpoints");
		a_tests.Check(std::equal(
			fixture.packed.begin(), fixture.packed.end(), generated->packedVertices.begin()),
			"adaptive output preserves every original packed byte");

		const auto midpoint = FindAdaptiveVertex(generated->packedVertices, 0.5F, 0.0F);
		a_tests.Check(midpoint != (std::numeric_limits<std::size_t>::max)(),
			"AB edge midpoint exists in the refined stream");
		if (midpoint == (std::numeric_limits<std::size_t>::max)()) {
			return;
		}
		const auto* packed = generated->packedVertices.data() + midpoint * kAdaptiveStride;
		std::uint32_t donorW{};
		std::array<std::uint16_t, 2> uv{};
		std::array<std::uint16_t, 4> weights{};
		std::array<std::uint8_t, 4> bones{};
		std::memcpy(std::addressof(donorW), packed + 12, sizeof(donorW));
		std::memcpy(uv.data(), packed + 16, sizeof(uv));
		std::memcpy(weights.data(), packed + 20, sizeof(weights));
		std::memcpy(bones.data(), packed + 28, sizeof(bones));
		a_tests.Check(donorW == 0xABC00000U,
			"equal barycentric donor ties select the lower source index");
		a_tests.Check(std::abs(TestHalfToFloat(uv[0]) - 0.5F) < 1.0e-6F &&
			std::abs(TestHalfToFloat(uv[1])) < 1.0e-6F,
			"midpoint UV is interpolated in float before half packing");
		a_tests.Check(bones == std::array<std::uint8_t, 4>{ 1, 5, 2, 3 },
			"duplicate global bones aggregate before deterministic top-four selection");
		float weightSum{};
		for (const auto weight : weights) {
			weightSum += TestHalfToFloat(weight);
		}
		a_tests.Check(std::abs(weightSum - 1.0F) < 0.001F,
			"retained top-four skin weights are normalized after half packing");
	}

	void TestAdaptiveVisibleGapWithCoveredCorners(TestContext& a_tests)
	{
		const std::array positions{
			std::array{ 0.0F, 0.0F, 0.0F },
			std::array{ 2.0F, 0.0F, 0.0F },
			std::array{ 0.0F, 2.0F, 0.0F }
		};
		const auto fixture = MakeAdaptiveFixture(positions);
		const std::array<std::uint16_t, 3> indices{ 0, 1, 2 };
		auto profile = Core::ShellProfile::Legacy17();
		profile.furLength = 1.0F;
		const std::array armor{
			HorizontalPatch(0.0F, 0.0F, 0.04F),
			HorizontalPatch(2.0F, 0.0F, 0.04F),
			HorizontalPatch(0.0F, 2.0F, 0.04F)
		};
		const auto query = Core::ArmorCoverageQuery::Create(profile, armor);
		a_tests.Check(query.has_value(), "covered-corner gap query builds");
		if (!query) {
			return;
		}

		const auto generated = Generator::GenerateAdaptiveShellMesh(
			MakeAdaptiveRequest(
				fixture, indices, *query,
				{ .maxDepth = 1, .maxVertices = 64, .maxTriangles = 16 }));
		a_tests.Check(generated.has_value(),
			"a visible interior gap with three covered corners refines safely");
		if (!generated) {
			return;
		}
		a_tests.Check(
			generated->stats.refinedSourceTriangleCount == 1 &&
				generated->stats.coarseHiddenSourceTriangleCount == 0 &&
				generated->stats.candidateTriangleCount == 4,
			"free midpoint and centroid probes preserve a geometric opening");
	}

	void TestAdaptiveUnderSkinFeatureBetweenSamples(TestContext& a_tests)
	{
		const std::array positions{
			std::array{ 0.0F, 0.0F, 0.0F },
			std::array{ 2.0F, 0.0F, 0.0F },
			std::array{ 0.0F, 2.0F, 0.0F }
		};
		const auto fixture = MakeAdaptiveFixture(positions);
		const std::array<std::uint16_t, 3> indices{ 0, 1, 2 };
		auto profile = Core::ShellProfile::Legacy17();
		profile.furLength = 1.0F;
		profile.armorUnderSkinDepth = 0.5F;
		// This tiny patch misses all corners, edge midpoints and the centroid. The
		// conservative swept bounds must still request local refinement behind skin.
		const std::array armor{ HorizontalPatch(0.2F, 0.2F, 0.01F, -0.2F) };
		const auto query = Core::ArmorCoverageQuery::Create(profile, armor);
		a_tests.Check(query.has_value(), "under-skin broad-phase query builds");
		if (!query) {
			return;
		}

		const auto generated = Generator::GenerateAdaptiveShellMesh(
			MakeAdaptiveRequest(
				fixture, indices, *query,
				{ .maxDepth = 1, .maxVertices = 64, .maxTriangles = 16 }));
		a_tests.Check(generated && generated->stats.refinedSourceTriangleCount == 1 &&
			generated->stats.candidateTriangleCount == 4,
			"a small under-skin armor feature between samples refines the source triangle");
	}

	void TestAdaptiveSharedEdgesBudgetsAndDeterminism(TestContext& a_tests)
	{
		const std::array positions{
			std::array{ 0.0F, 0.0F, 0.0F },
			std::array{ 1.0F, 0.0F, 0.0F },
			std::array{ 0.0F, 1.0F, 0.0F },
			std::array{ 1.0F, 1.0F, 0.0F }
		};
		const auto fixture = MakeAdaptiveFixture(positions);
		const std::array<std::uint16_t, 6> indices{ 0, 1, 2, 2, 1, 3 };
		auto profile = Core::ShellProfile::Legacy17();
		profile.furLength = 1.0F;
		const std::array armor{
			HorizontalPatch(0.0F, 0.0F, 0.04F),
			HorizontalPatch(1.0F, 1.0F, 0.04F)
		};
		const auto query = Core::ArmorCoverageQuery::Create(profile, armor);
		a_tests.Check(query.has_value(), "shared-edge adaptive query builds");
		if (!query) {
			return;
		}

		const auto request = MakeAdaptiveRequest(
			fixture, indices, *query,
			{ .maxDepth = 1, .maxVertices = 64, .maxTriangles = 16 });
		const auto first = Generator::GenerateAdaptiveShellMesh(request);
		const auto second = Generator::GenerateAdaptiveShellMesh(request);
		a_tests.Check(first.has_value() && second.has_value(),
			"two mixed adjacent triangles refine successfully");
		if (!first || !second) {
			return;
		}
		a_tests.Check(first->stats.refinedSourceTriangleCount == 2 &&
			first->stats.addedVertexCount == 5 &&
			first->stats.candidateTriangleCount == 8,
			"adjacent grids share their diagonal midpoint globally");
		a_tests.Check(first->packedVertices == second->packedVertices &&
			first->indices == second->indices,
			"adaptive packed topology is deterministic across runs");

		const std::array oneTriangle{ indices[0], indices[1], indices[2] };
		const std::array centerArmor{
			HorizontalPatch(1.0F / 3.0F, 1.0F / 3.0F, 0.08F)
		};
		const auto centerQuery = Core::ArmorCoverageQuery::Create(profile, centerArmor);
		if (!centerQuery) {
			a_tests.Check(false, "budget query builds");
			return;
		}
		const auto triangleFixture = MakeAdaptiveFixture(
			std::span<const std::array<float, 3>>(positions).first(3));
		const auto reduced = Generator::GenerateAdaptiveShellMesh(MakeAdaptiveRequest(
			triangleFixture, oneTriangle, *centerQuery,
			{ .maxDepth = 3, .maxVertices = 128, .maxTriangles = 16 }));
		a_tests.Check(reduced && reduced->stats.usedDepth == 2 &&
			reduced->stats.candidateTriangleCount == 16,
			"triangle budget retries the same deterministic mesh at a lower depth");

		const auto exhausted = Generator::GenerateAdaptiveShellMesh(MakeAdaptiveRequest(
			triangleFixture, oneTriangle, *centerQuery,
			{ .maxDepth = 3, .maxVertices = 128, .maxTriangles = 3 }));
		a_tests.Check(!exhausted &&
			exhausted.error() == Generator::AdaptiveShellMeshError::BudgetExceeded,
			"a budget smaller than depth one reports a typed budget error");

		auto unsupportedRequest = MakeAdaptiveRequest(
			triangleFixture, oneTriangle, *centerQuery);
		unsupportedRequest.vertexStride = 31;
		const auto unsupported = Generator::GenerateAdaptiveShellMesh(unsupportedRequest);
		a_tests.Check(!unsupported &&
			unsupported.error() == Generator::AdaptiveShellMeshError::UnsupportedLayout,
			"non-3BA packed layouts are explicitly rejected");
	}
}

int main()
{
	TestContext tests;
	TestCanonicalExpansion(tests);
	TestValidation(tests);
	TestTopologyNormalReconstruction(tests);
	TestBinaryArmorIndexMask(tests);
	TestCompactArmorIndexMask(tests);
	TestAdaptiveCenterProbeAndInterpolation(tests);
	TestAdaptiveVisibleGapWithCoveredCorners(tests);
	TestAdaptiveUnderSkinFeatureBetweenSamples(tests);
	TestAdaptiveSharedEdgesBudgetsAndDeterminism(tests);

	if (tests.failures != 0) {
		std::cerr << tests.failures << " generator test(s) failed\n";
		return 1;
	}
	std::cout << "All DynamicFur generator tests passed\n";
	return 0;
}
