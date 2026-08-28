#include "DynamicFur/Render/RenderHooks.h"

#include "RE/B/BSLightingShader.h"
#include "RE/B/BSRenderPass.h"
#include "RE/B/BSShader.h"
#include "RE/B/BSUtilityShader.h"
#include "RE/N/NiAlphaProperty.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <array>
#include <optional>
#include <string_view>

namespace DynamicFur::Render
{
	namespace
	{
		thread_local bool insideIndexedDrawHook{};

		class PassScopeStack final
		{
		public:
			void Push(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags) noexcept
			{
				if (!a_pass || depth_ == scopes_.size()) {
					Reset();
					return;
				}
				scopes_[depth_].emplace(
					nullptr,
					a_pass,
					a_pass->passEnum,
					UsesAlphaTesting(a_pass),
					a_renderFlags);
				handled_[depth_] = false;
				++depth_;
			}

			void MarkHandled(RE::BSRenderPass* a_pass) noexcept
			{
				const auto* current = ScopedPassContext::Current();
				if (depth_ != 0 && current && current->pass == a_pass) {
					handled_[depth_ - 1] = true;
				}
			}

			[[nodiscard]] bool WasHandled(RE::BSRenderPass* a_pass) const noexcept
			{
				const auto* current = ScopedPassContext::Current();
				return depth_ != 0 && current && current->pass == a_pass &&
					handled_[depth_ - 1];
			}

			// Shader overhauls commonly chain SetupGeometry but leave
			// RestoreGeometry alone. If a later hook failed to forward our setup
			// thunk, RestoreGeometry still contains the complete BSRenderPass and
			// runs while its pipeline is live. Rebuild that one missing scope here.
			[[nodiscard]] bool EnsureForRestore(
				RE::BSRenderPass* a_pass,
				std::uint32_t a_renderFlags) noexcept
			{
				const auto* current = ScopedPassContext::Current();
				if (depth_ != 0 && current && current->pass == a_pass) {
					return false;
				}

				Reset();
				Push(a_pass, a_renderFlags);
				current = ScopedPassContext::Current();
				return depth_ != 0 && current && current->pass == a_pass;
			}

			void Pop(RE::BSRenderPass* a_pass) noexcept
			{
				const auto* current = ScopedPassContext::Current();
				if (depth_ == 0 || !current || current->pass != a_pass) {
					Reset();
					return;
				}
				--depth_;
				scopes_[depth_].reset();
				handled_[depth_] = false;
			}

			void Reset() noexcept
			{
				while (depth_ != 0) {
					--depth_;
					scopes_[depth_].reset();
					handled_[depth_] = false;
				}
			}

		private:
			[[nodiscard]] static bool UsesAlphaTesting(
				RE::BSRenderPass* a_pass) noexcept
			{
				if (!a_pass || !a_pass->geometry) {
					return false;
				}
				auto* alpha =
					a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
				return alpha && alpha->GetAlphaTesting();
			}

			std::array<std::optional<ScopedPassContext>, 16> scopes_;
			std::array<bool, 16> handled_{};
			std::size_t depth_{};
		};

		thread_local PassScopeStack shaderPassScopes;

		class ReentrancyGuard final
		{
		public:
			ReentrancyGuard() noexcept { insideIndexedDrawHook = true; }
			~ReentrancyGuard() noexcept { insideIndexedDrawHook = false; }
		};
	}

	RenderHooks& RenderHooks::GetSingleton() noexcept
	{
		static RenderHooks singleton;
		return singleton;
	}

