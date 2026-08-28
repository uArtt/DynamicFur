#pragma once

#include "DynamicFur/Game/ArmorBlacklist.h"
#include "DynamicFur/Game/FurSourcePolicy.h"

#include "RE/B/BSGeometry.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiNode.h"
#include "RE/N/NiSmartPointer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class Actor;
	class TESObjectARMA;
	class TESObjectARMO;
}

namespace DynamicFur::Game
{
	struct LocatedArmorGeometry
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::TESObjectARMO* armor{};
		RE::TESObjectARMA* addon{};
		RE::NiAVObject* sourcePart{};
		std::uint32_t armorFormID{};
		std::string diffusePath;
	};

	struct ArmorCoverageSnapshot
	{
		// Borrowed identity token. All geometry/part objects are retained below.
		RE::NiNode* skeletonRoot{};
		std::vector<RE::NiPointer<RE::NiAVObject>> sourceParts;
		std::vector<LocatedArmorGeometry> geometries;
		std::uint32_t armorCount{};
		std::uint32_t blacklistedArmorCount{};
	};

	// Resolves only live third-person BIPOBJECTs. It never scans inventory or
	// the global form database. HasFur parts are excluded from this first binary
	// coverage policy so a fur source cannot erase its own carrier.
	class ArmorCoverageLocator final
	{
	public:
		ArmorCoverageLocator(
			const FurSourcePolicy& a_policy,
			const ArmorBlacklist& a_blacklist) noexcept;

		[[nodiscard]] ArmorCoverageSnapshot Locate(RE::Actor* a_actor) const;

	private:
		const FurSourcePolicy& policy_;
		const ArmorBlacklist& blacklist_;
	};
}
