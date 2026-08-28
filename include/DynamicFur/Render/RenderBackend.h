#pragma once

#include <bit>

#include "DynamicFur/Core/ShellCoverage.h"
#include "DynamicFur/Core/ShellProfile.h"
#include "RE/V/VertexDesc.h"
#include "REX/W32/COMPTR.h"
#include "REX/W32/D3D11.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <utility>

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSRenderPass;
	class BSShader;
}

namespace DynamicFur::Render
{
	inline constexpr std::uint32_t kAlphaReferenceConstantSlot = 11;
	inline constexpr std::uint32_t kCarrierVertexBufferSlot = 0;

	enum class BackendKind : std::uint8_t
	{
		Null,
		NativeMultiDraw
	};

	enum class ShaderFamily : std::uint8_t
	{
		Unknown,
		Lighting,
		Utility
	};

	enum class FurPassKind : std::uint8_t
	{
		Unsupported,
		Lighting,
		Depth,
		Normal,
		ShadowMap,
		ShadowMask
	};

	struct FurPassContext
	{
		RE::BSBatchRenderer* batch{};
		RE::BSRenderPass* pass{};
		RE::BSGeometry* geometry{};
		std::uint32_t technique{};
		std::uint32_t renderFlags{};
		bool alphaTest{};
		ShaderFamily shaderFamily{ ShaderFamily::Unknown };
		FurPassKind kind{ FurPassKind::Unsupported };
	};

	struct IndexedDrawArgs
	{
		std::uint32_t indexCount{};
		std::uint32_t startIndex{};
		std::int32_t baseVertex{};
	};

	struct NativeCarrierSidecar
	{
		RE::BSGeometry* geometry{};
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> sourceVertexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> sourceIndexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> expandedVertexBuffer;
		// Optional replacement topology. The registry continues to identify the
		// carrier through sourceVertexBuffer/sourceIndexBuffer; this buffer is bound
		// only inside ScopedNativeDrawState after that lookup has succeeded.
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> shellIndexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11DepthStencilState> shellDepthState;
		std::array<REX::W32::ComPtr<REX::W32::ID3D11Buffer>, Core::kMaximumShellCount>
			alphaReferenceBuffers;
		std::uint32_t vertexStride{};
		std::uint32_t sourceVertexOffset{};
		std::uint32_t expandedVertexOffset{};
		std::uint32_t sourceIndexOffset{};
		std::uint32_t sourceVertexCount{};
		std::uint32_t sourceIndexCount{};
		std::uint32_t shellVertexCount{};
		std::uint32_t shellIndexCount{};
		std::uint32_t profileShellCount{};
		std::uint32_t hiddenTriangleCount{};
		std::uint64_t vertexDescriptorBits{};
		std::uint64_t profileFingerprint{};
		std::uint64_t geometryRevision{};
		// Refined streams use local indices starting at vertex zero. Legacy and
		// binary-coverage streams retain the source draw's base-vertex semantics.
		bool refinedShellTopology{};
		bool correctShellPipeline{};

		[[nodiscard]] bool IsValid() const noexcept;
	};

	struct CarrierRegistration
	{
		std::shared_ptr<const NativeCarrierSidecar> sidecar;
		std::uint32_t viewShellCount{};
		std::uint32_t shadowShellCount{};
		std::uint64_t stateRevision{};
		bool enabled{ true };
	};

	struct ResolvedCarrier
	{
		std::shared_ptr<const NativeCarrierSidecar> sidecar;
		std::uint32_t viewShellCount{};
		std::uint32_t shadowShellCount{};
		std::uint64_t stateRevision{};
		bool enabled{};
	};

	enum class CarrierResolveStatus : std::uint8_t
	{
		NotCarrierGeometry,
		CarrierIdentityMismatch,
		ExactMatch
	};

	struct CarrierResolveResult
	{
		CarrierResolveStatus status{ CarrierResolveStatus::NotCarrierGeometry };
		std::optional<ResolvedCarrier> carrier;

		[[nodiscard]] bool IsCarrierGeometry() const noexcept
		{
			return status != CarrierResolveStatus::NotCarrierGeometry;
		}
	};

