#include "PCH.h"

#include "DynamicFur/Game/SkinnedGeometryCloner.h"

#include "RE/B/BSVisit.h"
#include "RE/M/MemoryManager.h"
#include "RE/N/NiAlphaProperty.h"
#include "RE/N/NiRefObject.h"

#include <cmath>

namespace DynamicFur::Game
{
	namespace
	{
		inline constexpr std::size_t kMaximumSkeletonDepth = 512;
		inline constexpr std::uint32_t kMaximumSaneBoneCount = 512;

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

		[[nodiscard]] bool IsFiniteTransform(
			const RE::NiTransform& a_transform) noexcept
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

		[[nodiscard]] RE::NiPointer<RE::NiAlphaProperty> CloneAlphaProperty(
			RE::NiAlphaProperty* a_source)
		{
			if (!a_source) {
				return {};
			}
			RE::NiPointer<RE::NiObject> clonedObject{ a_source->Clone() };
			auto* clonedAlpha = clonedObject ?
				netimmerse_cast<RE::NiAlphaProperty*>(clonedObject.get()) : nullptr;
			if (!clonedAlpha || clonedAlpha == a_source) {
				return {};
			}
			return RE::NiPointer<RE::NiAlphaProperty>{ clonedAlpha };
		}

		[[nodiscard]] RE::NiPointer<RE::NiAlphaProperty> CreateAlphaProperty()
		{
			// CommonLib exposes the runtime vtable but no NiAlphaProperty factory.
			// Allocate the known POD-like NetImmerse layout on Skyrim's heap and
			// install the executable's version-dependent vtable before the object
			// is ever observed by the scene graph.
			auto* created = RE::calloc<RE::NiAlphaProperty>(1);
			if (!created) {
				return {};
			}
			if (!RE::stl::emplace_vtable<RE::NiAlphaProperty>(created)) {
				RE::free(created);
				return {};
			}

			// Mirror NiRefObject's constructor bookkeeping. All other base members
			// are nullable handles/pointers and their zero state is valid.
			RE::stl::atomic_ref objectCount{
				*RE::NiRefObject::GetTotalObjectCount()
			};
			++objectCount;
			return RE::NiPointer<RE::NiAlphaProperty>{ created };
		}

		[[nodiscard]] RE::NiPointer<RE::NiAlphaProperty> FindAlphaTemplate(
			RE::NiNode& a_skeletonRoot)
		{
			RE::NiPointer<RE::NiAlphaProperty> result;
			RE::BSVisit::TraverseScenegraphGeometries(
				std::addressof(a_skeletonRoot),
				[&result](RE::BSGeometry* a_geometry) {
					if (!a_geometry) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					auto* alpha =
						a_geometry->GetGeometryRuntimeData().alphaProperty.get();
					if (!alpha) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					result.reset(alpha);
					return RE::BSVisit::BSVisitControl::kStop;
				});
			return result;
		}

