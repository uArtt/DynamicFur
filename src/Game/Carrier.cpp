#include "PCH.h"

#include "DynamicFur/Game/Carrier.h"

#include "RE/B/BSVisit.h"

#include <limits>
#include <unordered_set>
#include <utility>

namespace DynamicFur::Game
{
	namespace
	{
		inline constexpr std::size_t kMaximumSkeletonDepth = 512;
		inline constexpr std::uint32_t kMaximumSaneBoneCount = 512;

		[[nodiscard]] bool HasLiveSkinBindings(
			std::span<const RE::NiPointer<RE::BSGeometry>> a_geometries,
			RE::NiNode& a_skeletonRoot) noexcept
		{
			try {
				std::unordered_set<const RE::NiAVObject*> liveObjects;
				liveObjects.reserve(1024);
				RE::BSVisit::TraverseScenegraphObjects(
					std::addressof(a_skeletonRoot),
					[&liveObjects](RE::NiAVObject* a_object) {
						if (a_object) {
							liveObjects.insert(a_object);
						}
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				auto* ancestor = a_skeletonRoot.parent;
				for (std::size_t depth = 0;
					ancestor && depth < kMaximumSkeletonDepth;
					++depth, ancestor = ancestor->parent) {
					liveObjects.insert(ancestor);
				}

				for (const auto& geometry : a_geometries) {
					if (!geometry) {
						return false;
					}
					auto* skin =
						geometry->GetGeometryRuntimeData().skinInstance.get();
					if (!skin || !skin->skinData || !skin->skinPartition ||
						!skin->rootParent || !skin->bones ||
						!liveObjects.contains(skin->rootParent)) {
						return false;
					}

					const auto boneCount = skin->skinData->GetBoneCount();
					if (boneCount == 0 || boneCount > kMaximumSaneBoneCount) {
						return false;
					}
					for (std::uint32_t index = 0; index < boneCount; ++index) {
						if (!skin->bones[index] ||
							!liveObjects.contains(skin->bones[index])) {
							return false;
						}
					}
				}
				return true;
			} catch (...) {
				return false;
			}
		}
	}

	Carrier::Carrier(
		const IBodyGeometryLocator& a_locator,
		const ISkinnedGeometryCloner& a_cloner,
		CarrierConfig a_config) :
		locator_(a_locator),
		cloner_(a_cloner),
		config_(std::move(a_config))
	{}

	Carrier::~Carrier()
	{
		// boundRoot_ may already be invalid during process teardown. The
		// controller detaches during normal lifecycle transitions.
		HideOwned();
		geometries_.clear();
		sourceGeometryTokens_.clear();
		sourcePartTokens_.clear();
		carrierRoot_.reset();
		boundRoot_ = nullptr;
	}

	RE::NiNode* Carrier::CurrentRoot(RE::Actor* a_actor) const noexcept
	{
		if (!a_actor || config_.perspective != Perspective::kThirdPerson) {
			return nullptr;
		}

		const auto& bipedPointer = a_actor->GetBiped(false);
		auto* biped = bipedPointer.get();
		return biped ? biped->root : nullptr;
	}

	bool Carrier::MatchesObservedSource(
		const LocatedBodyGeometry& a_body) const noexcept
	{
		if (a_body.skeletonRoot != boundRoot_ ||
			a_body.sourceParts.size() != sourcePartTokens_.size() ||
			a_body.geometries.size() != sourceGeometryTokens_.size()) {
			return false;
		}
		for (std::size_t index = 0; index < a_body.sourceParts.size(); ++index) {
			if (!a_body.sourceParts[index] ||
				a_body.sourceParts[index].get() != sourcePartTokens_[index]) {
				return false;
			}
		}

		for (std::size_t index = 0; index < a_body.geometries.size(); ++index) {
			const auto& geometry = a_body.geometries[index];
			const auto& token = sourceGeometryTokens_[index];
			if (!geometry || geometry.get() != token.geometry) {
				return false;
			}
			const auto& runtime = geometry->GetGeometryRuntimeData();
			if (runtime.rendererData != token.rendererData ||
				runtime.skinInstance.get() != token.skinInstance) {
				return false;
			}
		}
		return true;
	}

	CarrierUpdateResult Carrier::Result(
		bool a_changed,
		BodyGeometryStatus a_bodyStatus,
		GeometryCloneStatus a_cloneStatus,
		SkinBindingStatus a_bindingStatus) const noexcept
	{
		CarrierUpdateResult result;
		result.state = state_;
		result.bodyStatus = a_bodyStatus;
		result.cloneStatus = a_cloneStatus;
		result.bindingStatus = a_bindingStatus;
		result.generation = generation_;
		result.geometryCount = geometries_.size();
		result.changed = a_changed;
		return result;
	}

	void Carrier::HideOwned() noexcept
	{
		if (carrierRoot_) {
			carrierRoot_->SetAppCulled(true);
		}

		for (const auto& geometry : geometries_) {
			if (geometry) {
				geometry->SetAppCulled(true);
			}
		}

		rendererReady_ = false;
	}

	void Carrier::ReleaseOwned() noexcept
	{
		const bool hadOwnedState =
			carrierRoot_.get() != nullptr ||
			boundRoot_ != nullptr ||
			!geometries_.empty();

		HideOwned();
		geometries_.clear();
		sourceGeometryTokens_.clear();
		sourcePartTokens_.clear();
		carrierRoot_.reset();
		boundRoot_ = nullptr;

		if (hadOwnedState) {
			++generation_;
		}
	}

	CarrierUpdateResult Carrier::Ensure(RE::Actor* a_actor)
	{
		if (config_.perspective != Perspective::kThirdPerson) {
			HideOwned();
			state_ = CarrierState::kUnsupportedPerspective;
			return Result(false, BodyGeometryStatus::kUnsupportedPerspective);
		}

		if (!a_actor) {
			HideOwned();
			state_ = CarrierState::kSourceRejected;
			return Result(false, BodyGeometryStatus::kInvalidQuery);
		}

		if (config_.markerName.empty() || config_.maximumGeometryCount == 0) {
			HideOwned();
			state_ = CarrierState::kBuildRejected;
			return Result(false, BodyGeometryStatus::kInvalidQuery);
		}

		auto* currentRoot = CurrentRoot(a_actor);
		if (!currentRoot) {
			HideOwned();
			state_ = CarrierState::kWaitingFor3D;
			return Result(false, BodyGeometryStatus::kActor3DUnavailable);
		}

		bool changed = false;

		if (carrierRoot_) {
			bool geometryTreeIntact = !geometries_.empty();
			for (const auto& geometry : geometries_) {
				if (!geometry || geometry->parent != carrierRoot_.get()) {
					geometryTreeIntact = false;
					break;
				}
			}

			bool sourceStillCompatible = false;
			BodyGeometryStatus observedStatus = BodyGeometryStatus::kInvalidQuery;
			if (boundRoot_ == currentRoot &&
				carrierRoot_->parent == currentRoot &&
				geometryTreeIntact &&
				HasLiveSkinBindings(Geometries(), *currentRoot)) {
				try {
					const auto observed = locator_.Locate(BodyGeometryQuery{
						.actor = a_actor,
						.perspective = config_.perspective
					});
					observedStatus = observed.status;
					sourceStillCompatible =
						(observed.status == BodyGeometryStatus::kFound &&
						 MatchesObservedSource(observed.body)) ||
						observed.status == BodyGeometryStatus::kBodyCovered ||
						observed.status == BodyGeometryStatus::kBodySlotUnavailable;
				} catch (...) {
					sourceStillCompatible = false;
				}
			}

			if (sourceStillCompatible) {
				state_ = rendererReady_ ?
					CarrierState::kRendererReady : CarrierState::kAttachedHidden;
				return Result(
					false,
					observedStatus,
					GeometryCloneStatus::kCloned,
					SkinBindingStatus::kValid);
			}

			HideOwned();
			if (boundRoot_ == currentRoot) {
				if (carrierRoot_->parent == currentRoot) {
					currentRoot->DetachChild(carrierRoot_.get());
					if (carrierRoot_->parent == currentRoot) {
						state_ = CarrierState::kDetachFailed;
						return Result(false, BodyGeometryStatus::kFound);
					}
				} else if (carrierRoot_->parent) {
					state_ = CarrierState::kDetachFailed;
					return Result(false, BodyGeometryStatus::kFound);
				}
			}

			// A different current root proves only that the old scene graph must
			// not be touched. Drop our ownership without dereferencing its token.
			ReleaseOwned();
			changed = true;
		}

		BodyGeometryResult located;
		try {
			located = locator_.Locate(BodyGeometryQuery{
				.actor = a_actor,
				.perspective = config_.perspective
			});
		} catch (...) {
			state_ = CarrierState::kSourceRejected;
			return Result(changed, BodyGeometryStatus::kInvalidQuery);
		}

		if (located.status != BodyGeometryStatus::kFound) {
			switch (located.status) {
			case BodyGeometryStatus::kActor3DUnavailable:
				state_ = CarrierState::kWaitingFor3D;
				break;
			case BodyGeometryStatus::kBodySlotUnavailable:
			case BodyGeometryStatus::kBodyCovered:
				state_ = CarrierState::kWaitingForNakedBody;
				break;
			case BodyGeometryStatus::kUnsupportedPerspective:
				state_ = CarrierState::kUnsupportedPerspective;
				break;
			default:
				state_ = CarrierState::kSourceRejected;
				break;
			}
			return Result(changed, located.status);
		}

		const auto sourceCount = located.body.geometries.size();
		if (located.body.skeletonRoot != currentRoot ||
			CurrentRoot(a_actor) != currentRoot ||
			sourceCount == 0 ||
			sourceCount > config_.maximumGeometryCount ||
			sourceCount > std::numeric_limits<std::uint16_t>::max()) {
			state_ = CarrierState::kBuildRejected;
			return Result(changed, BodyGeometryStatus::kFound);
		}

		try {
			const RE::BSFixedString marker{ config_.markerName };
			if (currentRoot->GetObjectByName(marker)) {
				state_ = CarrierState::kMarkerConflict;
				return Result(changed, BodyGeometryStatus::kFound);
			}

			RE::NiPointer<RE::NiNode> candidate{
				RE::NiNode::Create(static_cast<std::uint16_t>(sourceCount))
			};
			if (!candidate) {
				state_ = CarrierState::kBuildRejected;
				return Result(changed, BodyGeometryStatus::kFound);
			}

			candidate->name = marker;
			candidate->local = RE::NiTransform{};
			candidate->collisionObject.reset();
			candidate->SetAppCulled(true);

			std::vector<RE::NiPointer<RE::BSGeometry>> candidateGeometries;
			std::vector<SourceGeometryIdentity> candidateSourceTokens;
			candidateGeometries.reserve(sourceCount);
			candidateSourceTokens.reserve(sourceCount);
			for (const auto& source : located.body.geometries) {
				if (!source) {
					state_ = CarrierState::kCloneRejected;
					return Result(changed, BodyGeometryStatus::kFound,
						GeometryCloneStatus::kInvalidInput);
				}
				const auto& sourceRuntime = source->GetGeometryRuntimeData();
				candidateSourceTokens.push_back({
					source.get(),
					sourceRuntime.rendererData,
					sourceRuntime.skinInstance.get()
				});

				GeometryCloneResult clone;
				try {
					clone = cloner_.Clone(*source, *currentRoot);
				} catch (...) {
					state_ = CarrierState::kCloneRejected;
					return Result(changed, BodyGeometryStatus::kFound,
						GeometryCloneStatus::kUnexpectedFailure);
				}

				if (clone.status != GeometryCloneStatus::kCloned || !clone.geometry) {
					state_ = CarrierState::kCloneRejected;
					return Result(changed, BodyGeometryStatus::kFound,
						clone.status, clone.bindingStatus);
				}

				auto* geometry = clone.geometry.get();
				candidate->AttachChild(geometry);
				if (geometry->parent != candidate.get()) {
					state_ = CarrierState::kBuildRejected;
					return Result(changed, BodyGeometryStatus::kFound,
						GeometryCloneStatus::kCloneAlreadyParented,
						clone.bindingStatus);
				}
				candidateGeometries.push_back(clone.geometry);
			}

			currentRoot->AttachChild(candidate.get());
			if (candidate->parent != currentRoot) {
				state_ = CarrierState::kBuildRejected;
				return Result(changed, BodyGeometryStatus::kFound,
					GeometryCloneStatus::kCloned, SkinBindingStatus::kValid);
			}

			RE::NiUpdateData updateData{};
			updateData.flags.set(RE::NiUpdateData::Flag::kDirty);
			candidate->Update(updateData);

			boundRoot_ = currentRoot;
			sourcePartTokens_.clear();
			sourcePartTokens_.reserve(located.body.sourceParts.size());
			for (const auto& sourcePart : located.body.sourceParts) {
				sourcePartTokens_.push_back(sourcePart.get());
			}
			carrierRoot_ = std::move(candidate);
			geometries_ = std::move(candidateGeometries);
			sourceGeometryTokens_ = std::move(candidateSourceTokens);
			rendererReady_ = false;
			++generation_;
			state_ = CarrierState::kAttachedHidden;
			return Result(true, BodyGeometryStatus::kFound,
				GeometryCloneStatus::kCloned, SkinBindingStatus::kValid);
		} catch (...) {
			state_ = CarrierState::kBuildRejected;
			return Result(changed, BodyGeometryStatus::kFound,
				GeometryCloneStatus::kUnexpectedFailure);
		}
	}

	CarrierUpdateResult Carrier::Detach(RE::Actor* a_actor) noexcept
	{
		if (!carrierRoot_) {
			boundRoot_ = nullptr;
			sourcePartTokens_.clear();
			geometries_.clear();
			sourceGeometryTokens_.clear();
			rendererReady_ = false;
			state_ = CarrierState::kDetached;
			return Result(false, BodyGeometryStatus::kInvalidQuery);
		}

		HideOwned();
		auto* currentRoot = CurrentRoot(a_actor);
		if (!currentRoot) {
			state_ = CarrierState::kDetachFailed;
			return Result(false, BodyGeometryStatus::kActor3DUnavailable);
		}

		if (currentRoot != boundRoot_) {
			ReleaseOwned();
			state_ = CarrierState::kDetached;
			return Result(true, BodyGeometryStatus::kFound);
		}

		if (!carrierRoot_->parent) {
			ReleaseOwned();
			state_ = CarrierState::kDetached;
			return Result(true, BodyGeometryStatus::kFound);
		}

		if (carrierRoot_->parent != currentRoot) {
			state_ = CarrierState::kDetachFailed;
			return Result(false, BodyGeometryStatus::kFound);
		}

		currentRoot->DetachChild(carrierRoot_.get());
		if (carrierRoot_->parent == currentRoot) {
			state_ = CarrierState::kDetachFailed;
			return Result(false, BodyGeometryStatus::kFound);
		}

		ReleaseOwned();
		state_ = CarrierState::kDetached;
		return Result(true, BodyGeometryStatus::kFound);
	}

	void Carrier::AbandonReleased3D() noexcept
	{
		ReleaseOwned();
		state_ = CarrierState::kDetached;
	}

	bool Carrier::SetRendererReady(std::uint64_t a_generation, bool a_ready) noexcept
	{
		if (a_generation != generation_ || !carrierRoot_ || !boundRoot_) {
			return false;
		}

		bool geometryTreeIntact = !geometries_.empty();
		for (const auto& geometry : geometries_) {
			if (!geometry || geometry->parent != carrierRoot_.get()) {
				geometryTreeIntact = false;
				break;
			}
		}

		if (carrierRoot_->parent != boundRoot_ || !geometryTreeIntact) {
			HideOwned();
			state_ = CarrierState::kDetachFailed;
			return false;
		}

		if (a_ready) {
			for (const auto& geometry : geometries_) {
				geometry->SetAppCulled(false);
			}
			carrierRoot_->SetAppCulled(false);
			state_ = CarrierState::kRendererReady;
		} else {
			HideOwned();
			state_ = CarrierState::kAttachedHidden;
		}

		rendererReady_ = a_ready;
		return true;
	}

	CarrierState Carrier::State() const noexcept { return state_; }
	std::uint64_t Carrier::Generation() const noexcept { return generation_; }

	bool Carrier::IsAttached() const noexcept
	{
		return carrierRoot_ && boundRoot_ && carrierRoot_->parent == boundRoot_;
	}

	RE::NiNode* Carrier::Root() const noexcept { return carrierRoot_.get(); }
	RE::NiNode* Carrier::SkeletonRoot() const noexcept { return boundRoot_; }

	std::span<const RE::NiPointer<RE::BSGeometry>> Carrier::Geometries() const noexcept
	{
		return { geometries_.data(), geometries_.size() };
	}
}
