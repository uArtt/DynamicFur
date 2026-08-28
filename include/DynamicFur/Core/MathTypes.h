#pragma once

#include <algorithm>
#include <cmath>

namespace DynamicFur::Core
{
	struct Vec3
	{
		float x{};
		float y{};
		float z{};

		[[nodiscard]] constexpr Vec3 operator+(const Vec3& a_rhs) const noexcept
		{
			return { x + a_rhs.x, y + a_rhs.y, z + a_rhs.z };
		}

		[[nodiscard]] constexpr Vec3 operator-(const Vec3& a_rhs) const noexcept
		{
			return { x - a_rhs.x, y - a_rhs.y, z - a_rhs.z };
		}

		[[nodiscard]] constexpr Vec3 operator*(float a_scalar) const noexcept
		{
			return { x * a_scalar, y * a_scalar, z * a_scalar };
		}

		constexpr Vec3& operator+=(const Vec3& a_rhs) noexcept
		{
			x += a_rhs.x;
			y += a_rhs.y;
			z += a_rhs.z;
			return *this;
		}
	};

	[[nodiscard]] constexpr Vec3 Min(const Vec3& a_lhs, const Vec3& a_rhs) noexcept
	{
		return {
			std::min(a_lhs.x, a_rhs.x),
			std::min(a_lhs.y, a_rhs.y),
			std::min(a_lhs.z, a_rhs.z)
		};
	}

	[[nodiscard]] constexpr Vec3 Max(const Vec3& a_lhs, const Vec3& a_rhs) noexcept
	{
		return {
			std::max(a_lhs.x, a_rhs.x),
			std::max(a_lhs.y, a_rhs.y),
			std::max(a_lhs.z, a_rhs.z)
		};
	}

	[[nodiscard]] constexpr float Dot(const Vec3& a_lhs, const Vec3& a_rhs) noexcept
	{
		return a_lhs.x * a_rhs.x + a_lhs.y * a_rhs.y + a_lhs.z * a_rhs.z;
	}

	[[nodiscard]] constexpr Vec3 Cross(const Vec3& a_lhs, const Vec3& a_rhs) noexcept
	{
		return {
			a_lhs.y * a_rhs.z - a_lhs.z * a_rhs.y,
			a_lhs.z * a_rhs.x - a_lhs.x * a_rhs.z,
			a_lhs.x * a_rhs.y - a_lhs.y * a_rhs.x
		};
	}

	[[nodiscard]] inline float LengthSquared(const Vec3& a_value) noexcept
	{
		return Dot(a_value, a_value);
	}

	[[nodiscard]] inline bool IsFinite(const Vec3& a_value) noexcept
	{
		return std::isfinite(a_value.x) &&
			std::isfinite(a_value.y) &&
			std::isfinite(a_value.z);
	}

	[[nodiscard]] inline Vec3 Normalize(const Vec3& a_value) noexcept
	{
		if (!IsFinite(a_value)) {
			return {};
		}
		const auto largest = std::max(
			std::abs(a_value.x), std::max(std::abs(a_value.y), std::abs(a_value.z)));
		if (largest <= 1.0e-10F) {
			return {};
		}
		const Vec3 scaled{
			a_value.x / largest,
			a_value.y / largest,
			a_value.z / largest
		};
		return scaled * (1.0F / std::sqrt(LengthSquared(scaled)));
	}
}