		[[nodiscard]] bool PrepareAlphaTesting(
			RE::BSGeometry& a_source,
			RE::BSGeometry& a_clone,
			RE::NiNode& a_skeletonRoot)
		{
			auto& sourceRuntime = a_source.GetGeometryRuntimeData();
			auto& cloneRuntime = a_clone.GetGeometryRuntimeData();

			RE::NiPointer<RE::NiAlphaProperty> alpha = cloneRuntime.alphaProperty;
			if (alpha && alpha.get() == sourceRuntime.alphaProperty.get()) {
				alpha = CloneAlphaProperty(alpha.get());
			}

			if (!alpha) {
				auto alphaTemplate = sourceRuntime.alphaProperty;
				if (!alphaTemplate) {
					alphaTemplate = FindAlphaTemplate(a_skeletonRoot);
				}
				alpha = CloneAlphaProperty(alphaTemplate.get());
				if (!alpha) {
					alpha = CreateAlphaProperty();
					if (alpha) {
						SKSE::log::info(
							"Created a runtime NiAlphaProperty because the loaded actor graph exposed no alpha-test template");
					}
				}
			}
			if (!alpha) {
				return false;
			}

			alpha->SetAlphaBlending(false);
			alpha->SetAlphaTesting(true);
			alpha->alphaFlags &=
				static_cast<std::uint16_t>(~(0x7U << 10U));
			alpha->alphaFlags |= static_cast<std::uint16_t>(
				std::to_underlying(RE::NiAlphaProperty::TestFunction::kGreater) << 10U);
			// Keep one real base-layer draw so every supported runtime schedules the
			// carrier in its Lighting pass. A threshold of 255 with kGreater is an
			// impossible test and Skyrim 1.7.99 can omit that colour pass entirely.
			// Direct D3D routing replaces this draw with all layers; RestoreGeometry
			// fallback retains it as layer zero and contributes the outer layers.
			alpha->alphaThreshold = 1;

			// Use the engine's virtual attachment path so the geometry observes
			// the property exactly as if it had been authored in the NIF.
			a_clone.AttachProperty(alpha.get());
			const bool attached = cloneRuntime.alphaProperty.get() == alpha.get() &&
				cloneRuntime.alphaProperty->GetAlphaTesting();
			if (attached && cloneRuntime.shaderProperty) {
				// The source body's cached Lighting pass was built without alpha test.
				// Invalidate again after attaching NiAlphaProperty so Skyrim rebuilds
				// the clone with kDoAlphaTest instead of reusing 0x4D406633.
				cloneRuntime.shaderProperty->DoClearRenderPasses();
			}
			return attached;
		}

		[[nodiscard]] bool PrepareShaderProperty(
			RE::BSGeometry::GEOMETRY_RUNTIME_DATA& a_source,
			RE::BSGeometry::GEOMETRY_RUNTIME_DATA& a_clone)
		{
			if (!a_source.shaderProperty || !a_clone.shaderProperty) {
				return false;
			}

			if (a_source.shaderProperty == a_clone.shaderProperty) {
				RE::NiPointer<RE::NiObject> clonedObject{
					a_source.shaderProperty->Clone()
				};
				auto* clonedShader = clonedObject ?
					netimmerse_cast<RE::BSShaderProperty*>(clonedObject.get()) : nullptr;
				if (!clonedShader || clonedShader == a_source.shaderProperty.get()) {
					return false;
				}
				a_clone.shaderProperty =
					RE::NiPointer<RE::BSShaderProperty>{ clonedShader };
			}

			// Render-pass arrays contain geometry pointers and cannot be reused
			// from the source body after it was cloned into the carrier.
			a_clone.shaderProperty->DoClearRenderPasses();
			return true;
		}
	}

