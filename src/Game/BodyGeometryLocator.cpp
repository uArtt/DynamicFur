#include "PCH.h"

#include "DynamicFur/Game/BodyGeometryLocator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace DynamicFur::Game
{
	namespace
	{
		inline constexpr std::size_t kMaximumSkeletonDepth = 512;
		inline constexpr std::uint32_t kMaximumSaneBoneCount = 512;

		struct SkinInspection
		{
			const char* rejectionReason{ "unknown" };
			std::uint32_t boneCount{};
			std::uint32_t matrixCount{};
			std::uint32_t vertexCount{};
			bool rootUnderSkeleton{};
			bool skeletonUnderRoot{};

			[[nodiscard]] bool IsUsable() const noexcept
			{
				return rejectionReason == nullptr;
			}
		};

		struct RejectedGeometry
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			std::string diffusePath;
			SkinInspection inspection;
		};

		[[nodiscard]] bool IsInsideSkeleton(
			const RE::NiAVObject* a_object,
			const RE::NiAVObject* a_skeletonRoot) noexcept
		{
			auto* current = a_object;
			for (std::size_t depth = 0;
				current && depth < kMaximumSkeletonDepth;
				++depth, current = current->parent) {
				if (current == a_skeletonRoot) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool IsRelatedToSkeleton(
			const RE::NiAVObject* a_object,
			const RE::NiNode* a_skeletonRoot) noexcept
		{
			return IsInsideSkeleton(a_object, a_skeletonRoot) ||
				IsInsideSkeleton(a_skeletonRoot, a_object);
		}

		[[nodiscard]] SkinInspection InspectSkin(
			RE::BSGeometry& a_geometry,
			RE::NiNode& a_skeletonRoot) noexcept
		{
			SkinInspection inspection;
			const auto& runtime = a_geometry.GetGeometryRuntimeData();
			auto* skin = runtime.skinInstance.get();
			if (!skin) {
				inspection.rejectionReason = "missing-skin-instance";
				return inspection;
			}
			inspection.matrixCount = skin->numMatrices;
			if (!skin->skinData) {
				inspection.rejectionReason = "missing-skin-data";
				return inspection;
			}
			inspection.boneCount = skin->skinData->GetBoneCount();
			if (!skin->skinPartition) {
				inspection.rejectionReason = "missing-skin-partition";
				return inspection;
			}
			inspection.vertexCount = skin->skinPartition->vertexCount;
			if (!skin->rootParent) {
				inspection.rejectionReason = "missing-root-parent";
				return inspection;
			}
			if (!skin->bones) {
				inspection.rejectionReason = "missing-bone-array";
				return inspection;
			}

			if (inspection.boneCount == 0 ||
				inspection.boneCount > kMaximumSaneBoneCount) {
				inspection.rejectionReason = "invalid-bone-count";
				return inspection;
			}
			if (inspection.vertexCount == 0) {
				inspection.rejectionReason = "empty-skin-partition";
				return inspection;
			}

			inspection.rootUnderSkeleton =
				IsInsideSkeleton(skin->rootParent, std::addressof(a_skeletonRoot));
			inspection.skeletonUnderRoot =
				IsInsideSkeleton(std::addressof(a_skeletonRoot), skin->rootParent);
			if (!IsRelatedToSkeleton(skin->rootParent, std::addressof(a_skeletonRoot))) {
				inspection.rejectionReason = "unrelated-root-parent";
				return inspection;
			}

			for (std::uint32_t index = 0; index < inspection.boneCount; ++index) {
				auto* bone = skin->bones[index];
				if (!bone) {
					inspection.rejectionReason = "missing-bone";
					return inspection;
				}
				if (!IsInsideSkeleton(bone, skin->rootParent)) {
					inspection.rejectionReason = "bone-outside-skin-root";
					return inspection;
				}
			}

			inspection.rejectionReason = nullptr;
			return inspection;
		}

	}

	std::string NormalizeTexturePath(std::string_view a_path)
	{
		std::string normalized{ a_path };
		for (auto& character : normalized) {
			if (character == '/') {
				character = '\\';
			} else {
				character = static_cast<char>(
					std::tolower(static_cast<unsigned char>(character)));
			}
		}

		while (normalized.starts_with(".\\")) {
			normalized.erase(0, 2);
		}
		while (!normalized.empty() && normalized.front() == '\\') {
			normalized.erase(normalized.begin());
		}
		if (normalized.starts_with("data\\")) {
			normalized.erase(0, 5);
		}
		if (normalized.starts_with("textures\\")) {
			normalized.erase(0, 9);
		}
		return normalized;
	}

	std::string GeometryDiffuseTexturePath(RE::BSGeometry& a_geometry)
	{
		auto* shader = a_geometry.lightingShaderProp_cast();
		auto* baseMaterial = shader ? shader->GetBaseMaterial() : nullptr;
		if (!baseMaterial ||
			baseMaterial->GetType() != RE::BSShaderMaterial::Type::kLighting) {
			return {};
		}

		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(baseMaterial);
		const auto textureSet = material->GetTextureSet();
		if (textureSet) {
			const auto* path = textureSet->GetTexturePath(
				RE::BSTextureSet::Textures::kDiffuse);
			if (path && *path) {
				return NormalizeTexturePath(path);
			}
		}

		auto* diffuse = material->diffuseTexture.get();
		return diffuse ? NormalizeTexturePath(diffuse->name.c_str()) : std::string{};
	}

	BodyGeometryLocator::BodyGeometryLocator(
		const FurSourcePolicy& a_policy) noexcept :
		policy_(a_policy)
	{}

	BodyGeometryResult BodyGeometryLocator::Locate(
		const BodyGeometryQuery& a_query) const
	{
		BodyGeometryResult result;
		if (!a_query.actor || !policy_.Keyword()) {
			result.status = BodyGeometryStatus::kInvalidQuery;
			return result;
		}
		if (a_query.perspective != Perspective::kThirdPerson) {
			result.status = BodyGeometryStatus::kUnsupportedPerspective;
			return result;
		}

		const auto& bipedPointer = a_query.actor->GetBiped(false);
		auto* biped = bipedPointer.get();
		if (!biped || !biped->root) {
			result.status = BodyGeometryStatus::kActor3DUnavailable;
			return result;
		}
		result.body.skeletonRoot = biped->root;

		auto* keyword = policy_.Keyword();
		auto* skinArmor = a_query.actor->GetSkin();
		const bool markedSkin = skinArmor && skinArmor->HasKeyword(keyword);
		bool markedSkinCovered = false;
		std::unordered_set<RE::NiAVObject*> seenParts;

		// BIPOBJECT is the authoritative runtime link between the ARMO selected
		// by Skyrim, its race-compatible ARMA and the live scene-graph clone.
		// Scan only the 32 editor biped slots; weapon animation slots are not
		// armor sources and must never be interpreted as fur carriers.
		for (std::uint32_t slot = 0;
			slot < RE::BIPED_OBJECTS::kEditorTotal;
			++slot) {
			auto& object = biped->objects[slot];
			const auto slotFlag = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(
				std::uint32_t{ 1 } << slot);

			if (markedSkin && skinArmor->HasPartOf(slotFlag) &&
				object.item != skinArmor) {
				markedSkinCovered = true;
			}

			auto* armor = object.item ? object.item->As<RE::TESObjectARMO>() : nullptr;
			if (!armor || !armor->HasKeyword(keyword) ||
				!object.addon || !object.partClone) {
				continue;
			}

			// Do not trust a coincidental BIPOBJECT pointer combination. The live
			// ARMA must actually be declared by the keyword-bearing ARMO.
			if (std::ranges::find(armor->armorAddons, object.addon) ==
				armor->armorAddons.end()) {
				continue;
			}

			if (seenParts.insert(object.partClone.get()).second) {
				result.body.sourceParts.push_back(object.partClone);
			}
		}

		// The marked Skin ARMO is the persistent body source. If any of its
		// declared slots is currently replaced, preserve the already-built
		// carrier instead of accidentally rebuilding it from worn armor.
		if (markedSkinCovered) {
			result.body.sourceParts.clear();
			result.status = BodyGeometryStatus::kBodyCovered;
			return result;
		}

		if (result.body.sourceParts.empty()) {
			result.status = markedSkin ?
				BodyGeometryStatus::kBodySlotUnavailable :
				BodyGeometryStatus::kNoMatchingGeometry;
			return result;
		}

		std::vector<RejectedGeometry> rejectedGeometries;
		std::unordered_set<RE::BSGeometry*> seenGeometries;
		std::size_t visitedGeometryCount{};
		for (const auto& sourcePart : result.body.sourceParts) {
			RE::BSVisit::TraverseScenegraphGeometries(
				sourcePart.get(),
				[&](RE::BSGeometry* a_geometry) {
					if (!a_geometry || !seenGeometries.insert(a_geometry).second) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					++visitedGeometryCount;
					const auto inspection = InspectSkin(*a_geometry, *biped->root);
					if (inspection.IsUsable()) {
						result.body.geometries.emplace_back(a_geometry);
					} else {
						rejectedGeometries.push_back({
							RE::NiPointer<RE::BSGeometry>{ a_geometry },
							GeometryDiffuseTexturePath(*a_geometry),
							inspection
						});
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
		}

		static std::atomic_bool reportedRejection{};
		if (result.body.geometries.empty()) {
			if (!reportedRejection.exchange(true, std::memory_order_acq_rel)) {
				SKSE::log::warn(
					"HasFur traversal found {} active ARMA part(s) and {} geometries, but none had a usable live skin binding",
					result.body.sourceParts.size(), visitedGeometryCount);
				for (const auto& rejected : rejectedGeometries) {
					SKSE::log::warn(
						"Rejected HasFur geometry '{}' (reason {}, diffuse '{}', bones {}, matrices {}, vertices {}, root-under-skeleton {}, skeleton-under-root {})",
						rejected.geometry->name.c_str(),
						rejected.inspection.rejectionReason,
						rejected.diffusePath.empty() ? "<unavailable>" : rejected.diffusePath,
						rejected.inspection.boneCount,
						rejected.inspection.matrixCount,
						rejected.inspection.vertexCount,
						rejected.inspection.rootUnderSkeleton,
						rejected.inspection.skeletonUnderRoot);
				}
			}
		} else {
			reportedRejection.store(false, std::memory_order_release);
		}

		result.status = result.body.geometries.empty() ?
			BodyGeometryStatus::kNoMatchingGeometry : BodyGeometryStatus::kFound;
		return result;
	}
}