	bool RenderHooks::InstallPassScopes()
	{
		std::scoped_lock lock(installMutex_);
		if (passScopesInstalled_.load(std::memory_order_acquire)) {
			return true;
		}

		REL::Relocation<std::uintptr_t> lightingVtable{
			RE::VTABLE_BSLightingShader[0]
		};
		REL::Relocation<std::uintptr_t> utilityVtable{
			RE::VTABLE_BSUtilityShader[0]
		};

		auto* lightingTable = reinterpret_cast<std::uintptr_t*>(lightingVtable.address());
		auto* utilityTable = reinterpret_cast<std::uintptr_t*>(utilityVtable.address());
		if (!lightingTable || !utilityTable) {
			return false;
		}

		const auto lightingSetup =
			reinterpret_cast<ShaderGeometryFn>(lightingTable[0x6]);
		const auto lightingRestore =
			reinterpret_cast<ShaderGeometryFn>(lightingTable[0x7]);
		const auto utilitySetup =
			reinterpret_cast<ShaderGeometryFn>(utilityTable[0x6]);
		const auto utilityRestore =
			reinterpret_cast<ShaderGeometryFn>(utilityTable[0x7]);
		if (!lightingSetup || !lightingRestore || !utilitySetup || !utilityRestore ||
			lightingTable[0x6] == reinterpret_cast<std::uintptr_t>(&LightingSetupGeometryThunk) ||
			lightingTable[0x7] == reinterpret_cast<std::uintptr_t>(&LightingRestoreGeometryThunk) ||
			utilityTable[0x6] == reinterpret_cast<std::uintptr_t>(&UtilitySetupGeometryThunk) ||
			utilityTable[0x7] == reinterpret_cast<std::uintptr_t>(&UtilityRestoreGeometryThunk)) {
			return false;
		}

		lightingSetupGeometry_.store(lightingSetup, std::memory_order_release);
		lightingRestoreGeometry_.store(lightingRestore, std::memory_order_release);
		utilitySetupGeometry_.store(utilitySetup, std::memory_order_release);
		utilityRestoreGeometry_.store(utilityRestore, std::memory_order_release);
		lightingVtable_ = lightingTable;
		utilityVtable_ = utilityTable;

		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(lightingTable + 0x6),
			reinterpret_cast<std::uintptr_t>(&LightingSetupGeometryThunk));
		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(lightingTable + 0x7),
			reinterpret_cast<std::uintptr_t>(&LightingRestoreGeometryThunk));
		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(utilityTable + 0x6),
			reinterpret_cast<std::uintptr_t>(&UtilitySetupGeometryThunk));
		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(utilityTable + 0x7),
			reinterpret_cast<std::uintptr_t>(&UtilityRestoreGeometryThunk));
		passScopesInstalled_.store(true, std::memory_order_release);
		return true;
	}

	bool RenderHooks::UninstallPassScopes()
	{
		std::scoped_lock lock(installMutex_);
		if (!passScopesInstalled_.load(std::memory_order_acquire) ||
			!lightingVtable_ || !utilityVtable_) {
			return false;
		}

		if (lightingVtable_[0x6] !=
				reinterpret_cast<std::uintptr_t>(&LightingSetupGeometryThunk) ||
			lightingVtable_[0x7] !=
				reinterpret_cast<std::uintptr_t>(&LightingRestoreGeometryThunk) ||
			utilityVtable_[0x6] !=
				reinterpret_cast<std::uintptr_t>(&UtilitySetupGeometryThunk) ||
			utilityVtable_[0x7] !=
				reinterpret_cast<std::uintptr_t>(&UtilityRestoreGeometryThunk)) {
			return false;
		}

		const auto lightingSetup =
			lightingSetupGeometry_.load(std::memory_order_acquire);
		const auto lightingRestore =
			lightingRestoreGeometry_.load(std::memory_order_acquire);
		const auto utilitySetup =
			utilitySetupGeometry_.load(std::memory_order_acquire);
		const auto utilityRestore =
			utilityRestoreGeometry_.load(std::memory_order_acquire);
		if (!lightingSetup || !lightingRestore || !utilitySetup || !utilityRestore) {
			return false;
		}

		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(utilityVtable_ + 0x7),
			reinterpret_cast<std::uintptr_t>(utilityRestore));
		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(utilityVtable_ + 0x6),
			reinterpret_cast<std::uintptr_t>(utilitySetup));
		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(lightingVtable_ + 0x7),
			reinterpret_cast<std::uintptr_t>(lightingRestore));
		REL::safe_write(
			reinterpret_cast<std::uintptr_t>(lightingVtable_ + 0x6),
			reinterpret_cast<std::uintptr_t>(lightingSetup));

		passScopesInstalled_.store(false, std::memory_order_release);
		lightingSetupGeometry_.store(nullptr, std::memory_order_release);
		lightingRestoreGeometry_.store(nullptr, std::memory_order_release);
		utilitySetupGeometry_.store(nullptr, std::memory_order_release);
		utilityRestoreGeometry_.store(nullptr, std::memory_order_release);
		lightingVtable_ = nullptr;
		utilityVtable_ = nullptr;
		return true;
	}

	void RenderHooks::LightingSetupGeometryThunk(
		RE::BSShader* a_shader,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_renderFlags)
	{
		auto& hooks = GetSingleton();
		const auto next = hooks.lightingSetupGeometry_.load(std::memory_order_acquire);
		if (next) {
			next(a_shader, a_pass, a_renderFlags);
		}
		shaderPassScopes.Push(a_pass, a_renderFlags);
	}

	void RenderHooks::LightingRestoreGeometryThunk(
		RE::BSShader* a_shader,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_renderFlags)
	{
		auto& hooks = GetSingleton();
		if (shaderPassScopes.EnsureForRestore(a_pass, a_renderFlags) &&
			!hooks.reportedRecoveredPassScope_.exchange(
				true, std::memory_order_acq_rel)) {
			try {
				SKSE::log::info(
					"Recovered a missing shader setup scope from RestoreGeometry");
			} catch (...) {
				// Rendering must never depend on diagnostic I/O.
			}
		}
		if (!shaderPassScopes.WasHandled(a_pass)) {
			hooks.DrawPassFallback(a_pass);
		}
		shaderPassScopes.Pop(a_pass);
		const auto next = hooks.lightingRestoreGeometry_.load(std::memory_order_acquire);
		if (next) {
			next(a_shader, a_pass, a_renderFlags);
		}
	}

	void RenderHooks::UtilitySetupGeometryThunk(
		RE::BSShader* a_shader,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_renderFlags)
	{
		auto& hooks = GetSingleton();
		const auto next = hooks.utilitySetupGeometry_.load(std::memory_order_acquire);
		if (next) {
			next(a_shader, a_pass, a_renderFlags);
		}
		shaderPassScopes.Push(a_pass, a_renderFlags);
	}

	void RenderHooks::UtilityRestoreGeometryThunk(
		RE::BSShader* a_shader,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_renderFlags)
	{
		auto& hooks = GetSingleton();
		if (shaderPassScopes.EnsureForRestore(a_pass, a_renderFlags) &&
			!hooks.reportedRecoveredPassScope_.exchange(
				true, std::memory_order_acq_rel)) {
			try {
				SKSE::log::info(
					"Recovered a missing shader setup scope from RestoreGeometry");
			} catch (...) {
				// Rendering must never depend on diagnostic I/O.
			}
		}
		if (!shaderPassScopes.WasHandled(a_pass)) {
			hooks.DrawPassFallback(a_pass);
		}
		shaderPassScopes.Pop(a_pass);
		const auto next = hooks.utilityRestoreGeometry_.load(std::memory_order_acquire);
		if (next) {
			next(a_shader, a_pass, a_renderFlags);
		}
	}

	bool RenderHooks::InstallDrawHooks(
		REX::W32::ID3D11DeviceContext* a_immediateContext,
		RenderRouter& a_router)
	{
		if (!a_immediateContext) {
			return false;
		}

		std::scoped_lock lock(installMutex_);
		auto* installedRouter = router_.load(std::memory_order_acquire);
		if (installedRouter && installedRouter != std::addressof(a_router)) {
			return false;
		}
		router_.store(std::addressof(a_router), std::memory_order_release);
		if (!InstallDrawContextLocked(a_immediateContext)) {
			router_.store(installedRouter, std::memory_order_release);
			return false;
		}
		context_.store(a_immediateContext, std::memory_order_release);
		installed_.store(true, std::memory_order_release);
		return true;
	}

	bool RenderHooks::InstallAdditionalDrawContext(
		REX::W32::ID3D11DeviceContext* a_immediateContext)
	{
		if (!a_immediateContext || !router_.load(std::memory_order_acquire)) {
			return false;
		}

		std::scoped_lock lock(installMutex_);
		if (!InstallDrawContextLocked(a_immediateContext)) {
			return false;
		}
		installed_.store(true, std::memory_order_release);
		return true;
	}

	bool RenderHooks::InstallDrawContextLocked(
		REX::W32::ID3D11DeviceContext* a_immediateContext)
	{
		AcceptedDrawContext* acceptedRecord{};
		for (auto& candidate : acceptedDrawContexts_) {
			if (candidate.context.load(std::memory_order_acquire) ==
				a_immediateContext) {
				return true;
			}
			if (!acceptedRecord &&
				!candidate.context.load(std::memory_order_acquire)) {
				acceptedRecord = std::addressof(candidate);
			}
		}
		if (!acceptedRecord) {
			return false;
		}

		auto* vtable = *reinterpret_cast<std::uintptr_t**>(a_immediateContext);
		if (!vtable || vtable[kDrawIndexedVtableIndex] == 0 ||
			vtable[kDrawIndexedInstancedVtableIndex] == 0) {
			return false;
		}

		const auto thunkAddress = reinterpret_cast<std::uintptr_t>(&DrawIndexedThunk);
		const auto instancedThunkAddress =
			reinterpret_cast<std::uintptr_t>(&DrawIndexedInstancedThunk);
		DrawHookSite* site{};
		for (auto& candidate : drawHookSites_) {
			if (candidate.vtable.load(std::memory_order_acquire) == vtable) {
				site = std::addressof(candidate);
				break;
			}
		}
		if (!site) {
			for (auto& candidate : drawHookSites_) {
				if (!candidate.vtable.load(std::memory_order_acquire)) {
					site = std::addressof(candidate);
					break;
				}
			}
		}
		if (!site) {
			return false;
		}

		const bool newSite =
			site->vtable.load(std::memory_order_acquire) == nullptr;
		const auto currentDrawIndexed = vtable[kDrawIndexedVtableIndex];
		const auto currentDrawIndexedInstanced =
			vtable[kDrawIndexedInstancedVtableIndex];
		if (newSite &&
			(currentDrawIndexed == thunkAddress ||
			 currentDrawIndexedInstanced == instancedThunkAddress)) {
			return false;
		}

		if (newSite) {
			site->owner = a_immediateContext;
			const auto next = reinterpret_cast<DrawIndexedFn>(currentDrawIndexed);
			const auto nextInstanced = reinterpret_cast<DrawIndexedInstancedFn>(
				currentDrawIndexedInstanced);
			site->nextDrawIndexed.store(next, std::memory_order_release);
			site->nextDrawIndexedInstanced.store(
				nextInstanced, std::memory_order_release);
			site->vtable.store(vtable, std::memory_order_release);
			const auto slotAddress = reinterpret_cast<std::uintptr_t>(
				vtable + kDrawIndexedVtableIndex);
			REL::safe_write(slotAddress, thunkAddress);
			const auto instancedSlotAddress = reinterpret_cast<std::uintptr_t>(
				vtable + kDrawIndexedInstancedVtableIndex);
			REL::safe_write(instancedSlotAddress, instancedThunkAddress);

			if (vtable[kDrawIndexedVtableIndex] != thunkAddress ||
				vtable[kDrawIndexedInstancedVtableIndex] != instancedThunkAddress) {
				if (vtable[kDrawIndexedVtableIndex] == thunkAddress) {
					REL::safe_write(
						slotAddress, reinterpret_cast<std::uintptr_t>(next));
				}
				if (vtable[kDrawIndexedInstancedVtableIndex] == instancedThunkAddress) {
					REL::safe_write(
						instancedSlotAddress,
						reinterpret_cast<std::uintptr_t>(nextInstanced));
				}
				site->vtable.store(nullptr, std::memory_order_release);
				site->nextDrawIndexed.store(nullptr, std::memory_order_release);
				site->nextDrawIndexedInstanced.store(
					nullptr, std::memory_order_release);
				site->owner.Reset();
				return false;
			}

			try {
				SKSE::log::info(
					"D3D11 draw hook site registered: context {}, vtable {}, DrawIndexed next 0x{:X}, DrawIndexedInstanced next 0x{:X}",
					static_cast<const void*>(a_immediateContext),
					static_cast<const void*>(vtable),
					reinterpret_cast<std::uintptr_t>(next),
					reinterpret_cast<std::uintptr_t>(nextInstanced));
			} catch (...) {
				// Hook installation must never depend on diagnostic I/O.
			}
		} else if ((currentDrawIndexed != thunkAddress ||
				currentDrawIndexedInstanced != instancedThunkAddress) &&
			!site->reportedDisplaced.exchange(true, std::memory_order_acq_rel)) {
			try {
				SKSE::log::info(
					"D3D11 hook site {} is now headed by another renderer hook; preserving its existing chain",
					static_cast<const void*>(vtable));
			} catch (...) {
				// Hook installation must never depend on diagnostic I/O.
			}
		}

		acceptedRecord->owner = a_immediateContext;
		acceptedRecord->siteVtable.store(vtable, std::memory_order_release);
		acceptedRecord->context.store(a_immediateContext, std::memory_order_release);
		try {
			SKSE::log::info(
				"D3D11 draw context accepted: context {}, hook site {}",
				static_cast<const void*>(a_immediateContext),
				static_cast<const void*>(vtable));
		} catch (...) {
			// Hook installation must never depend on diagnostic I/O.
		}
		return true;
	}

	bool RenderHooks::UninstallDrawHooks()
	{
		std::scoped_lock lock(installMutex_);
		if (!installed_.load(std::memory_order_acquire)) {
			return false;
		}

		const auto thunkAddress = reinterpret_cast<std::uintptr_t>(&DrawIndexedThunk);
		const auto instancedThunkAddress =
			reinterpret_cast<std::uintptr_t>(&DrawIndexedInstancedThunk);
		bool removedAny{};
		for (const auto& site : drawHookSites_) {
			auto* vtable = site.vtable.load(std::memory_order_acquire);
			if (!vtable) {
				continue;
			}
			if (vtable[kDrawIndexedVtableIndex] != thunkAddress ||
				vtable[kDrawIndexedInstancedVtableIndex] != instancedThunkAddress) {
				// A hook installed above us may still retain our thunk as its next.
				// Never clear the dispatch table or overwrite that external hook.
				return false;
			}
		}
		for (auto& site : drawHookSites_) {
			auto* vtable = site.vtable.load(std::memory_order_acquire);
			if (!vtable) {
				continue;
			}
			const auto next = site.nextDrawIndexed.load(std::memory_order_acquire);
			const auto nextInstanced =
				site.nextDrawIndexedInstanced.load(std::memory_order_acquire);
			if (next) {
				REL::safe_write(
					reinterpret_cast<std::uintptr_t>(
						vtable + kDrawIndexedVtableIndex),
					reinterpret_cast<std::uintptr_t>(next));
			}
			if (nextInstanced) {
				REL::safe_write(
					reinterpret_cast<std::uintptr_t>(
						vtable + kDrawIndexedInstancedVtableIndex),
					reinterpret_cast<std::uintptr_t>(nextInstanced));
			}
			site.vtable.store(nullptr, std::memory_order_release);
			site.nextDrawIndexed.store(nullptr, std::memory_order_release);
			site.nextDrawIndexedInstanced.store(nullptr, std::memory_order_release);
			site.reportedDisplaced.store(false, std::memory_order_release);
			site.owner.Reset();
			removedAny = true;
		}
		for (auto& accepted : acceptedDrawContexts_) {
			accepted.context.store(nullptr, std::memory_order_release);
			accepted.siteVtable.store(nullptr, std::memory_order_release);
			accepted.owner.Reset();
		}

		installed_.store(false, std::memory_order_release);
		router_.store(nullptr, std::memory_order_release);
		context_.store(nullptr, std::memory_order_release);
		return removedAny;
	}

	RenderHooks::DrawIndexedDispatch RenderHooks::ResolveDrawIndexed(
		REX::W32::ID3D11DeviceContext* a_context) const noexcept
	{
		if (!a_context) {
			return {};
		}
		std::uintptr_t* vtable{};
		bool accepted{};
		for (const auto& candidate : acceptedDrawContexts_) {
			if (candidate.context.load(std::memory_order_acquire) == a_context) {
				vtable = candidate.siteVtable.load(std::memory_order_acquire);
				accepted = true;
				break;
			}
		}
		if (!vtable) {
			vtable = *reinterpret_cast<std::uintptr_t**>(a_context);
		}
		for (const auto& site : drawHookSites_) {
			if (site.vtable.load(std::memory_order_acquire) == vtable) {
				return { site.nextDrawIndexed.load(std::memory_order_acquire), accepted };
			}
		}
		return {};
	}

	RenderHooks::DrawIndexedInstancedDispatch RenderHooks::ResolveDrawIndexedInstanced(
		REX::W32::ID3D11DeviceContext* a_context) const noexcept
	{
		if (!a_context) {
			return {};
		}
		std::uintptr_t* vtable{};
		bool accepted{};
		for (const auto& candidate : acceptedDrawContexts_) {
			if (candidate.context.load(std::memory_order_acquire) == a_context) {
				vtable = candidate.siteVtable.load(std::memory_order_acquire);
				accepted = true;
				break;
			}
		}
		if (!vtable) {
			vtable = *reinterpret_cast<std::uintptr_t**>(a_context);
		}
		for (const auto& site : drawHookSites_) {
			if (site.vtable.load(std::memory_order_acquire) == vtable) {
				return {
					site.nextDrawIndexedInstanced.load(std::memory_order_acquire),
					accepted
				};
			}
		}
		return {};
	}

	void RenderHooks::SetEnabled(bool a_enabled) noexcept
	{
		enabled_.store(a_enabled, std::memory_order_release);
	}

	bool RenderHooks::IsEnabled() const noexcept
	{
		return enabled_.load(std::memory_order_acquire);
	}

	bool RenderHooks::IsInstalled() const noexcept
	{
		return installed_.load(std::memory_order_acquire);
	}

	bool RenderHooks::ArePassScopesInstalled() const noexcept
	{
		return passScopesInstalled_.load(std::memory_order_acquire);
	}

	void RenderHooks::DrawPassFallback(RE::BSRenderPass* a_pass) noexcept
	{
		if (insideIndexedDrawHook || !a_pass ||
			!enabled_.load(std::memory_order_acquire)) {
			return;
		}
		const auto* pass = ScopedPassContext::Current();
		auto* context = context_.load(std::memory_order_acquire);
		auto* router = router_.load(std::memory_order_acquire);
		if (!pass || pass->pass != a_pass || !context || !router) {
			return;
		}

		const auto prepared = router->PrepareBoundCarrier(context, *pass);
		if (!prepared.carrierMatched) {
			return;
		}
		if (!reportedPassFallbackEntry_.exchange(true, std::memory_order_acq_rel)) {
			try {
				SKSE::log::info(
					"Native carrier reached the shader-pass fallback; direct D3D interception was not required");
			} catch (...) {
				// Rendering must never depend on diagnostic I/O.
			}
		}

		const IndexedDrawArgs diagnosticDraw{
			prepared.draw.indexCount,
			prepared.draw.startIndex,
			prepared.draw.layers[0].baseVertex
		};
		ReportPipelineSnapshotOnce(context, prepared, *pass, diagnosticDraw);
		ReportCarrierDrawOnce(prepared, *pass, "shader-pass fallback");
		if (!prepared.IsReady()) {
			return;
		}

		ReentrancyGuard reentrancyGuard;
		ScopedNativeDrawState state(context, prepared.draw);
		if (!state.IsApplied()) {
			return;
		}

		// The engine-issued carrier draw immediately preceding RestoreGeometry
		// is the zero-length scheduling shell. Only add the outer layers here.
		// The direct DrawIndexed routes suppress that source draw and therefore
		// continue to emit every layer, including zero, in their own loops.
		for (std::uint32_t layer = 1; layer < prepared.draw.layerCount; ++layer) {
			const auto& layerDraw = prepared.draw.layers[layer];
			if (!state.BindLayer(layerDraw)) {
				break;
			}
			context->DrawIndexed(
				prepared.draw.indexCount,
				prepared.draw.startIndex,
				layerDraw.baseVertex);
		}
		shaderPassScopes.MarkHandled(a_pass);
	}

	void RenderHooks::ReportCarrierDrawOnce(
		const PrepareResult& a_prepared,
		const FurPassContext& a_pass,
		const char* a_command) noexcept
	{
		if (!a_prepared.carrierMatched) {
			return;
		}
		if (a_prepared.IsReady()) {
			const auto passIndex = static_cast<std::size_t>(a_pass.kind);
			if (passIndex >= reportedReadyPassKinds_.size() ||
				reportedReadyPassKinds_[passIndex].exchange(
					true, std::memory_order_acq_rel)) {
				return;
			}
			try {
				SKSE::log::info(
					"First native carrier draw accepted via {} for pass {}: {} shell(s), shader family {}, technique 0x{:08X}",
					a_command,
					static_cast<std::uint32_t>(a_pass.kind),
					a_prepared.draw.layerCount,
					static_cast<std::uint32_t>(a_pass.shaderFamily),
					a_pass.technique);
			} catch (...) {
				// Rendering must never depend on diagnostic I/O.
			}
			return;
		}
		const auto index = static_cast<std::size_t>(a_prepared.status);
		if (index >= reportedPrepareStatuses_.size() ||
			reportedPrepareStatuses_[index].exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		try {
			SKSE::log::warn(
				"Native carrier draw suppressed safely: prepare status {}, pass {}, shader family {}, technique 0x{:08X}",
				static_cast<std::uint32_t>(a_prepared.status),
				static_cast<std::uint32_t>(a_pass.kind),
				static_cast<std::uint32_t>(a_pass.shaderFamily),
				a_pass.technique);
		} catch (...) {
			// Rendering must never depend on diagnostic I/O.
		}
	}

	void RenderHooks::ReportPipelineSnapshotOnce(
		REX::W32::ID3D11DeviceContext* a_context,
		const PrepareResult& a_prepared,
		const FurPassContext& a_pass,
		const IndexedDrawArgs& a_draw) noexcept
	{
		if (!a_context || a_pass.shaderFamily != ShaderFamily::Lighting ||
			!a_pass.geometry) {
			return;
		}

		const auto* geometryNameData = a_pass.geometry->name.c_str();
		const std::string_view geometryName{
			geometryNameData ? geometryNameData : ""
		};
		const auto isCarrier = a_prepared.carrierMatched;
		const auto isOriginalBody = !isCarrier && geometryName == "3BA";
		if (!isCarrier && !isOriginalBody) {
			return;
		}

		auto& reported = isCarrier ?
			reportedCarrierPipeline_ : reportedOriginalBodyPipeline_;
		if (reported.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		try {
			REX::W32::ComPtr<REX::W32::ID3D11DepthStencilState> depthState;
			REX::W32::ComPtr<REX::W32::ID3D11BlendState> blendState;
			REX::W32::ComPtr<REX::W32::ID3D11RasterizerState> rasterizerState;
			REX::W32::ComPtr<REX::W32::ID3D11VertexShader> vertexShader;
			REX::W32::ComPtr<REX::W32::ID3D11PixelShader> pixelShader;
			REX::W32::ComPtr<REX::W32::ID3D11ShaderResourceView> diffuseView;
			REX::W32::ComPtr<REX::W32::ID3D11SamplerState> diffuseSampler;
			REX::W32::ComPtr<REX::W32::ID3D11Buffer> alphaReference;

			std::uint32_t stencilReference{};
			std::array<float, 4> blendFactor{};
			std::uint32_t sampleMask{};
			a_context->OMGetDepthStencilState(
				depthState.ReleaseAndGetAddressOf(), &stencilReference);
			a_context->OMGetBlendState(
				blendState.ReleaseAndGetAddressOf(), blendFactor.data(), &sampleMask);
			a_context->RSGetState(rasterizerState.ReleaseAndGetAddressOf());
			a_context->VSGetShader(vertexShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
			a_context->PSGetShader(pixelShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
			a_context->PSGetShaderResources(
				0, 1, diffuseView.ReleaseAndGetAddressOf());
			a_context->PSGetSamplers(
				0, 1, diffuseSampler.ReleaseAndGetAddressOf());
			a_context->PSGetConstantBuffers(
				kAlphaReferenceConstantSlot, 1,
				alphaReference.ReleaseAndGetAddressOf());

			REX::W32::D3D11_DEPTH_STENCIL_DESC depthDescription{};
			REX::W32::D3D11_BLEND_DESC blendDescription{};
			REX::W32::D3D11_RASTERIZER_DESC rasterizerDescription{};
			REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC diffuseDescription{};
			REX::W32::D3D11_SAMPLER_DESC samplerDescription{};
			REX::W32::D3D11_BUFFER_DESC alphaReferenceDescription{};
			if (depthState.Get()) {
				depthState->GetDesc(&depthDescription);
			}
			if (blendState.Get()) {
				blendState->GetDesc(&blendDescription);
			}
			if (rasterizerState.Get()) {
				rasterizerState->GetDesc(&rasterizerDescription);
			}
			if (diffuseView.Get()) {
				diffuseView->GetDesc(&diffuseDescription);
			}
			if (diffuseSampler.Get()) {
				diffuseSampler->GetDesc(&samplerDescription);
			}
			if (alphaReference.Get()) {
				alphaReference->GetDesc(&alphaReferenceDescription);
			}

			std::array<REX::W32::ID3D11RenderTargetView*, 8> renderTargets{};
			REX::W32::ID3D11DepthStencilView* depthView{};
			a_context->OMGetRenderTargets(
				static_cast<std::uint32_t>(renderTargets.size()),
				renderTargets.data(), &depthView);
			std::array<REX::W32::D3D11_RENDER_TARGET_VIEW_DESC, 8>
				renderTargetDescriptions{};
			std::array<const void*, 8> renderTargetAddresses{};
			std::uint32_t renderTargetCount{};
			for (std::size_t index = 0; index < renderTargets.size(); ++index) {
				auto* view = renderTargets[index];
				if (!view) {
					continue;
				}
				renderTargetAddresses[index] = view;
				view->GetDesc(&renderTargetDescriptions[index]);
				renderTargetCount = static_cast<std::uint32_t>(index + 1);
				view->Release();
			}
			REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
			const auto* depthViewAddress = static_cast<const void*>(depthView);
			if (depthView) {
				depthView->GetDesc(&depthViewDescription);
				depthView->Release();
			}

			std::array<REX::W32::D3D11_VIEWPORT, 16> viewports{};
			auto viewportCount = static_cast<std::uint32_t>(viewports.size());
			a_context->RSGetViewports(&viewportCount, viewports.data());
			REX::W32::D3D11_PRIMITIVE_TOPOLOGY topology{};
			a_context->IAGetPrimitiveTopology(&topology);

			auto* alpha =
				a_pass.geometry->GetGeometryRuntimeData().alphaProperty.get();
			const auto label = isCarrier ? "CARRIER" : "ORIGINAL-3BA";
			SKSE::log::info(
				"PIPELINE SNAPSHOT BEGIN [{}] geometry '{}' {} draw(indexCount {}, startIndex {}, baseVertex {})",
				label, geometryName, static_cast<const void*>(a_pass.geometry),
				a_draw.indexCount, a_draw.startIndex, a_draw.baseVertex);
			SKSE::log::info(
				"PIPELINE [{}] pass kind {}, family {}, technique 0x{:08X}, renderFlags 0x{:08X}, alphaTest {}, prepareStatus {}",
				label, static_cast<std::uint32_t>(a_pass.kind),
				static_cast<std::uint32_t>(a_pass.shaderFamily), a_pass.technique,
				a_pass.renderFlags, a_pass.alphaTest,
				static_cast<std::uint32_t>(a_prepared.status));
			SKSE::log::info(
				"PIPELINE [{}] NiAlphaProperty {} flags 0x{:04X}, threshold {}, blending {}, testing {}",
				label, static_cast<const void*>(alpha), alpha ? alpha->alphaFlags : 0,
				alpha ? alpha->alphaThreshold : 0,
				alpha ? alpha->GetAlphaBlending() : false,
				alpha ? alpha->GetAlphaTesting() : false);
			SKSE::log::info(
				"PIPELINE [{}] shaders VS {}, PS {}; topology {}; PS t0 {} format {} dimension {}; s0 {} filter {}; b11 {} bytes {}",
				label, static_cast<const void*>(vertexShader.Get()),
				static_cast<const void*>(pixelShader.Get()),
				static_cast<std::uint32_t>(topology),
				static_cast<const void*>(diffuseView.Get()),
				static_cast<std::uint32_t>(diffuseDescription.format),
				static_cast<std::uint32_t>(diffuseDescription.viewDimension),
				static_cast<const void*>(diffuseSampler.Get()),
				static_cast<std::uint32_t>(samplerDescription.filter),
				static_cast<const void*>(alphaReference.Get()),
				alphaReferenceDescription.byteWidth);
			SKSE::log::info(
				"PIPELINE [{}] depth state {} enable {}, writeMask {}, func {}; stencil enable {}, ref {}, readMask 0x{:02X}, writeMask 0x{:02X}, front(fail {}, depthFail {}, pass {}, func {})",
				label, static_cast<const void*>(depthState.Get()), depthDescription.depthEnable,
				static_cast<std::uint32_t>(depthDescription.depthWriteMask),
				static_cast<std::uint32_t>(depthDescription.depthFunc),
				depthDescription.stencilEnable, stencilReference,
				depthDescription.stencilReadMask, depthDescription.stencilWriteMask,
				static_cast<std::uint32_t>(depthDescription.frontFace.stencilFailOp),
				static_cast<std::uint32_t>(depthDescription.frontFace.stencilDepthFailOp),
				static_cast<std::uint32_t>(depthDescription.frontFace.stencilPassOp),
				static_cast<std::uint32_t>(depthDescription.frontFace.stencilFunc));
			const auto& target0 = blendDescription.renderTarget[0];
			SKSE::log::info(
				"PIPELINE [{}] blend state {} alphaToCoverage {}, independent {}, sampleMask 0x{:08X}; RT0 enable {}, src {}, dst {}, op {}, srcA {}, dstA {}, opA {}, writeMask 0x{:02X}; factor ({:.3f}, {:.3f}, {:.3f}, {:.3f})",
				label, static_cast<const void*>(blendState.Get()),
				blendDescription.alphaToCoverageEnable,
				blendDescription.independentBlendEnable, sampleMask,
				target0.blendEnable, static_cast<std::uint32_t>(target0.srcBlend),
				static_cast<std::uint32_t>(target0.destBlend),
				static_cast<std::uint32_t>(target0.blendOp),
				static_cast<std::uint32_t>(target0.srcBlendAlpha),
				static_cast<std::uint32_t>(target0.destBlendAlpha),
				static_cast<std::uint32_t>(target0.blendOpAlpha),
				target0.renderTargetWriteMask, blendFactor[0], blendFactor[1],
				blendFactor[2], blendFactor[3]);
			SKSE::log::info(
				"PIPELINE [{}] raster state {} fill {}, cull {}, frontCCW {}, depthBias {}, slopeBias {:.3f}, depthClip {}, scissor {}, multisample {}",
				label, static_cast<const void*>(rasterizerState.Get()),
				static_cast<std::uint32_t>(rasterizerDescription.fillMode),
				static_cast<std::uint32_t>(rasterizerDescription.cullMode),
				rasterizerDescription.frontCounterClockwise,
				rasterizerDescription.depthBias,
				rasterizerDescription.slopeScaledDepthBias,
				rasterizerDescription.depthClipEnable,
				rasterizerDescription.scissorEnable,
				rasterizerDescription.multisampleEnable);
			if (viewportCount != 0) {
				const auto& viewport = viewports[0];
				SKSE::log::info(
					"PIPELINE [{}] viewports {}, first ({:.1f}, {:.1f}, {:.1f}x{:.1f}, depth {:.3f}..{:.3f})",
					label, viewportCount, viewport.topLeftX, viewport.topLeftY,
					viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth);
			}
			SKSE::log::info(
				"PIPELINE [{}] render targets {}, DSV {} format {} dimension {} flags 0x{:X}",
				label, renderTargetCount, depthViewAddress,
				static_cast<std::uint32_t>(depthViewDescription.format),
				static_cast<std::uint32_t>(depthViewDescription.viewDimension),
				depthViewDescription.flags);
			for (std::uint32_t index = 0; index < renderTargetCount; ++index) {
				if (!renderTargetAddresses[index]) {
					continue;
				}
				SKSE::log::info(
					"PIPELINE [{}] RT{} {} format {} dimension {}",
					label, index, renderTargetAddresses[index],
					static_cast<std::uint32_t>(renderTargetDescriptions[index].format),
					static_cast<std::uint32_t>(renderTargetDescriptions[index].viewDimension));
			}
			SKSE::log::info("PIPELINE SNAPSHOT END [{}]", label);
		} catch (...) {
			// Diagnostics must never affect rendering.
		}
	}

	void __stdcall RenderHooks::DrawIndexedThunk(
		REX::W32::ID3D11DeviceContext* a_context,
		std::uint32_t a_indexCount,
		std::uint32_t a_startIndex,
		std::int32_t a_baseVertex)
	{
		auto& hooks = GetSingleton();
		if (!hooks.reportedDrawIndexedEntry_.exchange(
			true, std::memory_order_acq_rel)) {
			SKSE::log::info(
				"D3D11 DrawIndexed routing is active on context {}",
				static_cast<const void*>(a_context));
		}
		const auto dispatch = hooks.ResolveDrawIndexed(a_context);
		if (!dispatch.next) {
			return;
		}

		if (insideIndexedDrawHook ||
			!hooks.enabled_.load(std::memory_order_acquire) || !dispatch.accepted) {
			dispatch.next(a_context, a_indexCount, a_startIndex, a_baseVertex);
			return;
		}

		ReentrancyGuard reentrancyGuard;
		const auto* pass = ScopedPassContext::Current();
		auto* router = hooks.router_.load(std::memory_order_acquire);
		if (!pass || !router) {
			if (!pass && !hooks.reportedDrawWithoutPass_.exchange(
				true, std::memory_order_acq_rel)) {
				SKSE::log::warn(
					"An indexed D3D11 draw arrived outside a Lighting/Utility pass scope");
			}
			dispatch.next(a_context, a_indexCount, a_startIndex, a_baseVertex);
			return;
		}

		const IndexedDrawArgs drawArgs{ a_indexCount, a_startIndex, a_baseVertex };
		const auto prepared = router->Prepare(a_context, *pass, drawArgs);
		hooks.ReportPipelineSnapshotOnce(a_context, prepared, *pass, drawArgs);
		hooks.ReportCarrierDrawOnce(prepared, *pass, "DrawIndexed");
		if (!prepared.IsReady()) {
			if (!prepared.carrierMatched) {
				dispatch.next(a_context, a_indexCount, a_startIndex, a_baseVertex);
			}
			return;
		}

		ScopedNativeDrawState state(a_context, prepared.draw);
		if (!state.IsApplied()) {
			return;
		}

		for (std::uint32_t layer = 0; layer < prepared.draw.layerCount; ++layer) {
			const auto& layerDraw = prepared.draw.layers[layer];
			if (!state.BindLayer(layerDraw)) {
				break;
			}
			a_context->DrawIndexed(
				prepared.draw.indexCount, prepared.draw.startIndex,
				layerDraw.baseVertex);
		}
		shaderPassScopes.MarkHandled(pass->pass);
	}

	void __stdcall RenderHooks::DrawIndexedInstancedThunk(
		REX::W32::ID3D11DeviceContext* a_context,
		std::uint32_t a_indexCountPerInstance,
		std::uint32_t a_instanceCount,
		std::uint32_t a_startIndex,
		std::int32_t a_baseVertex,
		std::uint32_t a_startInstance)
	{
		auto& hooks = GetSingleton();
		if (!hooks.reportedDrawIndexedInstancedEntry_.exchange(
			true, std::memory_order_acq_rel)) {
			SKSE::log::info(
				"D3D11 DrawIndexedInstanced routing is active on context {}",
				static_cast<const void*>(a_context));
		}
		const auto dispatch = hooks.ResolveDrawIndexedInstanced(a_context);
		if (!dispatch.next) {
			return;
		}

		if (insideIndexedDrawHook ||
			!hooks.enabled_.load(std::memory_order_acquire) || !dispatch.accepted) {
			dispatch.next(a_context, a_indexCountPerInstance, a_instanceCount, a_startIndex,
				a_baseVertex, a_startInstance);
			return;
		}

		ReentrancyGuard reentrancyGuard;
		const auto* pass = ScopedPassContext::Current();
		auto* router = hooks.router_.load(std::memory_order_acquire);
		if (!pass || !router) {
			if (!pass && !hooks.reportedDrawWithoutPass_.exchange(
				true, std::memory_order_acq_rel)) {
				SKSE::log::warn(
					"An indexed D3D11 draw arrived outside a Lighting/Utility pass scope");
			}
			dispatch.next(a_context, a_indexCountPerInstance, a_instanceCount, a_startIndex,
				a_baseVertex, a_startInstance);
			return;
		}

		const IndexedDrawArgs drawArgs{
			a_indexCountPerInstance, a_startIndex, a_baseVertex
		};
		const auto prepared = router->Prepare(a_context, *pass, drawArgs);
		hooks.ReportPipelineSnapshotOnce(a_context, prepared, *pass, drawArgs);
		hooks.ReportCarrierDrawOnce(prepared, *pass, "DrawIndexedInstanced");
		if (!prepared.IsReady()) {
			if (!prepared.carrierMatched) {
				dispatch.next(a_context, a_indexCountPerInstance, a_instanceCount, a_startIndex,
					a_baseVertex, a_startInstance);
			}
			return;
		}

		ScopedNativeDrawState state(a_context, prepared.draw);
		if (!state.IsApplied()) {
			return;
		}

		for (std::uint32_t layer = 0; layer < prepared.draw.layerCount; ++layer) {
			const auto& layerDraw = prepared.draw.layers[layer];
			if (!state.BindLayer(layerDraw)) {
				break;
			}
			a_context->DrawIndexedInstanced(
				prepared.draw.indexCount, a_instanceCount,
				prepared.draw.startIndex, layerDraw.baseVertex, a_startInstance);
		}
		shaderPassScopes.MarkHandled(pass->pass);
	}

}
