#pragma once

#include "DynamicFur/Core/ShellProfile.h"
#include "DynamicFur/Game/ArmorBlacklist.h"
#include "DynamicFur/Game/BodyGeometryLocator.h"
#include "DynamicFur/Game/ArmorCoverageLocator.h"
#include "DynamicFur/Game/Carrier.h"
#include "DynamicFur/Game/FurSourcePolicy.h"
#include "DynamicFur/Game/SkinnedGeometryCloner.h"
#include "DynamicFur/Render/RenderBackend.h"

#include <atomic>
#include <cstdint>

namespace DynamicFur
{
	class Controller final
	{
	public:
		[[nodiscard]] static Controller& GetSingleton() noexcept;

		void Initialize();
		void QueuePlayerRefresh() noexcept;
		void QueueArmorCoverageRefresh() noexcept;
		void QueuePlayerRebuild() noexcept;
		void OnPlayer3DLoaded() noexcept;
		void OnPlayer3DReleased() noexcept;
		void OnPreLoadGame() noexcept;
		void OnGameReady() noexcept;

		Controller(const Controller&) = delete;
		Controller(Controller&&) = delete;
		Controller& operator=(const Controller&) = delete;
		Controller& operator=(Controller&&) = delete;

	private:
		Controller();

		void QueueRefreshTask() noexcept;
		void ScheduleRetry() noexcept;
		[[nodiscard]] bool SchedulePreparationRetry() noexcept;
		void RefreshPlayer() noexcept;
		[[nodiscard]] bool PrepareCarrier(
			std::uint64_t a_generation,
			std::uint64_t a_coverageRevision) noexcept;

		Core::ShellProfile profile_{ Core::ShellProfile::Legacy17() };
		Game::FurSourcePolicy furSourcePolicy_;
		Game::ArmorBlacklist armorBlacklist_;
		Game::BodyGeometryLocator bodyLocator_;
		Game::ArmorCoverageLocator armorCoverageLocator_;
		Game::SkinBindingValidator bindingValidator_;
		Game::SkinnedGeometryCloner geometryCloner_;
		Game::Carrier carrier_;
		Render::CarrierRegistry carrierRegistry_;
		Render::RenderRouter renderRouter_;

		std::atomic_bool initialized_{};
		std::atomic_bool suspended_{};
		std::atomic_bool gameLoadInProgress_{};
		std::atomic_bool rebuildRequested_{};
		std::atomic_uint64_t coverageRevision_{ 1 };
		std::atomic_uint64_t lifecycleEpoch_{ 1 };
		std::atomic_uint64_t queuedEpoch_{};
		std::atomic_uint32_t retryBudget_{};
		std::atomic_uint8_t preparationRetryBudget_{};
		std::uint64_t preparedGeneration_{};
		std::uint64_t preparedCoverageRevision_{};
		std::uint64_t failedPreparationGeneration_{};
		std::uint64_t failedPreparationCoverageRevision_{};
		std::uint8_t lastReportedState_{ 0xFF };
		std::uint8_t lastReportedFurSourceStatus_{ 0xFF };
		std::uint32_t lastReportedSkinFormID_{};
		std::uint32_t lastReportedMarkedArmorCount_{ 0xFFFFFFFFU };
	};
}
