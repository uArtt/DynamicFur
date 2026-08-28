#pragma once

#include "DynamicFur/Game/BodyGeometryLocator.h"
#include "DynamicFur/Game/SkinnedGeometryCloner.h"

#include "RE/N/NiNode.h"
#include "RE/N/NiSmartPointer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class Actor;
}

namespace DynamicFur::Game
{
	enum class CarrierState : std::uint8_t
	{
		kDetached,
		kWaitingFor3D,
		kWaitingForNakedBody,
		kUnsupportedPerspective,
		kSourceRejected,
		kCloneRejected,
		kBuildRejected,
		kMarkerConflict,
		kAttachedHidden,
		kRendererReady,
		kDetachFailed
	};

	struct CarrierConfig
	{
		Perspective perspective{ Perspective::kThirdPerson };
		std::string markerName{ "DynamicFurV2_Carrier_3P" };
		std::size_t maximumGeometryCount{ 128 };
	};

	struct CarrierUpdateResult
	{
		CarrierState state{ CarrierState::kDetached };
		BodyGeometryStatus bodyStatus{ BodyGeometryStatus::kInvalidQuery };
		GeometryCloneStatus cloneStatus{ GeometryCloneStatus::kNotAttempted };
		SkinBindingStatus bindingStatus{ SkinBindingStatus::kNotChecked };
		std::uint64_t generation{};
		std::size_t geometryCount{};
		bool changed{};
	};

	// All methods that mutate the scene graph must be called on Skyrim's game
	// thread. The class deliberately performs no internal task scheduling.
	class Carrier final
	{
	public:
		Carrier(
			const IBodyGeometryLocator& a_locator,
			const ISkinnedGeometryCloner& a_cloner,
			CarrierConfig a_config = {});

		~Carrier();

		Carrier(const Carrier&) = delete;
		Carrier(Carrier&&) = delete;
		Carrier& operator=(const Carrier&) = delete;
		Carrier& operator=(Carrier&&) = delete;

		[[nodiscard]] CarrierUpdateResult Ensure(RE::Actor* a_actor);

		// Detaches only when the currently observed root proves that boundRoot_
		// is still alive. Otherwise it keeps the owned carrier hidden.
		[[nodiscard]] CarrierUpdateResult Detach(RE::Actor* a_actor) noexcept;

		// Use only after an object-unloaded/pre-load lifecycle signal proves the
		// old actor 3D is no longer usable.
		void AbandonReleased3D() noexcept;

		// A generation token prevents a renderer from revealing a replacement
		// carrier using stale preparation work.
		[[nodiscard]] bool SetRendererReady(
			std::uint64_t a_generation,
			bool a_ready = true) noexcept;

		[[nodiscard]] CarrierState State() const noexcept;
		[[nodiscard]] std::uint64_t Generation() const noexcept;
		[[nodiscard]] bool IsAttached() const noexcept;
		[[nodiscard]] RE::NiNode* Root() const noexcept;
		// Borrowed actor skeleton identity. Valid only while IsAttached() is true.
		[[nodiscard]] RE::NiNode* SkeletonRoot() const noexcept;
		[[nodiscard]] std::span<const RE::NiPointer<RE::BSGeometry>> Geometries()
			const noexcept;

	private:
		struct SourceGeometryIdentity
		{
			RE::BSGeometry* geometry{};
			const void* rendererData{};
			const void* skinInstance{};
		};

		[[nodiscard]] RE::NiNode* CurrentRoot(RE::Actor* a_actor) const noexcept;
		[[nodiscard]] bool MatchesObservedSource(
			const LocatedBodyGeometry& a_body) const noexcept;
		[[nodiscard]] CarrierUpdateResult Result(
			bool a_changed,
			BodyGeometryStatus a_bodyStatus,
			GeometryCloneStatus a_cloneStatus =
				GeometryCloneStatus::kNotAttempted,
			SkinBindingStatus a_bindingStatus =
				SkinBindingStatus::kNotChecked) const noexcept;

		void HideOwned() noexcept;
		void ReleaseOwned() noexcept;

		const IBodyGeometryLocator& locator_;
		const ISkinnedGeometryCloner& cloner_;
		CarrierConfig config_;

		// Raw by design: retaining the skeleton would keep obsolete actor 3D
		// alive. It is used as an identity token and is dereferenced only after
		// CurrentRoot() returns the same pointer.
		RE::NiNode* boundRoot_{};
		std::vector<RE::NiAVObject*> sourcePartTokens_;
		RE::NiPointer<RE::NiNode> carrierRoot_;
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries_;
		std::vector<SourceGeometryIdentity> sourceGeometryTokens_;
		std::uint64_t generation_{};
		CarrierState state_{ CarrierState::kDetached };
		bool rendererReady_{};
	};
}
