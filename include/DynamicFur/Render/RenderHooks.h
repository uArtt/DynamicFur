#pragma once

#include "DynamicFur/Render/RenderBackend.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace DynamicFur::Render
{
	class RenderHooks final
	{
	public:
		using DrawIndexedFn = void(__stdcall*)(
			REX::W32::ID3D11DeviceContext*,
			std::uint32_t,
			std::uint32_t,
			std::int32_t);
		using DrawIndexedInstancedFn = void(__stdcall*)(
			REX::W32::ID3D11DeviceContext*,
			std::uint32_t,
			std::uint32_t,
			std::uint32_t,
			std::int32_t,
			std::uint32_t);

		inline static constexpr std::size_t kDrawIndexedVtableIndex = 12;
		inline static constexpr std::size_t kDrawIndexedInstancedVtableIndex = 20;

		[[nodiscard]] static RenderHooks& GetSingleton() noexcept;
		[[nodiscard]] bool InstallPassScopes();
		[[nodiscard]] bool UninstallPassScopes();
		[[nodiscard]] bool InstallDrawHooks(
			REX::W32::ID3D11DeviceContext* a_immediateContext,
			RenderRouter& a_router);
		[[nodiscard]] bool InstallAdditionalDrawContext(
			REX::W32::ID3D11DeviceContext* a_immediateContext);
		[[nodiscard]] bool UninstallDrawHooks();
		void SetEnabled(bool a_enabled) noexcept;
		[[nodiscard]] bool IsEnabled() const noexcept;
		[[nodiscard]] bool IsInstalled() const noexcept;
		[[nodiscard]] bool ArePassScopesInstalled() const noexcept;

	private:
		struct DrawHookSite
		{
			REX::W32::ComPtr<REX::W32::ID3D11DeviceContext> owner;
			std::atomic<std::uintptr_t*> vtable{};
			std::atomic<DrawIndexedFn> nextDrawIndexed{};
			std::atomic<DrawIndexedInstancedFn> nextDrawIndexedInstanced{};
			std::atomic_bool reportedDisplaced{};
		};

		struct AcceptedDrawContext
		{
			REX::W32::ComPtr<REX::W32::ID3D11DeviceContext> owner;
			std::atomic<REX::W32::ID3D11DeviceContext*> context{};
			std::atomic<std::uintptr_t*> siteVtable{};
		};

		struct DrawIndexedDispatch
		{
			DrawIndexedFn next{};
			bool accepted{};
		};

		struct DrawIndexedInstancedDispatch
		{
			DrawIndexedInstancedFn next{};
			bool accepted{};
		};

		using ShaderGeometryFn = void(*)(
			RE::BSShader*, RE::BSRenderPass*, std::uint32_t);

		static void LightingSetupGeometryThunk(
			RE::BSShader* a_shader,
			RE::BSRenderPass* a_pass,
			std::uint32_t a_renderFlags);
		static void LightingRestoreGeometryThunk(
			RE::BSShader* a_shader,
			RE::BSRenderPass* a_pass,
			std::uint32_t a_renderFlags);
		static void UtilitySetupGeometryThunk(
			RE::BSShader* a_shader,
			RE::BSRenderPass* a_pass,
			std::uint32_t a_renderFlags);
		static void UtilityRestoreGeometryThunk(
			RE::BSShader* a_shader,
			RE::BSRenderPass* a_pass,
			std::uint32_t a_renderFlags);

		static void __stdcall DrawIndexedThunk(
			REX::W32::ID3D11DeviceContext* a_context,
			std::uint32_t a_indexCount,
			std::uint32_t a_startIndex,
			std::int32_t a_baseVertex);
		static void __stdcall DrawIndexedInstancedThunk(
			REX::W32::ID3D11DeviceContext* a_context,
			std::uint32_t a_indexCountPerInstance,
			std::uint32_t a_instanceCount,
			std::uint32_t a_startIndex,
			std::int32_t a_baseVertex,
			std::uint32_t a_startInstance);
		void ReportCarrierDrawOnce(
			const PrepareResult& a_prepared,
			const FurPassContext& a_pass,
			const char* a_command) noexcept;
		void ReportPipelineSnapshotOnce(
			REX::W32::ID3D11DeviceContext* a_context,
			const PrepareResult& a_prepared,
			const FurPassContext& a_pass,
			const IndexedDrawArgs& a_draw) noexcept;
		void DrawPassFallback(RE::BSRenderPass* a_pass) noexcept;
		[[nodiscard]] bool InstallDrawContextLocked(
			REX::W32::ID3D11DeviceContext* a_immediateContext);
		[[nodiscard]] DrawIndexedDispatch ResolveDrawIndexed(
			REX::W32::ID3D11DeviceContext* a_context) const noexcept;
		[[nodiscard]] DrawIndexedInstancedDispatch ResolveDrawIndexedInstanced(
			REX::W32::ID3D11DeviceContext* a_context) const noexcept;

		std::mutex installMutex_;
		std::atomic<ShaderGeometryFn> lightingSetupGeometry_{};
		std::atomic<ShaderGeometryFn> lightingRestoreGeometry_{};
		std::atomic<ShaderGeometryFn> utilitySetupGeometry_{};
		std::atomic<ShaderGeometryFn> utilityRestoreGeometry_{};
		std::uintptr_t* lightingVtable_{};
		std::uintptr_t* utilityVtable_{};
		inline static constexpr std::size_t kMaximumDrawHookSites = 4;
		inline static constexpr std::size_t kMaximumAcceptedDrawContexts = 8;
		std::array<DrawHookSite, kMaximumDrawHookSites> drawHookSites_;
		std::array<AcceptedDrawContext, kMaximumAcceptedDrawContexts>
			acceptedDrawContexts_;
		std::atomic<REX::W32::ID3D11DeviceContext*> context_{};
		std::atomic<RenderRouter*> router_{};
		std::atomic_bool installed_{};
		std::atomic_bool passScopesInstalled_{};
		std::atomic_bool enabled_{ true };
		std::array<std::atomic_bool,
			static_cast<std::size_t>(PrepareStatus::InvalidDraw) + 1>
			reportedPrepareStatuses_{};
		std::array<std::atomic_bool,
			static_cast<std::size_t>(FurPassKind::ShadowMask) + 1>
			reportedReadyPassKinds_{};
		std::atomic_bool reportedOriginalBodyPipeline_{};
		std::atomic_bool reportedCarrierPipeline_{};
		std::atomic_bool reportedDrawIndexedEntry_{};
		std::atomic_bool reportedDrawIndexedInstancedEntry_{};
		std::atomic_bool reportedDrawWithoutPass_{};
		std::atomic_bool reportedPassFallbackEntry_{};
		std::atomic_bool reportedRecoveredPassScope_{};
	};
}