	struct CarrierLookup
	{
		RE::BSGeometry* geometry{};
		REX::W32::ID3D11Buffer* vertexBuffer{};
		REX::W32::ID3D11Buffer* indexBuffer{};
		REX::W32::DXGI_FORMAT indexFormat{ REX::W32::DXGI_FORMAT_UNKNOWN };
		std::uint32_t vertexStride{};
		std::uint32_t vertexOffset{};
		std::uint32_t indexOffset{};
	};

	enum class RegistrationResult : std::uint8_t
	{
		Added,
		Replaced,
		Invalid
	};

	class ICarrierDrawSource
	{
	public:
		virtual ~ICarrierDrawSource() = default;

		[[nodiscard]] virtual CarrierResolveResult Resolve(
			const CarrierLookup& a_lookup) const noexcept = 0;
	};

	class CarrierRegistry final : public ICarrierDrawSource
	{
	public:
		[[nodiscard]] RegistrationResult Register(const CarrierRegistration& a_registration);
		// Validates the complete replacement before one exclusive swap. In-flight
		// draws retain old shared_ptr sidecars; no frame can observe a half-filled
		// registry during an armor-only coverage refresh.
		[[nodiscard]] bool ReplaceAll(
			std::span<const CarrierRegistration> a_registrations);
		[[nodiscard]] bool UpdateState(
			RE::BSGeometry* a_geometry,
			REX::W32::ID3D11Buffer* a_vertexBuffer,
			REX::W32::ID3D11Buffer* a_indexBuffer,
			std::uint32_t a_viewShellCount,
			std::uint32_t a_shadowShellCount,
			bool a_enabled,
			std::uint64_t a_stateRevision);
		[[nodiscard]] bool Unregister(
			RE::BSGeometry* a_geometry,
			REX::W32::ID3D11Buffer* a_vertexBuffer,
			REX::W32::ID3D11Buffer* a_indexBuffer);
		std::size_t UnregisterGeometry(RE::BSGeometry* a_geometry);
		void Clear();

		[[nodiscard]] std::size_t Size() const noexcept;
		[[nodiscard]] CarrierResolveResult Resolve(
			const CarrierLookup& a_lookup) const noexcept override;

	private:
		struct Key
		{
			RE::BSGeometry* geometry{};
			REX::W32::ID3D11Buffer* vertexBuffer{};
			REX::W32::ID3D11Buffer* indexBuffer{};

			[[nodiscard]] bool operator==(const Key&) const noexcept = default;
		};

		struct KeyHash
		{
			[[nodiscard]] std::size_t operator()(const Key& a_key) const noexcept;
		};

		mutable std::shared_mutex mutex_;
		std::unordered_map<Key, CarrierRegistration, KeyHash> entries_;
		std::unordered_map<RE::BSGeometry*, std::size_t> geometryCounts_;
	};

	struct NativeSidecarBuildRequest
	{
		RE::BSGeometry* geometry{};
		REX::W32::ID3D11Device* device{};
		REX::W32::ID3D11Buffer* sourceVertexBuffer{};
		REX::W32::ID3D11Buffer* sourceIndexBuffer{};
		REX::W32::DXGI_FORMAT indexFormat{ REX::W32::DXGI_FORMAT_UNKNOWN };
		RE::BSGraphics::VertexDesc vertexDesc{};
		std::span<const std::byte> rawVertexData;
		std::span<const std::uint16_t> rawIndexData;
		// Optional packed base mesh for adaptive refinement. All related fields must
		// be provided together. The stream must use the source VertexDesc/stride;
		// indices are deliberately R16 for the first refinement implementation.
		std::span<const std::byte> shellVertexData;
		std::span<const std::uint16_t> shellIndexData;
		// Object-space positions and extrusion normals corresponding one-to-one to
		// shellVertexData. Adaptive refinement supplies interpolated normals here so
		// shell extrusion does not reinterpret the clipped topology's boundary.
		std::span<const Core::CarrierVertex> shellCarrierVertices;
		std::uint32_t shellVertexCount{};
		// Optional binary per-vertex armor coverage. A hit anywhere inside the
		// fur length removes the adjacent triangles from every shell through one
		// immutable R16 index buffer.
		std::span<const Core::FirstHiddenShell> coverage;
		// Partitions of one NiSkinPartition share the full-mesh vertex data.
		// Reusing an earlier sidecar avoids allocating the 17-shell VB and
		// alpha-reference buffers again for every index partition.
		std::shared_ptr<const NativeCarrierSidecar> sharedVertexResources;
		Core::ShellProfile profile;
		std::uint32_t vertexCount{};
		std::uint32_t vertexStride{};
		std::uint32_t sourceVertexOffset{};
		std::uint32_t sourceIndexOffset{};
		std::uint64_t geometryRevision{};
		// Diagnostic only: bind -1.0 as AlphaTestRef so even zero-alpha texels
		// survive. This isolates raster/depth issues from diffuse alpha issues.
		bool forceVisibleAlpha{};
		// Override only depth for displaced shells. Blend, MRT write masks,
		// stencil and rasterizer state remain exactly as Skyrim configured them.
		bool correctShellPipeline{};
	};

