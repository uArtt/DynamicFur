#pragma once

#include "RE/B/BSGeometry.h"
#include "RE/N/NiNode.h"
#include "RE/N/NiSmartPointer.h"

#include <cstdint>

namespace DynamicFur::Game
{
	enum class SkinBindingStatus : std::uint8_t
	{
		kNotChecked,
		kValid,
		kMissingSkinInstance,
		kSharedSkinInstance,
		kMissingSkinData,
		kMissingSkinPartition,
		kMissingRootParent,
		kRootParentMismatch,
		kRootOutsideSkeleton,
		kInvalidBoneCount,
		kMissingBoneArray,
		kBoneMismatch,
		kBoneOutsideSkeleton
	};

	enum class GeometryCloneStatus : std::uint8_t
	{
		kNotAttempted,
		kCloned,
		kInvalidInput,
		kInvalidTransform,
		kEngineCloneFailed,
		kWrongCloneType,
		kCloneAlreadyParented,
		kMissingShaderProperty,
		kSharedShaderProperty,
		kAlphaPropertyFailed,
		kInvalidSkinBinding,
		kUnexpectedFailure
	};

	struct GeometryCloneResult
	{
		GeometryCloneStatus status{ GeometryCloneStatus::kNotAttempted };
		SkinBindingStatus bindingStatus{ SkinBindingStatus::kNotChecked };
		RE::NiPointer<RE::BSGeometry> geometry;
	};

	class ISkinBindingValidator
	{
	public:
		virtual ~ISkinBindingValidator() = default;

		[[nodiscard]] virtual SkinBindingStatus Validate(
			RE::BSGeometry& a_source,
			RE::BSGeometry& a_clone,
			RE::NiNode& a_skeletonRoot) const noexcept = 0;
	};

	class SkinBindingValidator final : public ISkinBindingValidator
	{
	public:
		[[nodiscard]] SkinBindingStatus Validate(
			RE::BSGeometry& a_source,
			RE::BSGeometry& a_clone,
			RE::NiNode& a_skeletonRoot) const noexcept override;
	};

	class ISkinnedGeometryCloner
	{
	public:
		virtual ~ISkinnedGeometryCloner() = default;

		[[nodiscard]] virtual GeometryCloneResult Clone(
			RE::BSGeometry& a_source,
			RE::NiNode& a_skeletonRoot) const = 0;
	};

	class SkinnedGeometryCloner final : public ISkinnedGeometryCloner
	{
	public:
		explicit SkinnedGeometryCloner(
			const ISkinBindingValidator& a_validator) noexcept;

		[[nodiscard]] GeometryCloneResult Clone(
			RE::BSGeometry& a_source,
			RE::NiNode& a_skeletonRoot) const override;

	private:
		const ISkinBindingValidator& validator_;
	};
}
