#include "PCH.h"

#include "DynamicFur/Game/ArmorCoverageLocator.h"

#include "DynamicFur/Game/BodyGeometryLocator.h"

#include <memory>
#include <ranges>
#include <unordered_set>

namespace DynamicFur::Game
{
	namespace
	{
		[[nodiscard]] bool IsEffectivelyCulled(
			const RE::NiAVObject& a_object,
			const RE::NiAVObject& a_sourcePart) noexcept
		{
			// A visible child beneath a culled alternate/slot node is not submitted
			// by Skyrim and therefore must not participate in armor coverage.
			auto* current = std::addressof(a_object);
			for (std::uint32_t depth = 0; current && depth < 128; ++depth) {
				if (current->GetAppCulled()) {
					return true;
				}
				if (current == std::addressof(a_sourcePart)) {
					break;
				}
				if (auto* parent = current->parent) {
					if (auto* switchNode = parent->AsSwitchNode(); switchNode &&
						(switchNode->index < 0 ||
							static_cast<std::uint32_t>(switchNode->index) !=
								current->parentIndex)) {
						return true;
					}
				}
				current = current->parent;
			}
			return false;
		}
	}

	ArmorCoverageLocator::ArmorCoverageLocator(
		const FurSourcePolicy& a_policy,
		const ArmorBlacklist& a_blacklist) noexcept :
		policy_(a_policy),
		blacklist_(a_blacklist)
	{}

	ArmorCoverageSnapshot ArmorCoverageLocator::Locate(RE::Actor* a_actor) const
	{
		ArmorCoverageSnapshot result;
		if (!a_actor || !policy_.Keyword()) {
			return result;
		}

		const auto& bipedPointer = a_actor->GetBiped(false);
		auto* biped = bipedPointer.get();
		if (!biped || !biped->root) {
			return result;
		}
		result.skeletonRoot = biped->root;

		std::unordered_set<RE::TESObjectARMO*> seenArmors;
		std::unordered_set<RE::TESObjectARMO*> seenBlacklistedArmors;
		std::unordered_set<RE::NiAVObject*> seenParts;
		std::unordered_set<RE::BSGeometry*> seenGeometries;
		for (std::uint32_t slot = 0; slot < RE::BIPED_OBJECTS::kEditorTotal; ++slot) {
			auto& object = biped->objects[slot];
			auto* armor = object.item ? object.item->As<RE::TESObjectARMO>() : nullptr;
			if (!armor || armor->HasKeyword(policy_.Keyword())) {
				continue;
			}
			if (blacklist_.Contains(armor)) {
				seenBlacklistedArmors.insert(armor);
				continue;
			}
			if (!object.addon ||
				!object.partClone) {
				continue;
			}
			if (std::ranges::find(armor->armorAddons, object.addon) ==
				armor->armorAddons.end()) {
				continue;
			}

			seenArmors.insert(armor);
			if (!seenParts.insert(object.partClone.get()).second) {
				continue;
			}
			result.sourceParts.push_back(object.partClone);
			auto* sourcePart = object.partClone.get();
			RE::BSVisit::TraverseScenegraphGeometries(
				sourcePart,
				[&](RE::BSGeometry* a_geometry) {
					if (!a_geometry || IsEffectivelyCulled(*a_geometry, *sourcePart) ||
						!seenGeometries.insert(a_geometry).second) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					result.geometries.push_back({
						RE::NiPointer<RE::BSGeometry>{ a_geometry },
						armor,
						object.addon,
						sourcePart,
						armor->GetFormID(),
						GeometryDiffuseTexturePath(*a_geometry)
					});
					return RE::BSVisit::BSVisitControl::kContinue;
				});
		}

		result.armorCount = static_cast<std::uint32_t>(seenArmors.size());
		result.blacklistedArmorCount = static_cast<std::uint32_t>(seenBlacklistedArmors.size());
		return result;
	}
}