	enum class SidecarBuildError : std::uint8_t
	{
		InvalidArgument,
		InvalidProfile,
		UnsupportedIndexFormat,
		UnsupportedVertexFlags,
		InvalidVertexLayout,
		MissingRawData,
		InvalidIndex,
		InvalidCoverage,
		ResourceTooLarge,
		BufferCreationFailed,
		DeviceMismatch,
		InvalidBufferBinding,
		InvalidBufferRange
	};

	using SidecarBuildResult = std::expected<
		std::shared_ptr<const NativeCarrierSidecar>, SidecarBuildError>;

	[[nodiscard]] SidecarBuildResult BuildNativeSidecar(
		const NativeSidecarBuildRequest& a_request);

	struct NativeLayerDraw
	{
		std::int32_t baseVertex{};
		std::uint32_t canonicalLayer{};
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> alphaReferenceBuffer;
	};

	struct PreparedShellDraw
	{
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> expandedVertexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> shellIndexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11DepthStencilState> shellDepthState;
		std::array<NativeLayerDraw, Core::kMaximumShellCount> layers;
		std::uint32_t vertexBufferSlot{ kCarrierVertexBufferSlot };
		std::uint32_t vertexStride{};
		std::uint32_t vertexOffset{};
		REX::W32::DXGI_FORMAT shellIndexFormat{ REX::W32::DXGI_FORMAT_R16_UINT };
		std::uint32_t shellIndexOffset{};
		std::uint32_t indexCount{};
		std::uint32_t startIndex{};
		std::uint32_t layerCount{};

		[[nodiscard]] bool IsValid() const noexcept;
	};

	enum class PrepareStatus : std::uint8_t
	{
		Ready,
		NoPassContext,
		NoCarrier,
		DisabledCarrier,
		UnsupportedBackend,
		UnsupportedPass,
		UnsupportedPipeline,
		InvalidDraw
	};

	struct PrepareResult
	{
		PrepareStatus status{ PrepareStatus::UnsupportedBackend };
		PreparedShellDraw draw;
		// True only after the exact geometry/VB/IB identity was found in the
		// carrier registry. A matched carrier is synthetic: unsupported or
		// invalid passes must suppress it instead of drawing one fallback layer.
		bool carrierMatched{};

		[[nodiscard]] bool IsReady() const noexcept
		{
			return status == PrepareStatus::Ready && draw.IsValid();
		}
	};

	class IShellRenderAdapter
	{
	public:
		virtual ~IShellRenderAdapter() = default;
		[[nodiscard]] virtual BackendKind Kind() const noexcept = 0;
		[[nodiscard]] virtual PrepareResult Prepare(
			REX::W32::ID3D11DeviceContext* a_context,
			const FurPassContext& a_pass,
			const IndexedDrawArgs& a_draw,
			const ResolvedCarrier& a_carrier) const noexcept = 0;
	};

	class NullShellRenderAdapter final : public IShellRenderAdapter
	{
	public:
		[[nodiscard]] BackendKind Kind() const noexcept override;
		[[nodiscard]] PrepareResult Prepare(
			REX::W32::ID3D11DeviceContext*,
			const FurPassContext&,
			const IndexedDrawArgs&,
			const ResolvedCarrier&) const noexcept override;
	};

	class NativeMultiDrawAdapter final : public IShellRenderAdapter
	{
	public:
		[[nodiscard]] BackendKind Kind() const noexcept override;
		[[nodiscard]] PrepareResult Prepare(
			REX::W32::ID3D11DeviceContext* a_context,
			const FurPassContext& a_pass,
			const IndexedDrawArgs& a_draw,
			const ResolvedCarrier& a_carrier) const noexcept override;
	};

