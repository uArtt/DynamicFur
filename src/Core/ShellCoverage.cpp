#include "DynamicFur/Core/ShellCoverage.h"

#include "DynamicFur/Core/ShellMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace DynamicFur::Core
{
	namespace
	{
		struct Bounds
		{
			Vec3 minimum{
				std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity(),
				std::numeric_limits<float>::infinity()
			};
			Vec3 maximum{
				-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity(),
				-std::numeric_limits<float>::infinity()
			};

			void Include(const Vec3& a_point) noexcept
			{
				minimum = Min(minimum, a_point);
				maximum = Max(maximum, a_point);
			}
		};

		struct BvhNode
		{
			Bounds bounds;
			std::uint32_t begin{};
			std::uint32_t count{};
			std::uint32_t left{};
			std::uint32_t right{};
			bool leaf{};
		};

		[[nodiscard]] Bounds TriangleBounds(const Triangle& a_triangle) noexcept
		{
			Bounds result;
			result.Include(a_triangle.a);
			result.Include(a_triangle.b);
			result.Include(a_triangle.c);
			return result;
		}

		[[nodiscard]] Vec3 TriangleCentroid(const Triangle& a_triangle) noexcept
		{
			return {
				static_cast<float>((static_cast<double>(a_triangle.a.x) + a_triangle.b.x + a_triangle.c.x) / 3.0),
				static_cast<float>((static_cast<double>(a_triangle.a.y) + a_triangle.b.y + a_triangle.c.y) / 3.0),
				static_cast<float>((static_cast<double>(a_triangle.a.z) + a_triangle.b.z + a_triangle.c.z) / 3.0)
			};
		}

		[[nodiscard]] float AxisValue(const Vec3& a_value, std::uint32_t a_axis) noexcept
		{
			return a_axis == 0 ? a_value.x : (a_axis == 1 ? a_value.y : a_value.z);
		}

		[[nodiscard]] bool IntersectsBounds(
			const Bounds& a_bounds,
			const Vec3& a_origin,
			const Vec3& a_inverseDirection,
			float a_maxDistance) noexcept
		{
			float minimumDistance = 0.0F;
			float maximumDistance = a_maxDistance;
			for (std::uint32_t axis = 0; axis < 3; ++axis) {
				const auto origin = AxisValue(a_origin, axis);
				const auto inverse = AxisValue(a_inverseDirection, axis);
				const auto lower = AxisValue(a_bounds.minimum, axis);
				const auto upper = AxisValue(a_bounds.maximum, axis);
				if (!std::isfinite(inverse)) {
					if (origin < lower || origin > upper) {
						return false;
					}
					continue;
				}
				auto first = (lower - origin) * inverse;
				auto second = (upper - origin) * inverse;
				if (first > second) {
					std::swap(first, second);
				}
				minimumDistance = std::max(minimumDistance, first);
				maximumDistance = std::min(maximumDistance, second);
				if (minimumDistance > maximumDistance) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool Overlaps(
			const Bounds& a_first,
			const AxisAlignedBounds& a_second) noexcept
		{
			return a_first.minimum.x <= a_second.maximum.x &&
				a_first.maximum.x >= a_second.minimum.x &&
				a_first.minimum.y <= a_second.maximum.y &&
				a_first.maximum.y >= a_second.minimum.y &&
				a_first.minimum.z <= a_second.maximum.z &&
				a_first.maximum.z >= a_second.minimum.z;
		}

		[[nodiscard]] float IntersectTriangle(
			const Triangle& a_triangle,
			const Vec3& a_origin,
			const Vec3& a_direction,
			float a_maxDistance) noexcept
		{
			constexpr float epsilon = 1.0e-7F;
			const auto edge1 = a_triangle.b - a_triangle.a;
			const auto edge2 = a_triangle.c - a_triangle.a;
			const auto p = Cross(a_direction, edge2);
			const auto determinant = Dot(edge1, p);
			if (std::abs(determinant) <= epsilon) {
				return std::numeric_limits<float>::infinity();
			}
			const auto inverseDeterminant = 1.0F / determinant;
			const auto translated = a_origin - a_triangle.a;
			const auto u = Dot(translated, p) * inverseDeterminant;
			if (u < -epsilon || u > 1.0F + epsilon) {
				return std::numeric_limits<float>::infinity();
			}
			const auto q = Cross(translated, edge1);
			const auto v = Dot(a_direction, q) * inverseDeterminant;
			if (v < -epsilon || u + v > 1.0F + epsilon) {
				return std::numeric_limits<float>::infinity();
			}
			const auto distance = Dot(edge2, q) * inverseDeterminant;
			if (distance < 0.0F || distance > a_maxDistance) {
				return std::numeric_limits<float>::infinity();
			}
			return distance;
		}

		class TriangleBvh final
		{
		public:
			explicit TriangleBvh(std::span<const Triangle> a_triangles) : triangles_(a_triangles)
			{
				indices_.resize(a_triangles.size());
				triangleBounds_.resize(a_triangles.size());
				centroids_.resize(a_triangles.size());
				std::iota(indices_.begin(), indices_.end(), 0U);
				for (std::size_t index = 0; index < a_triangles.size(); ++index) {
					triangleBounds_[index] = TriangleBounds(a_triangles[index]);
					centroids_[index] = TriangleCentroid(a_triangles[index]);
				}
				if (!indices_.empty()) {
					const auto leafEstimate = (indices_.size() + 7U) / 8U;
					nodes_.reserve(leafEstimate * 2U - 1U);
					Build(0, static_cast<std::uint32_t>(indices_.size()));
				}
			}

			[[nodiscard]] float NearestHit(
				const Vec3& a_origin,
				const Vec3& a_direction,
				float a_maxDistance) const noexcept
			{
				if (nodes_.empty()) {
					return std::numeric_limits<float>::infinity();
				}
				const auto inverseComponent = [](float a_value) {
					return std::abs(a_value) <= 1.0e-12F ?
						std::numeric_limits<float>::infinity() : 1.0F / a_value;
				};
				const Vec3 inverseDirection{
					inverseComponent(a_direction.x),
					inverseComponent(a_direction.y),
					inverseComponent(a_direction.z)
				};
				float nearest = a_maxDistance;
				bool found = false;
				std::array<std::uint32_t, 128> stack{};
				std::size_t stackSize = 1;
				stack[0] = 0;
				while (stackSize != 0) {
					const auto nodeIndex = stack[--stackSize];
					const auto& node = nodes_[nodeIndex];
					if (!IntersectsBounds(node.bounds, a_origin, inverseDirection, nearest)) {
						continue;
					}
					if (node.leaf) {
						for (std::uint32_t index = 0; index < node.count; ++index) {
							const auto distance = IntersectTriangle(
								triangles_[indices_[node.begin + index]], a_origin, a_direction, nearest);
							if (distance <= nearest) {
								nearest = distance;
								found = true;
							}
						}
					} else if (stackSize + 2 <= stack.size()) {
						stack[stackSize++] = node.left;
						stack[stackSize++] = node.right;
					}
				}
				return found ? nearest : std::numeric_limits<float>::infinity();
			}

			[[nodiscard]] bool MayIntersect(
				const AxisAlignedBounds& a_bounds) const noexcept
			{
				if (nodes_.empty()) {
					return false;
				}
				std::array<std::uint32_t, 128> stack{};
				std::size_t stackSize = 1;
				stack[0] = 0;
				while (stackSize != 0) {
					const auto nodeIndex = stack[--stackSize];
					const auto& node = nodes_[nodeIndex];
					if (!Overlaps(node.bounds, a_bounds)) {
						continue;
					}
					if (node.leaf) {
						for (std::uint32_t index = 0; index < node.count; ++index) {
							if (Overlaps(
								triangleBounds_[indices_[node.begin + index]], a_bounds)) {
								return true;
							}
						}
					} else if (stackSize + 2 <= stack.size()) {
						stack[stackSize++] = node.left;
						stack[stackSize++] = node.right;
					}
				}
				return false;
			}

		private:
			std::uint32_t Build(std::uint32_t a_begin, std::uint32_t a_count)
			{
				const auto nodeIndex = static_cast<std::uint32_t>(nodes_.size());
				nodes_.emplace_back();
				nodes_[nodeIndex].begin = a_begin;
				nodes_[nodeIndex].count = a_count;
				Bounds nodeBounds;
				Bounds centroidBounds;
				for (std::uint32_t index = 0; index < a_count; ++index) {
					const auto triangleIndex = indices_[a_begin + index];
					const auto& bounds = triangleBounds_[triangleIndex];
					nodeBounds.Include(bounds.minimum);
					nodeBounds.Include(bounds.maximum);
					centroidBounds.Include(centroids_[triangleIndex]);
				}
				nodes_[nodeIndex].bounds = nodeBounds;
				if (a_count <= 8) {
					nodes_[nodeIndex].leaf = true;
					return nodeIndex;
				}

				const auto extent = centroidBounds.maximum - centroidBounds.minimum;
				const auto axis = extent.y > extent.x ? (extent.z > extent.y ? 2U : 1U) :
					(extent.z > extent.x ? 2U : 0U);
				const auto middle = a_begin + a_count / 2;
				std::nth_element(
					indices_.begin() + a_begin,
					indices_.begin() + middle,
					indices_.begin() + a_begin + a_count,
					[&](std::uint32_t a_lhs, std::uint32_t a_rhs) {
						return AxisValue(centroids_[a_lhs], axis) <
							AxisValue(centroids_[a_rhs], axis);
					});
				const auto leftCount = middle - a_begin;
				const auto left = Build(a_begin, leftCount);
				const auto right = Build(middle, a_count - leftCount);
				auto& completedNode = nodes_[nodeIndex];
				completedNode.left = left;
				completedNode.right = right;
				completedNode.leaf = false;
				return nodeIndex;
			}

			std::span<const Triangle> triangles_;
			std::vector<std::uint32_t> indices_;
			std::vector<Bounds> triangleBounds_;
			std::vector<Vec3> centroids_;
			std::vector<BvhNode> nodes_;
		};
	}

	struct ArmorCoverageQuery::Impl
	{
		Impl(const ShellLayout& a_layout, std::span<const Triangle> a_triangles) :
			layout(a_layout),
			triangles(a_triangles.begin(), a_triangles.end()),
			bvh(triangles)
		{}

		ShellLayout layout;
		std::vector<Triangle> triangles;
		TriangleBvh bvh;
	};

	CoverageField::CoverageField(std::size_t a_vertexCount, FirstHiddenShell a_neutralValue) :
		values_(a_vertexCount, a_neutralValue)
	{}

	std::size_t CoverageField::Size() const noexcept { return values_.size(); }
	bool CoverageField::Empty() const noexcept { return values_.empty(); }
	FirstHiddenShell CoverageField::operator[](std::size_t a_index) const noexcept { return values_[a_index]; }
	std::span<const FirstHiddenShell> CoverageField::Values() const noexcept { return values_; }
	std::span<FirstHiddenShell> CoverageField::Values() noexcept { return values_; }

	CoverageSet::CoverageSet(std::size_t a_vertexCount, FirstHiddenShell a_neutralValue) :
		vertexCount_(a_vertexCount),
		neutralValue_(a_neutralValue),
		aggregate_(a_vertexCount, a_neutralValue)
	{}

	bool CoverageSet::Set(CoverageSourceID a_source, CoverageField a_field)
	{
		if (a_field.Size() != vertexCount_) {
			return false;
		}
		sources_.insert_or_assign(a_source, std::move(a_field));
		++revision_;
		Rebuild();
		return true;
	}

	bool CoverageSet::Remove(CoverageSourceID a_source)
	{
		if (sources_.erase(a_source) == 0) {
			return false;
		}
		++revision_;
		Rebuild();
		return true;
	}

	void CoverageSet::Clear()
	{
		if (sources_.empty()) {
			return;
		}
		sources_.clear();
		++revision_;
		Rebuild();
	}

	const CoverageField& CoverageSet::Aggregate() const noexcept { return aggregate_; }
	std::uint64_t CoverageSet::Revision() const noexcept { return revision_; }

	void CoverageSet::Rebuild()
	{
		CoverageField rebuilt(vertexCount_, neutralValue_);
		auto aggregateValues = rebuilt.Values();
		for (const auto& [source, field] : sources_) {
			(void)source;
			const auto fieldValues = field.Values();
			for (std::size_t index = 0; index < vertexCount_; ++index) {
				aggregateValues[index] = std::min(aggregateValues[index], fieldValues[index]);
			}
		}
		aggregate_ = std::move(rebuilt);
	}

	FirstHiddenShell FirstHiddenShellFromDistance(
		const ShellProfile& a_profile,
		float a_hitDistance) noexcept
	{
		if (a_profile.shellCount == 0 || a_profile.shellCount > kMaximumShellCount ||
			!std::isfinite(a_profile.furLength) || a_profile.furLength <= 0.0F ||
			!std::isfinite(a_profile.distributionExponent) || a_profile.distributionExponent <= 0.0F) {
			return kFullyVisibleCoverage;
		}
		if (!std::isfinite(a_hitDistance)) {
			return static_cast<FirstHiddenShell>(a_profile.shellCount);
		}
		if (a_hitDistance <= 0.0F) {
			return 0;
		}
		for (std::uint32_t index = 0; index < a_profile.shellCount; ++index) {
			if (SampleShell(a_profile, index, a_profile.shellCount).offset >= a_hitDistance) {
				return static_cast<FirstHiddenShell>(index);
			}
		}
		return static_cast<FirstHiddenShell>(a_profile.shellCount);
	}

	FirstHiddenShell FirstHiddenShellFromDistance(
		const ShellLayout& a_layout,
		float a_hitDistance) noexcept
	{
		return static_cast<FirstHiddenShell>(a_layout.FirstShellAtOrBeyond(a_hitDistance));
	}

	ArmorCoverageQuery::ArmorCoverageQuery(
		std::shared_ptr<const Impl> a_impl) noexcept :
		impl_(std::move(a_impl))
	{}

	std::expected<ArmorCoverageQuery, std::string> ArmorCoverageQuery::Create(
		const ShellProfile& a_profile,
		std::span<const Triangle> a_armorTriangles)
	{
		auto layout = ShellLayout::Create(a_profile);
		if (!layout) {
			return std::unexpected(std::move(layout.error()));
		}
		return Create(*layout, a_armorTriangles);
	}

	std::expected<ArmorCoverageQuery, std::string> ArmorCoverageQuery::Create(
		const ShellLayout& a_layout,
		std::span<const Triangle> a_armorTriangles)
	{
		if (a_armorTriangles.size() > std::numeric_limits<std::uint32_t>::max()) {
			return std::unexpected("armor triangle count exceeds the Core index limit");
		}
		for (const auto& triangle : a_armorTriangles) {
			if (!IsFinite(triangle.a) || !IsFinite(triangle.b) || !IsFinite(triangle.c)) {
				return std::unexpected("armor triangles must contain only finite values");
			}
		}
		return ArmorCoverageQuery{
			std::make_shared<const Impl>(a_layout, a_armorTriangles)
		};
	}

	std::expected<CoverageField, std::string> ArmorCoverageQuery::Sample(
		std::span<const CarrierVertex> a_vertices) const
	{
		if (!impl_) {
			return std::unexpected("armor coverage query is not initialized");
		}
		CoverageField result(
			a_vertices.size(), static_cast<FirstHiddenShell>(impl_->layout.ShellCount()));
		if (a_vertices.empty() || impl_->triangles.empty()) {
			return result;
		}
		for (const auto& vertex : a_vertices) {
			if (!IsFinite(vertex.position) || !IsFinite(vertex.extrusionNormal)) {
				return std::unexpected("carrier vertices must contain only finite values");
			}
		}

		const auto& profile = impl_->layout.Profile();
		// One continuous segment handles both cases without doubling BVH work:
		// armor slightly beneath an oversized body mesh and ordinary armor along
		// the outward fur extrusion. armorMargin remains the minimum backtrack for
		// profiles that disable the wider under-skin recovery.
		const auto backtrack = std::max(profile.armorMargin, profile.armorUnderSkinDepth);
		const auto rayLength = profile.furLength + backtrack;
		for (std::size_t index = 0; index < a_vertices.size(); ++index) {
			const auto direction = Normalize(a_vertices[index].extrusionNormal);
			if (LengthSquared(direction) <= 1.0e-20F) {
				continue;
			}
			const auto origin = a_vertices[index].position - direction * backtrack;
			if (!IsFinite(origin)) {
				return std::unexpected("coverage ray origin is outside the finite float range");
			}
			const auto hit = impl_->bvh.NearestHit(origin, direction, rayLength);
			if (std::isfinite(hit)) {
				const auto distanceFromSkin = std::max(0.0F, hit - backtrack);
				result.Values()[index] = FirstHiddenShellFromDistance(
					impl_->layout, distanceFromSkin);
			}
		}
		return result;
	}

	bool ArmorCoverageQuery::Empty() const noexcept
	{
		return !impl_ || impl_->triangles.empty();
	}

	std::uint32_t ArmorCoverageQuery::ShellCount() const noexcept
	{
		return impl_ ? impl_->layout.ShellCount() : 0;
	}

	float ArmorCoverageQuery::FurLength() const noexcept
	{
		return impl_ ? impl_->layout.Profile().furLength : 0.0F;
	}

	float ArmorCoverageQuery::UnderSkinDepth() const noexcept
	{
		return impl_ ? impl_->layout.Profile().armorUnderSkinDepth : 0.0F;
	}

	bool ArmorCoverageQuery::MayIntersect(
		const AxisAlignedBounds& a_bounds) const noexcept
	{
		if (!impl_ || impl_->triangles.empty() ||
			!IsFinite(a_bounds.minimum) || !IsFinite(a_bounds.maximum) ||
			a_bounds.minimum.x > a_bounds.maximum.x ||
			a_bounds.minimum.y > a_bounds.maximum.y ||
			a_bounds.minimum.z > a_bounds.maximum.z) {
			return false;
		}
		return impl_->bvh.MayIntersect(a_bounds);
	}

	std::expected<CoverageField, std::string> SolveArmorCoverage(
		const ShellProfile& a_profile,
		std::span<const CarrierVertex> a_vertices,
		std::span<const Triangle> a_armorTriangles)
	{
		auto layout = ShellLayout::Create(a_profile);
		if (!layout) {
			return std::unexpected(std::move(layout.error()));
		}
		return SolveArmorCoverage(*layout, a_vertices, a_armorTriangles);
	}

	std::expected<CoverageField, std::string> SolveArmorCoverage(
		const ShellLayout& a_layout,
		std::span<const CarrierVertex> a_vertices,
		std::span<const Triangle> a_armorTriangles)
	{
		auto query = ArmorCoverageQuery::Create(a_layout, a_armorTriangles);
		if (!query) {
			return std::unexpected(std::move(query.error()));
		}
		return query->Sample(a_vertices);
	}
}
