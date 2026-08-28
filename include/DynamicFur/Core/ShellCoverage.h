#pragma once

#include "DynamicFur/Core/MathTypes.h"
#include "DynamicFur/Core/ShellMath.h"
#include "DynamicFur/Core/ShellProfile.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace DynamicFur::Core
{
	using CoverageSourceID = std::uint64_t;
	using FirstHiddenShell = std::uint8_t;
	inline constexpr FirstHiddenShell kFullyVisibleCoverage =
		static_cast<FirstHiddenShell>(kMaximumShellCount);

	struct Triangle
	{
		Vec3 a;
		Vec3 b;
		Vec3 c;
	};

	struct CarrierVertex
	{
		Vec3 position;
		Vec3 extrusionNormal;
	};

	struct AxisAlignedBounds
	{
		Vec3 minimum;
		Vec3 maximum;
	};

	class CoverageField final
	{
	public:
		CoverageField() = default;
		CoverageField(std::size_t a_vertexCount, FirstHiddenShell a_neutralValue);

		[[nodiscard]] std::size_t Size() const noexcept;
		[[nodiscard]] bool Empty() const noexcept;
		[[nodiscard]] FirstHiddenShell operator[](std::size_t a_index) const noexcept;
		[[nodiscard]] std::span<const FirstHiddenShell> Values() const noexcept;
		[[nodiscard]] std::span<FirstHiddenShell> Values() noexcept;

	private:
		std::vector<FirstHiddenShell> values_;
	};

	class CoverageSet final
	{
	public:
		// Mutations and Aggregate() access belong to one owner thread. A reference
		// returned by Aggregate() is invalidated by the next successful mutation.
		explicit CoverageSet(
			std::size_t a_vertexCount = 0,
			FirstHiddenShell a_neutralValue = kFullyVisibleCoverage);

		[[nodiscard]] bool Set(CoverageSourceID a_source, CoverageField a_field);
		[[nodiscard]] bool Remove(CoverageSourceID a_source);
		void Clear();

		[[nodiscard]] const CoverageField& Aggregate() const noexcept;
		[[nodiscard]] std::uint64_t Revision() const noexcept;

	private:
		void Rebuild();

		std::size_t vertexCount_{};
		FirstHiddenShell neutralValue_{};
		std::uint64_t revision_{};
		std::unordered_map<CoverageSourceID, CoverageField> sources_;
		CoverageField aggregate_;
	};

	[[nodiscard]] FirstHiddenShell FirstHiddenShellFromDistance(
		const ShellProfile& a_profile,
		float a_hitDistance) noexcept;
	[[nodiscard]] FirstHiddenShell FirstHiddenShellFromDistance(
		const ShellLayout& a_layout,
		float a_hitDistance) noexcept;

	// Immutable acceleration structure for one equipped-armor snapshot. Adaptive
	// boundary refinement samples several points per carrier triangle, so rebuilding
	// the armor BVH for every batch would dominate an otherwise event-only update.
	class ArmorCoverageQuery final
	{
	public:
		ArmorCoverageQuery() = default;

		[[nodiscard]] static std::expected<ArmorCoverageQuery, std::string> Create(
			const ShellProfile& a_profile,
			std::span<const Triangle> a_armorTriangles);
		[[nodiscard]] static std::expected<ArmorCoverageQuery, std::string> Create(
			const ShellLayout& a_layout,
			std::span<const Triangle> a_armorTriangles);

		[[nodiscard]] std::expected<CoverageField, std::string> Sample(
			std::span<const CarrierVertex> a_vertices) const;
		[[nodiscard]] bool Empty() const noexcept;
		[[nodiscard]] std::uint32_t ShellCount() const noexcept;
		[[nodiscard]] float FurLength() const noexcept;
		[[nodiscard]] float UnderSkinDepth() const noexcept;
		// Conservative broad phase used to find a small armor feature whose swept
		// volume lies wholly inside a large carrier triangle and between samples.
		[[nodiscard]] bool MayIntersect(const AxisAlignedBounds& a_bounds) const noexcept;

	private:
		struct Impl;
		explicit ArmorCoverageQuery(std::shared_ptr<const Impl> a_impl) noexcept;

		std::shared_ptr<const Impl> impl_;
	};

	// Carrier vertices and armor triangles must be expressed in the same object
	// space and reference pose. The returned values retain carrier vertex order,
	// so they are valid only for that exact carrier topology.
	[[nodiscard]] std::expected<CoverageField, std::string> SolveArmorCoverage(
		const ShellProfile& a_profile,
		std::span<const CarrierVertex> a_vertices,
		std::span<const Triangle> a_armorTriangles);
	[[nodiscard]] std::expected<CoverageField, std::string> SolveArmorCoverage(
		const ShellLayout& a_layout,
		std::span<const CarrierVertex> a_vertices,
		std::span<const Triangle> a_armorTriangles);
}