	SkinBindingStatus SkinBindingValidator::Validate(
		RE::BSGeometry& a_source,
		RE::BSGeometry& a_clone,
		RE::NiNode& a_skeletonRoot) const noexcept
	{
		auto* sourceSkin = a_source.GetGeometryRuntimeData().skinInstance.get();
		auto* cloneSkin = a_clone.GetGeometryRuntimeData().skinInstance.get();
		if (!sourceSkin || !cloneSkin) {
			return SkinBindingStatus::kMissingSkinInstance;
		}
		if (sourceSkin == cloneSkin) {
			return SkinBindingStatus::kSharedSkinInstance;
		}
		if (!sourceSkin->skinData || !cloneSkin->skinData) {
			return SkinBindingStatus::kMissingSkinData;
		}
		if (!sourceSkin->skinPartition || !cloneSkin->skinPartition) {
			return SkinBindingStatus::kMissingSkinPartition;
		}
		if (!sourceSkin->rootParent || !cloneSkin->rootParent) {
			return SkinBindingStatus::kMissingRootParent;
		}
		if (sourceSkin->rootParent != cloneSkin->rootParent) {
			return SkinBindingStatus::kRootParentMismatch;
		}
		if (!IsInsideSkeleton(sourceSkin->rootParent, std::addressof(a_skeletonRoot)) &&
			!IsInsideSkeleton(std::addressof(a_skeletonRoot), sourceSkin->rootParent)) {
			return SkinBindingStatus::kRootOutsideSkeleton;
		}

		const auto sourceBoneCount = sourceSkin->skinData->GetBoneCount();
		const auto cloneBoneCount = cloneSkin->skinData->GetBoneCount();
		if (sourceBoneCount == 0 || sourceBoneCount > kMaximumSaneBoneCount ||
			sourceBoneCount != cloneBoneCount) {
			return SkinBindingStatus::kInvalidBoneCount;
		}
		if (!sourceSkin->bones || !cloneSkin->bones) {
			return SkinBindingStatus::kMissingBoneArray;
		}

		for (std::uint32_t index = 0; index < sourceBoneCount; ++index) {
			auto* sourceBone = sourceSkin->bones[index];
			auto* cloneBone = cloneSkin->bones[index];
			if (!sourceBone || cloneBone != sourceBone) {
				return SkinBindingStatus::kBoneMismatch;
			}
			if (!IsInsideSkeleton(sourceBone, sourceSkin->rootParent)) {
				return SkinBindingStatus::kBoneOutsideSkeleton;
			}
		}
		return SkinBindingStatus::kValid;
	}

	SkinnedGeometryCloner::SkinnedGeometryCloner(
		const ISkinBindingValidator& a_validator) noexcept :
		validator_(a_validator)
	{}

	GeometryCloneResult SkinnedGeometryCloner::Clone(
		RE::BSGeometry& a_source,
		RE::NiNode& a_skeletonRoot) const
	{
		GeometryCloneResult result;
		if (!IsFiniteTransform(a_source.world) || !IsFiniteTransform(a_skeletonRoot.world)) {
			result.status = GeometryCloneStatus::kInvalidTransform;
			return result;
		}

		RE::NiPointer<RE::NiObject> clonedObject{ a_source.Clone() };
		if (!clonedObject) {
			result.status = GeometryCloneStatus::kEngineCloneFailed;
			return result;
		}

		auto* clonedGeometry = clonedObject->AsGeometry();
		if (!clonedGeometry || clonedGeometry == std::addressof(a_source)) {
			result.status = GeometryCloneStatus::kWrongCloneType;
			return result;
		}
		if (clonedGeometry->parent) {
			result.status = GeometryCloneStatus::kCloneAlreadyParented;
			return result;
		}

		auto& sourceRuntime = a_source.GetGeometryRuntimeData();
		auto& cloneRuntime = clonedGeometry->GetGeometryRuntimeData();
		if (!sourceRuntime.shaderProperty || !cloneRuntime.shaderProperty) {
			result.status = GeometryCloneStatus::kMissingShaderProperty;
			return result;
		}
		if (!PrepareShaderProperty(sourceRuntime, cloneRuntime)) {
			result.status = GeometryCloneStatus::kSharedShaderProperty;
			return result;
		}
		if (!PrepareAlphaTesting(a_source, *clonedGeometry, a_skeletonRoot)) {
			result.status = GeometryCloneStatus::kAlphaPropertyFailed;
			return result;
		}

		result.bindingStatus = validator_.Validate(a_source, *clonedGeometry, a_skeletonRoot);
		if (result.bindingStatus != SkinBindingStatus::kValid) {
			result.status = GeometryCloneStatus::kInvalidSkinBinding;
			return result;
		}

		const auto relativeTransform = a_skeletonRoot.world.Invert() * a_source.world;
		if (!IsFiniteTransform(relativeTransform)) {
			result.status = GeometryCloneStatus::kInvalidTransform;
			return result;
		}

		clonedGeometry->local = relativeTransform;
		clonedGeometry->collisionObject.reset();
		clonedGeometry->SetAppCulled(true);

		result.geometry.reset(clonedGeometry);
		result.status = GeometryCloneStatus::kCloned;
		return result;
	}
}