	class RenderRouter final
	{
	public:
		explicit RenderRouter(ICarrierDrawSource& a_carriers);
		void SetAdapter(std::shared_ptr<IShellRenderAdapter> a_adapter);
		[[nodiscard]] std::shared_ptr<IShellRenderAdapter> GetAdapter() const noexcept;
		[[nodiscard]] PrepareResult Prepare(
			REX::W32::ID3D11DeviceContext* a_context,
			const FurPassContext& a_pass,
			const IndexedDrawArgs& a_draw) const noexcept;
		// Used by the shader-pass fallback after Skyrim has issued the source
		// geometry draw. At that point the source IA bindings are still live, so
		// the immutable sidecar can supply the exact draw arguments without
		// depending on which D3D11 dispatch command the runtime used.
		[[nodiscard]] PrepareResult PrepareBoundCarrier(
			REX::W32::ID3D11DeviceContext* a_context,
			const FurPassContext& a_pass) const noexcept;

	private:
		ICarrierDrawSource* carriers_{};
		std::atomic<std::shared_ptr<IShellRenderAdapter>> adapter_;
	};

	[[nodiscard]] FurPassContext DescribePass(
		RE::BSBatchRenderer* a_batch,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_technique,
		bool a_alphaTest,
		std::uint32_t a_renderFlags) noexcept;
	// Both the live property and the actual shader permutation must alpha-test.
	// Keep this safety boundary independent from runtime pass construction.
	[[nodiscard]] FurPassKind ClassifyLightingPass(
		std::uint32_t a_technique,
		bool a_alphaTest) noexcept;

	// Utility technique IDs describe the output operation independently from
	// some of the source geometry's live bindings. In particular, Skyrim's
	// observed skinned alpha-tested depth technique is 0x20B1: it carries
	// RenderDepth and AlphaTest but not the generic Texture/Skinned flag pair.
	// Keep this classifier pure so that exact runtime techniques stay testable.
	[[nodiscard]] FurPassKind ClassifyUtilityPass(
		std::uint32_t a_technique,
		bool a_alphaTest) noexcept;

	class ScopedPassContext final
	{
	public:
		explicit ScopedPassContext(FurPassContext a_context) noexcept;
		ScopedPassContext(
			RE::BSBatchRenderer* a_batch,
			RE::BSRenderPass* a_pass,
			std::uint32_t a_technique,
			bool a_alphaTest,
			std::uint32_t a_renderFlags) noexcept;
		~ScopedPassContext() noexcept;

		ScopedPassContext(const ScopedPassContext&) = delete;
		ScopedPassContext& operator=(const ScopedPassContext&) = delete;

		[[nodiscard]] static const FurPassContext* Current() noexcept;

	private:
		FurPassContext context_;
		const FurPassContext* previous_{};
	};

	class ScopedNativeDrawState final
	{
	public:
		ScopedNativeDrawState(
			REX::W32::ID3D11DeviceContext* a_context,
			const PreparedShellDraw& a_draw) noexcept;
		~ScopedNativeDrawState() noexcept;

		ScopedNativeDrawState(const ScopedNativeDrawState&) = delete;
		ScopedNativeDrawState& operator=(const ScopedNativeDrawState&) = delete;

		[[nodiscard]] bool IsApplied() const noexcept;
		[[nodiscard]] bool BindLayer(const NativeLayerDraw& a_layer) noexcept;
		void Restore() noexcept;

	private:
		REX::W32::ID3D11DeviceContext* context_{};
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> originalVertexBuffer_;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> originalIndexBuffer_;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> originalAlphaReference_;
		REX::W32::ComPtr<REX::W32::ID3D11DepthStencilState> originalDepthState_;
		std::uint32_t vertexBufferSlot_{};
		std::uint32_t originalStride_{};
		std::uint32_t originalOffset_{};
		REX::W32::DXGI_FORMAT originalIndexFormat_{ REX::W32::DXGI_FORMAT_UNKNOWN };
		std::uint32_t originalIndexOffset_{};
		std::uint32_t originalStencilReference_{};
		bool indexOverridden_{};
		bool applied_{};
	};
}
