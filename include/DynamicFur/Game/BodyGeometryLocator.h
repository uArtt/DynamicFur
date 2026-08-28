#pragma once

#include "DynamicFur/Game/FurSourcePolicy.h"

#include "RE/B/BSGeometry.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiNode.h"
#include "RE/N/NiSmartPointer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class Actor;
}

namespace DynamicFur::Game
{
	enum class Perspective : std::uint8_t
	{
		kThirdPerson,
		kFirstPersonFuture
	};

	enum class BodyGeometryStatus : std::uint8_t
	{
		kFound,
		kInvalidQuery,
		kUnsupportedPerspective,
		kActor3DUnavailable,
		kBodySlotUnavailable,
		kBodyCovered,
		kNoMatchingGeometry
	};

	struct BodyGeometryQuery
	{
		RE::Actor* actor{};
		Perspective perspective{ Perspective::kThirdPerson };
	};

	struct LocatedBodyGeometry
	{
		// Borrowed from the actor. Valid only while this actor 3D remains loaded.
		RE::NiNode* skeletonRoot{};

		// Strong references protect every distinct active ARMA part while a
		// carrier is built. One partClone can occupy more than one biped slot.
		std::vector<RE::NiPointer<RE::NiAVObject>> sourceParts;
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};

	struct BodyGeometryResult
	{
		BodyGeometryStatus status{ BodyGeometryStatus::kInvalidQuery };
		LocatedBodyGeometry body;
	};

	class IBodyGeometryLocator
	{
	public:
		virtual ~IBodyGeometryLocator() = default;

		[[nodiscard]] virtual BodyGeometryResult Locate(
			const BodyGeometryQuery& a_query) const = 0;
	};

	class BodyGeometryLocator final : public IBodyGeometryLocator
	{
	public:
		explicit BodyGeometryLocator(const FurSourcePolicy& a_policy) noexcept;

		[[nodiscard]] BodyGeometryResult Locate(
			const BodyGeometryQuery& a_query) const override;

	private:
		const FurSourcePolicy& policy_;
	};

	[[nodiscard]] std::string NormalizeTexturePath(std::string_view a_path);
	[[nodiscard]] std::string GeometryDiffuseTexturePath(RE::BSGeometry& a_geometry);
}
