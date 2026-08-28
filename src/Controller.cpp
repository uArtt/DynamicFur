#include "PCH.h"

#include "DynamicFur/Controller.h"

#include "DynamicFur/Core/ShellCoverage.h"
#include "DynamicFur/Game/SkinnedPose.h"
#include "DynamicFur/Generator/AdaptiveShellMesh.h"
#include "DynamicFur/Generator/ShellIndexMask.h"
#include "DynamicFur/Generator/ShellVertexGenerator.h"
#include "DynamicFur/Render/RenderHooks.h"

#include "RE/L/LoadingMenu.h"
#include "RE/M/MenuOpenCloseEvent.h"
#include "RE/R/Renderer.h"
#include "RE/S/ScriptEventSourceHolder.h"
#include "RE/T/TESEquipEvent.h"
#include "RE/T/TESObjectLoadedEvent.h"
#include "RE/T/TESSwitchRaceCompleteEvent.h"
#include "RE/U/UI.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DynamicFur
{
	namespace
	{
		inline constexpr std::uint64_t kAdaptive3BaVertexDescriptor =
			0x0004300050000408ULL;

		class RendererLockGuard final
		{
		public:
			explicit RendererLockGuard(RE::BSGraphics::Renderer& a_renderer) :
				renderer_(a_renderer)
			{
				renderer_.Lock();
			}

			~RendererLockGuard() { renderer_.Unlock(); }

			RendererLockGuard(const RendererLockGuard&) = delete;
			RendererLockGuard& operator=(const RendererLockGuard&) = delete;

		private:
			RE::BSGraphics::Renderer& renderer_;
		};

		[[nodiscard]] const char* SidecarBuildErrorName(
			Render::SidecarBuildError a_error) noexcept
		{
			switch (a_error) {
			case Render::SidecarBuildError::InvalidArgument:
				return "invalid-argument";
			case Render::SidecarBuildError::InvalidProfile:
				return "invalid-profile";
			case Render::SidecarBuildError::UnsupportedIndexFormat:
				return "unsupported-index-format";
			case Render::SidecarBuildError::UnsupportedVertexFlags:
				return "unsupported-vertex-flags";
			case Render::SidecarBuildError::InvalidVertexLayout:
				return "invalid-vertex-layout";
			case Render::SidecarBuildError::MissingRawData:
				return "missing-raw-data";
			case Render::SidecarBuildError::InvalidIndex:
				return "invalid-index";
			case Render::SidecarBuildError::InvalidCoverage:
				return "invalid-coverage";
			case Render::SidecarBuildError::ResourceTooLarge:
				return "resource-too-large";
			case Render::SidecarBuildError::BufferCreationFailed:
				return "buffer-creation-failed";
			case Render::SidecarBuildError::DeviceMismatch:
				return "device-mismatch";
			case Render::SidecarBuildError::InvalidBufferBinding:
				return "invalid-buffer-binding";
			case Render::SidecarBuildError::InvalidBufferRange:
				return "invalid-buffer-range";
			default:
				return "unknown";
			}
		}

		[[nodiscard]] const char* AdaptiveMeshErrorName(
			Generator::AdaptiveShellMeshError a_error) noexcept
		{
			switch (a_error) {
			case Generator::AdaptiveShellMeshError::InvalidArgument:
				return "invalid-argument";
			case Generator::AdaptiveShellMeshError::UnsupportedLayout:
				return "unsupported-layout";
			case Generator::AdaptiveShellMeshError::InvalidVertex:
				return "invalid-vertex";
			case Generator::AdaptiveShellMeshError::InvalidTopology:
				return "invalid-topology";
			case Generator::AdaptiveShellMeshError::InvalidSkinning:
				return "invalid-skinning";
			case Generator::AdaptiveShellMeshError::CoverageFailure:
				return "coverage-failure";
			case Generator::AdaptiveShellMeshError::BudgetExceeded:
				return "budget-exceeded";
			case Generator::AdaptiveShellMeshError::ResourceTooLarge:
				return "resource-too-large";
			default:
				return "unknown";
			}
		}

		class PlayerEventSink final :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
			public RE::BSTEventSink<RE::TESSwitchRaceCompleteEvent>,
			public RE::BSTEventSink<SKSE::NiNodeUpdateEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			[[nodiscard]] static PlayerEventSink& GetSingleton() noexcept
			{
				static PlayerEventSink singleton;
				return singleton;
			}

			bool Install() noexcept
			{
				if (installed_) {
					return true;
				}
				auto* source = RE::ScriptEventSourceHolder::GetSingleton();
				if (!source) {
					return false;
				}
				source->AddEventSink<RE::TESEquipEvent>(this);
				source->AddEventSink<RE::TESObjectLoadedEvent>(this);
				source->AddEventSink<RE::TESSwitchRaceCompleteEvent>(this);
				if (auto* nodeUpdates = SKSE::GetNiNodeUpdateEventSource()) {
					nodeUpdates->AddEventSink(this);
				} else {
					SKSE::log::warn("SKSE NiNodeUpdateEvent source is unavailable");
				}
				if (auto* ui = RE::UI::GetSingleton()) {
					ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
				} else {
					SKSE::log::warn("Skyrim UI event source is unavailable");
				}
				installed_ = true;
				return true;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESEquipEvent* a_event,
				RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* armor = a_event ?
					RE::TESForm::LookupByID<RE::TESObjectARMO>(a_event->baseObject) :
					nullptr;
				if (a_event && armor && player && a_event->actor.get() == player) {
					Controller::GetSingleton().QueueArmorCoverageRefresh();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESObjectLoadedEvent* a_event,
				RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (a_event && player && a_event->formID == player->GetFormID()) {
					if (a_event->loaded) {
						Controller::GetSingleton().OnPlayer3DLoaded();
					} else {
						Controller::GetSingleton().OnPlayer3DReleased();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::TESSwitchRaceCompleteEvent* a_event,
				RE::BSTEventSource<RE::TESSwitchRaceCompleteEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (a_event && player && a_event->subject.get() == player) {
					Controller::GetSingleton().QueuePlayerRebuild();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const SKSE::NiNodeUpdateEvent* a_event,
				RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (a_event && player && a_event->reference == player) {
					Controller::GetSingleton().QueueArmorCoverageRefresh();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && !a_event->opening && a_event->menuName.c_str() &&
					std::string_view{ a_event->menuName.c_str() } ==
						RE::LoadingMenu::MENU_NAME) {
					// PostLoadGame can precede the final player 3D/biped rebuild.
					// Loading Menu closing is a second, independent recovery edge.
					SKSE::log::info(
						"Loading Menu closed; player fur recovery queued");
					Controller::GetSingleton().OnGameReady();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			bool installed_{};
		};

		[[nodiscard]] std::expected<std::vector<std::byte>, std::string> ReadBufferBytes(
			REX::W32::ID3D11Device* a_device,
			REX::W32::ID3D11DeviceContext* a_context,
			REX::W32::ID3D11Buffer* a_source,
			std::uint32_t a_offset,
			std::size_t a_size)
		{
			if (!a_device || !a_context || !a_source || a_size == 0 ||
				a_size > (std::numeric_limits<std::uint32_t>::max)()) {
				return std::unexpected("invalid GPU readback request");
			}

			REX::W32::D3D11_BUFFER_DESC sourceDescription{};
			a_source->GetDesc(&sourceDescription);
			if (a_offset > sourceDescription.byteWidth ||
				a_size > sourceDescription.byteWidth - a_offset) {
				return std::unexpected("GPU readback range exceeds the source buffer");
			}

			REX::W32::D3D11_BUFFER_DESC stagingDescription{};
			stagingDescription.byteWidth = sourceDescription.byteWidth;
			stagingDescription.usage = REX::W32::D3D11_USAGE_STAGING;
			stagingDescription.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;

			REX::W32::ComPtr<REX::W32::ID3D11Buffer> staging;
			std::vector<std::byte> result(a_size);
			if (a_device->CreateBuffer(
					&stagingDescription, nullptr, staging.ReleaseAndGetAddressOf()) < 0 ||
				!staging.Get()) {
				return std::unexpected("could not create a staging buffer");
			}

			a_context->CopyResource(staging.Get(), a_source);
			REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
			if (a_context->Map(
					staging.Get(), 0, REX::W32::D3D11_MAP_READ, 0, &mapped) < 0 ||
				!mapped.data) {
				return std::unexpected("could not map the staging buffer");
			}

			std::memcpy(
				result.data(),
				static_cast<const std::byte*>(mapped.data) + a_offset,
				a_size);
			a_context->Unmap(staging.Get(), 0);
			return result;
		}

		struct PosedPartitionSnapshot
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			REX::W32::ComPtr<REX::W32::ID3D11Device> device;
			REX::W32::ComPtr<REX::W32::ID3D11Buffer> vertexBuffer;
			REX::W32::ComPtr<REX::W32::ID3D11Buffer> indexBuffer;
			RE::BSGraphics::VertexDesc vertexDesc{};
			std::vector<std::byte> rawVertices;
			std::vector<std::uint16_t> rawIndices;
			std::vector<Core::CarrierVertex> referenceVertices;
			std::vector<Core::CarrierVertex> posedVertices;
			std::uint32_t partitionIndex{};
			std::uint32_t vertexCount{};
			std::uint32_t vertexStride{};
		};

		[[nodiscard]] std::expected<PosedPartitionSnapshot, std::string>
			CaptureSkinnedPartition(
				RE::BSGeometry& a_geometry,
				RE::NiNode& a_skeletonRoot,
				const RE::NiSkinPartition::Partition& a_partition,
				std::uint32_t a_partitionIndex,
				std::uint32_t a_vertexCount,
				REX::W32::ID3D11DeviceContext* a_context)
		{
			auto* buffers = a_partition.buffData;
			if (!a_context || !buffers || !buffers->vertexBuffer ||
				!buffers->indexBuffer || a_partition.triangles == 0 ||
				a_vertexCount == 0) {
				return std::unexpected("partition has no live draw buffers");
			}

			PosedPartitionSnapshot result;
			result.geometry = RE::NiPointer<RE::BSGeometry>{ std::addressof(a_geometry) };
			result.vertexDesc = buffers->vertexDesc;
			result.partitionIndex = a_partitionIndex;
			result.vertexCount = a_vertexCount;
			result.vertexStride = result.vertexDesc.GetSize();
			if (result.vertexStride == 0 ||
				std::bit_cast<std::uint64_t>(a_partition.vertexDesc) !=
					std::bit_cast<std::uint64_t>(result.vertexDesc)) {
				return std::unexpected("partition vertex descriptor is inconsistent");
			}
			if (!result.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED)) {
				return std::unexpected("partition vertex stream is not skinned");
			}
			const auto skinningOffset = result.vertexDesc.GetAttributeOffset(
				RE::BSGraphics::Vertex::VA_SKINNING);
			if (skinningOffset > result.vertexStride ||
				12U > result.vertexStride - skinningOffset) {
				return std::unexpected("partition skinning attribute is out of range");
			}

			result.vertexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(
				buffers->vertexBuffer);
			result.indexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(
				buffers->indexBuffer);
			result.vertexBuffer->GetDevice(result.device.ReleaseAndGetAddressOf());
			if (!result.device.Get()) {
				return std::unexpected("partition buffer has no D3D11 device");
			}

			const auto vertexBytes = static_cast<std::size_t>(a_vertexCount) *
				result.vertexStride;
			const auto indexCount = static_cast<std::size_t>(a_partition.triangles) * 3U;
			const auto indexBytes = indexCount * sizeof(std::uint16_t);
			auto vertices = ReadBufferBytes(
				result.device.Get(), a_context, result.vertexBuffer.Get(), 0, vertexBytes);
			if (!vertices) {
				return std::unexpected("vertex readback failed: " + vertices.error());
			}
			auto indices = ReadBufferBytes(
				result.device.Get(), a_context, result.indexBuffer.Get(), 0, indexBytes);
			if (!indices) {
				return std::unexpected("index readback failed: " + indices.error());
			}
			result.rawVertices = std::move(*vertices);
			result.rawIndices.resize(indexCount);
			std::memcpy(result.rawIndices.data(), indices->data(), indexBytes);

			const auto flags = static_cast<std::uint16_t>(result.vertexDesc.GetFlags());
			const auto hasNormal =
				(flags & RE::BSGraphics::Vertex::VF_NORMAL) != 0;
			auto decoded = Generator::DecodeCarrierVertices({
				.sourceVertices = result.rawVertices,
				.vertexCount = result.vertexCount,
				.vertexStride = result.vertexStride,
				.positionOffset = result.vertexDesc.GetAttributeOffset(
					RE::BSGraphics::Vertex::VA_POSITION),
				.packedNormalOffset = hasNormal ?
					std::optional<std::uint32_t>{ result.vertexDesc.GetAttributeOffset(
						RE::BSGraphics::Vertex::VA_NORMAL) } :
					std::nullopt,
				.sourceIndices = result.rawIndices
			});
			if (!decoded) {
				return std::unexpected(std::format(
					"packed vertex decode failed ({})",
					static_cast<std::uint32_t>(decoded.error())));
			}

			const auto posed = Game::PoseVertices({
				.geometry = std::addressof(a_geometry),
				.skeletonRoot = std::addressof(a_skeletonRoot),
				.partition = std::addressof(a_partition),
				.packedVertices = result.rawVertices,
				.sourceIndices = result.rawIndices,
				.referenceVertices = *decoded,
				.vertexStride = result.vertexStride,
				.skinningOffset = skinningOffset
			});
			if (!posed) {
				return std::unexpected("CPU skinning failed: " + posed.error());
			}
			result.referenceVertices = std::move(*decoded);
			result.posedVertices = std::move(*posed);
			return result;
		}

		[[nodiscard]] std::expected<PosedPartitionSnapshot, std::string>
			CaptureRigidGeometry(
				RE::BSGeometry& a_geometry,
				RE::NiNode& a_skeletonRoot,
				REX::W32::ID3D11DeviceContext* a_context)
		{
			auto* triShape = a_geometry.AsTriShape();
			auto& runtime = a_geometry.GetGeometryRuntimeData();
			auto* buffers = runtime.rendererData;
			if (!triShape || !buffers || !buffers->vertexBuffer ||
				!buffers->indexBuffer) {
				return std::unexpected("rigid geometry has no triangle buffers");
			}
			const auto& shapeData = triShape->GetTrishapeRuntimeData();
			if (shapeData.vertexCount == 0 || shapeData.triangleCount == 0) {
				return std::unexpected("rigid geometry is empty");
			}

			// Reuse the same packed decode/pose path through a temporary partition
			// description; PoseVertices observes the absent skin instance and uses
			// root^-1 * geometry.world instead of its palette fields.
			PosedPartitionSnapshot result;
			result.geometry = RE::NiPointer<RE::BSGeometry>{ std::addressof(a_geometry) };
			result.vertexDesc = buffers->vertexDesc;
			result.vertexCount = shapeData.vertexCount;
			result.vertexStride = result.vertexDesc.GetSize();
			result.vertexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(
				buffers->vertexBuffer);
			result.indexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(
				buffers->indexBuffer);
			result.vertexBuffer->GetDevice(result.device.ReleaseAndGetAddressOf());
			if (!result.device.Get() || result.vertexStride == 0) {
				return std::unexpected("rigid geometry has an invalid D3D11 layout");
			}

			const auto vertexBytes = static_cast<std::size_t>(result.vertexCount) *
				result.vertexStride;
			const auto indexCount = static_cast<std::size_t>(shapeData.triangleCount) * 3U;
			const auto indexBytes = indexCount * sizeof(std::uint16_t);
			auto vertices = ReadBufferBytes(
				result.device.Get(), a_context, result.vertexBuffer.Get(), 0, vertexBytes);
			auto indices = ReadBufferBytes(
				result.device.Get(), a_context, result.indexBuffer.Get(), 0, indexBytes);
			if (!vertices || !indices) {
				return std::unexpected("rigid geometry GPU readback failed");
			}
			result.rawVertices = std::move(*vertices);
			result.rawIndices.resize(indexCount);
			std::memcpy(result.rawIndices.data(), indices->data(), indexBytes);

			const auto hasNormal = result.vertexDesc.HasFlag(
				RE::BSGraphics::Vertex::VF_NORMAL);
			auto decoded = Generator::DecodeCarrierVertices({
				.sourceVertices = result.rawVertices,
				.vertexCount = result.vertexCount,
				.vertexStride = result.vertexStride,
				.positionOffset = result.vertexDesc.GetAttributeOffset(
					RE::BSGraphics::Vertex::VA_POSITION),
				.packedNormalOffset = hasNormal ?
					std::optional<std::uint32_t>{ result.vertexDesc.GetAttributeOffset(
						RE::BSGraphics::Vertex::VA_NORMAL) } :
					std::nullopt,
				.sourceIndices = result.rawIndices
			});
			if (!decoded) {
				return std::unexpected("rigid packed vertex decode failed");
			}
			const auto posed = Game::PoseVertices({
				.geometry = std::addressof(a_geometry),
				.skeletonRoot = std::addressof(a_skeletonRoot),
				.partition = nullptr,
				.packedVertices = result.rawVertices,
				.sourceIndices = result.rawIndices,
				.referenceVertices = *decoded,
				.vertexStride = result.vertexStride,
				.skinningOffset = 0
			});
			if (!posed) {
				return std::unexpected("rigid pose failed: " + posed.error());
			}
			result.referenceVertices = std::move(*decoded);
			result.posedVertices = std::move(*posed);
			return result;
		}

		void AppendTriangles(
			const PosedPartitionSnapshot& a_snapshot,
			std::vector<Core::Triangle>& a_output)
		{
			a_output.reserve(a_output.size() + a_snapshot.rawIndices.size() / 3U);
			for (std::size_t triangle = 0;
				triangle < a_snapshot.rawIndices.size(); triangle += 3) {
				a_output.push_back({
					a_snapshot.posedVertices[a_snapshot.rawIndices[triangle]].position,
					a_snapshot.posedVertices[a_snapshot.rawIndices[triangle + 1]].position,
					a_snapshot.posedVertices[a_snapshot.rawIndices[triangle + 2]].position
				});
			}
		}

		[[nodiscard]] std::expected<std::vector<Core::CarrierVertex>, std::string>
			BuildTriangleCentroidSamples(
				std::span<const Core::CarrierVertex> a_vertices,
				std::span<const std::uint16_t> a_indices)
		{
			if (a_vertices.empty() || a_indices.empty() || (a_indices.size() % 3U) != 0) {
				return std::unexpected("invalid centroid sample topology");
			}
			std::vector<Core::CarrierVertex> result;
			result.reserve(a_indices.size() / 3U);
			for (std::size_t triangle = 0; triangle < a_indices.size(); triangle += 3) {
				const auto first = static_cast<std::size_t>(a_indices[triangle]);
				const auto second = static_cast<std::size_t>(a_indices[triangle + 1]);
				const auto third = static_cast<std::size_t>(a_indices[triangle + 2]);
				if (first >= a_vertices.size() || second >= a_vertices.size() ||
					third >= a_vertices.size()) {
					return std::unexpected("centroid sample index is out of range");
				}
				const auto& v0 = a_vertices[first];
				const auto& v1 = a_vertices[second];
				const auto& v2 = a_vertices[third];
				auto normal = Core::Normalize(
					v0.extrusionNormal + v1.extrusionNormal + v2.extrusionNormal);
				if (Core::LengthSquared(normal) <= 1.0e-12F) {
					normal = v0.extrusionNormal;
				}
				Core::CarrierVertex sample{
					(v0.position + v1.position + v2.position) * (1.0F / 3.0F),
					normal
				};
				if (!Core::IsFinite(sample.position) ||
					!Core::IsFinite(sample.extrusionNormal) ||
					Core::LengthSquared(sample.extrusionNormal) <= 1.0e-12F) {
					return std::unexpected("centroid sample is degenerate");
				}
				result.push_back(sample);
			}
			return result;
		}

		[[nodiscard]] const char* CarrierStateName(Game::CarrierState a_state) noexcept
		{
			switch (a_state) {
			case Game::CarrierState::kDetached: return "detached";
			case Game::CarrierState::kWaitingFor3D: return "waiting-for-3d";
			case Game::CarrierState::kWaitingForNakedBody: return "waiting-for-fur-source";
			case Game::CarrierState::kUnsupportedPerspective: return "unsupported-perspective";
			case Game::CarrierState::kSourceRejected: return "source-rejected";
			case Game::CarrierState::kCloneRejected: return "clone-rejected";
			case Game::CarrierState::kBuildRejected: return "build-rejected";
			case Game::CarrierState::kMarkerConflict: return "marker-conflict";
			case Game::CarrierState::kAttachedHidden: return "attached-hidden";
			case Game::CarrierState::kRendererReady: return "renderer-ready";
			case Game::CarrierState::kDetachFailed: return "detach-failed";
			default: return "unknown";
			}
		}

		[[nodiscard]] const char* FurSourceStatusName(
			Game::FurSourceStatus a_status) noexcept
		{
			switch (a_status) {
			case Game::FurSourceStatus::kEligible: return "eligible";
			case Game::FurSourceStatus::kKeywordUnresolved: return "keyword-unresolved";
			case Game::FurSourceStatus::kActorUnavailable: return "actor-unavailable";
			case Game::FurSourceStatus::kSkinUnavailable: return "skin-unavailable";
			case Game::FurSourceStatus::kSkinUnmarked: return "skin-unmarked";
			default: return "unknown";
			}
		}

		[[nodiscard]] const char* GeometryCloneStatusName(
			Game::GeometryCloneStatus a_status) noexcept
		{
			switch (a_status) {
			case Game::GeometryCloneStatus::kNotAttempted: return "not-attempted";
			case Game::GeometryCloneStatus::kCloned: return "cloned";
			case Game::GeometryCloneStatus::kInvalidInput: return "invalid-input";
			case Game::GeometryCloneStatus::kInvalidTransform: return "invalid-transform";
			case Game::GeometryCloneStatus::kEngineCloneFailed: return "engine-clone-failed";
			case Game::GeometryCloneStatus::kWrongCloneType: return "wrong-clone-type";
			case Game::GeometryCloneStatus::kCloneAlreadyParented: return "clone-already-parented";
			case Game::GeometryCloneStatus::kMissingShaderProperty: return "missing-shader-property";
			case Game::GeometryCloneStatus::kSharedShaderProperty: return "shared-shader-property";
			case Game::GeometryCloneStatus::kAlphaPropertyFailed: return "alpha-property-failed";
			case Game::GeometryCloneStatus::kInvalidSkinBinding: return "invalid-skin-binding";
			case Game::GeometryCloneStatus::kUnexpectedFailure: return "unexpected-failure";
			default: return "unknown";
			}
		}
	}

	Controller::Controller() :
		bodyLocator_(furSourcePolicy_),
		armorCoverageLocator_(furSourcePolicy_, armorBlacklist_),
		geometryCloner_(bindingValidator_),
		carrier_(bodyLocator_, geometryCloner_),
		renderRouter_(carrierRegistry_)
	{
		profile_.furLength = 1.0F;
	}

	Controller& Controller::GetSingleton() noexcept
	{
		static Controller singleton;
		return singleton;
	}

	void Controller::Initialize()
	{
		if (initialized_.load(std::memory_order_acquire)) {
			return;
		}

		if (const auto validation = profile_.Validate(); !validation.empty()) {
			throw std::runtime_error("invalid shell profile: " + validation);
		}
		if (!furSourcePolicy_.Resolve()) {
			const auto& config = furSourcePolicy_.Config();
			throw std::runtime_error(
				"required HasFur keyword was not found at " + config.pluginName +
				"|0x" + std::format("{:06X}", config.localFormID));
		}
		armorBlacklist_.Load();

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			throw std::runtime_error("Skyrim renderer is unavailable");
		}
		auto& rendererData = renderer->GetRuntimeData();
		if (!rendererData.forwarder || !rendererData.context) {
			throw std::runtime_error("Skyrim D3D11 device/context is unavailable");
		}

		renderRouter_.SetAdapter(std::make_shared<Render::NativeMultiDrawAdapter>());
		if (!PlayerEventSink::GetSingleton().Install()) {
			throw std::runtime_error("could not register player lifecycle event sinks");
		}

		auto& hooks = Render::RenderHooks::GetSingleton();
		{
			RendererLockGuard lock(*renderer);
			if (!hooks.InstallDrawHooks(rendererData.context, renderRouter_)) {
				throw std::runtime_error("could not install the indexed draw hooks");
			}
			if (!hooks.InstallPassScopes()) {
				(void)hooks.UninstallDrawHooks();
				throw std::runtime_error("could not install native shader pass scopes");
			}
		}

		initialized_.store(true, std::memory_order_release);
		SKSE::log::info(
			"Native shell backend ready: {} view shells, {} shadow shells, fur length {:.3f}",
			profile_.shellCount, profile_.shadowShellCount, profile_.furLength);
		SKSE::log::info(
			"D3D11 indexed draw routing ready: DrawIndexed and DrawIndexedInstanced");
		SKSE::log::info(
			"Fur source policy ready: ARMO keyword '{}' from {}|0x{:06X} resolved to runtime form 0x{:08X}",
			furSourcePolicy_.Keyword()->GetFormEditorID(),
			furSourcePolicy_.Config().pluginName,
			furSourcePolicy_.Config().localFormID,
			furSourcePolicy_.Keyword()->GetFormID());
		SKSE::log::info(
			"Uniform shell thresholds are active: 17 layers from 0/255 through 240/255 in 15/255 steps");
		SKSE::log::info(
			"Carrier scheduling base is active: engine layer 0 uses threshold 1/255; shader-pass fallback adds only outer layers");
		SKSE::log::info(
			"Shell pipeline correction is active: only LESS_EQUAL depth writes are overridden; Skyrim's blend/MRT masks, stencil and rasterizer remain untouched");
		SKSE::log::info("Shell fur length is {:.3f} model units", profile_.furLength);
		SKSE::log::info(
			"Under-skin armor recovery is active: inward depth {:.3f} model units",
			profile_.armorUnderSkinDepth);
		SKSE::log::info(
			"Adaptive armor boundaries are active for the 3BA SSE stream: local depth 3, exact re-skin sampling, R16 hard cap 65535 vertices");
		SKSE::log::info(
			"Pipeline diagnostics are active: one ORIGINAL-3BA and one CARRIER lighting snapshot will be written before shell state overrides");
		SKSE::log::info(
			"The active VS/PS are preserved; shell draws are enabled only for alpha-test techniques with a 16-byte PS b11 contract");
		QueuePlayerRefresh();
	}

	void Controller::QueuePlayerRefresh() noexcept
	{
		retryBudget_.store(60, std::memory_order_release);
		// The current call is attempt one. Four bounded retries tolerate the
		// interval where an equip/3D event precedes live GPU buffers without
		// turning one event into dozens of expensive BVH rebuilds.
		preparationRetryBudget_.store(4, std::memory_order_release);
		QueueRefreshTask();
	}

	void Controller::QueueArmorCoverageRefresh() noexcept
	{
		coverageRevision_.fetch_add(1, std::memory_order_acq_rel);
		QueuePlayerRefresh();
	}

	void Controller::QueuePlayerRebuild() noexcept
	{
		rebuildRequested_.store(true, std::memory_order_release);
		QueuePlayerRefresh();
	}

	void Controller::QueueRefreshTask() noexcept
	{
		if (!initialized_.load(std::memory_order_acquire) ||
			suspended_.load(std::memory_order_acquire)) {
			return;
		}
		const auto epoch = lifecycleEpoch_.load(std::memory_order_acquire);
		std::uint64_t empty{};
		if (!queuedEpoch_.compare_exchange_strong(
				empty, epoch, std::memory_order_acq_rel)) {
			return;
		}

		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			auto expected = epoch;
			(void)queuedEpoch_.compare_exchange_strong(
				expected, 0, std::memory_order_acq_rel);
			return;
		}

		tasks->AddTask([epoch]() {
			auto& controller = Controller::GetSingleton();
			auto expected = epoch;
			if (!controller.queuedEpoch_.compare_exchange_strong(
					expected, 0, std::memory_order_acq_rel) ||
				controller.suspended_.load(std::memory_order_acquire) ||
				controller.lifecycleEpoch_.load(std::memory_order_acquire) != epoch) {
				return;
			}
			controller.RefreshPlayer();
		});
	}

	void Controller::ScheduleRetry() noexcept
	{
		auto remaining = retryBudget_.load(std::memory_order_acquire);
		while (remaining != 0) {
			if (retryBudget_.compare_exchange_weak(
					remaining, remaining - 1, std::memory_order_acq_rel)) {
				QueueRefreshTask();
				return;
			}
		}
	}

	bool Controller::SchedulePreparationRetry() noexcept
	{
		auto remaining = preparationRetryBudget_.load(std::memory_order_acquire);
		while (remaining != 0) {
			if (preparationRetryBudget_.compare_exchange_weak(
					remaining, static_cast<std::uint8_t>(remaining - 1),
					std::memory_order_acq_rel)) {
				QueueRefreshTask();
				return true;
			}
		}
		return false;
	}

	void Controller::OnPlayer3DLoaded() noexcept
	{
		queuedEpoch_.store(0, std::memory_order_release);
		lifecycleEpoch_.fetch_add(1, std::memory_order_acq_rel);
		failedPreparationGeneration_ = 0;
		failedPreparationCoverageRevision_ = 0;
		if (gameLoadInProgress_.load(std::memory_order_acquire)) {
			return;
		}
		coverageRevision_.fetch_add(1, std::memory_order_acq_rel);
		suspended_.store(false, std::memory_order_release);
		QueuePlayerRefresh();
	}

	void Controller::OnPlayer3DReleased() noexcept
	{
		const auto epoch = lifecycleEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
		suspended_.store(true, std::memory_order_release);
		queuedEpoch_.store(0, std::memory_order_release);
		retryBudget_.store(0, std::memory_order_release);
		preparationRetryBudget_.store(0, std::memory_order_release);
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([epoch]() {
				auto& controller = Controller::GetSingleton();
				if (controller.lifecycleEpoch_.load(std::memory_order_acquire) != epoch ||
					!controller.suspended_.load(std::memory_order_acquire)) {
					return;
				}
				controller.carrierRegistry_.Clear();
				controller.preparedGeneration_ = 0;
				controller.preparedCoverageRevision_ = 0;
				controller.failedPreparationGeneration_ = 0;
				controller.failedPreparationCoverageRevision_ = 0;
				controller.carrier_.AbandonReleased3D();
				// A player-unloaded event may arrive after PostLoadGame. Do not
				// remain suspended forever if no matching loaded event follows.
				if (!controller.gameLoadInProgress_.load(std::memory_order_acquire)) {
					controller.suspended_.store(false, std::memory_order_release);
					SKSE::log::info(
						"Late player 3D release recovered; carrier reconstruction queued");
					controller.QueuePlayerRefresh();
				}
			});
		}
	}

	void Controller::OnPreLoadGame() noexcept
	{
		gameLoadInProgress_.store(true, std::memory_order_release);
		suspended_.store(true, std::memory_order_release);
		lifecycleEpoch_.fetch_add(1, std::memory_order_acq_rel);
		queuedEpoch_.store(0, std::memory_order_release);
		retryBudget_.store(0, std::memory_order_release);
		preparationRetryBudget_.store(0, std::memory_order_release);
		carrierRegistry_.Clear();
		preparedGeneration_ = 0;
		preparedCoverageRevision_ = 0;
		failedPreparationGeneration_ = 0;
		failedPreparationCoverageRevision_ = 0;
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto detached = carrier_.Detach(player);
		if (detached.state == Game::CarrierState::kDetachFailed) {
			(void)carrier_.SetRendererReady(detached.generation, false);
		}
	}

	void Controller::OnGameReady() noexcept
	{
		queuedEpoch_.store(0, std::memory_order_release);
		lifecycleEpoch_.fetch_add(1, std::memory_order_acq_rel);
		gameLoadInProgress_.store(false, std::memory_order_release);
		suspended_.store(false, std::memory_order_release);
		failedPreparationGeneration_ = 0;
		failedPreparationCoverageRevision_ = 0;
		lastReportedFurSourceStatus_ = 0xFF;
		lastReportedSkinFormID_ = 0;
		lastReportedMarkedArmorCount_ = 0xFFFFFFFFU;
		coverageRevision_.fetch_add(1, std::memory_order_acq_rel);
		QueuePlayerRefresh();
	}

	void Controller::RefreshPlayer() noexcept
	{
		if (!initialized_.load(std::memory_order_acquire)) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (rebuildRequested_.exchange(false, std::memory_order_acq_rel)) {
			carrierRegistry_.Clear();
			preparedGeneration_ = 0;
			preparedCoverageRevision_ = 0;
			coverageRevision_.fetch_add(1, std::memory_order_acq_rel);
			const auto detached = carrier_.Detach(player);
			if (detached.state == Game::CarrierState::kDetachFailed) {
				rebuildRequested_.store(true, std::memory_order_release);
				(void)carrier_.SetRendererReady(detached.generation, false);
				ScheduleRetry();
				return;
			}
		}

		const auto furSource = furSourcePolicy_.Evaluate(player);
		const auto furSourceStatus = static_cast<std::uint8_t>(furSource.status);
		if (furSourceStatus != lastReportedFurSourceStatus_ ||
			furSource.skinFormID != lastReportedSkinFormID_ ||
			furSource.markedArmorCount != lastReportedMarkedArmorCount_) {
			SKSE::log::info(
				"Player fur source state: {} (skin 0x{:08X}, skin HasFur {}, marked active ARMO {})",
				FurSourceStatusName(furSource.status), furSource.skinFormID,
				furSource.skinMarked, furSource.markedArmorCount);
			lastReportedFurSourceStatus_ = furSourceStatus;
			lastReportedSkinFormID_ = furSource.skinFormID;
			lastReportedMarkedArmorCount_ = furSource.markedArmorCount;
		}

		if (!furSource.IsEligible()) {
			carrierRegistry_.Clear();
			preparedGeneration_ = 0;
			preparedCoverageRevision_ = 0;
			failedPreparationGeneration_ = 0;
			failedPreparationCoverageRevision_ = 0;
			if (carrier_.IsAttached()) {
				const auto detached = carrier_.Detach(player);
				if (detached.state == Game::CarrierState::kDetachFailed) {
					(void)carrier_.SetRendererReady(detached.generation, false);
					ScheduleRetry();
					return;
				}
			}
			if (furSource.status == Game::FurSourceStatus::kActorUnavailable ||
				furSource.status == Game::FurSourceStatus::kSkinUnavailable) {
				ScheduleRetry();
			}
			return;
		}

		const auto update = carrier_.Ensure(player);
		const auto stateValue = static_cast<std::uint8_t>(update.state);
		if (stateValue != lastReportedState_ || update.changed) {
			SKSE::log::info(
				"Player carrier state: {} (generation {}, geometries {}, body {}, clone {} [{}], binding {})",
				CarrierStateName(update.state), update.generation, update.geometryCount,
				static_cast<std::uint32_t>(update.bodyStatus),
				static_cast<std::uint32_t>(update.cloneStatus),
				GeometryCloneStatusName(update.cloneStatus),
				static_cast<std::uint32_t>(update.bindingStatus));
			lastReportedState_ = stateValue;
		}

		if (!carrier_.IsAttached()) {
			if (update.state == Game::CarrierState::kWaitingFor3D ||
				update.state == Game::CarrierState::kCloneRejected ||
				update.state == Game::CarrierState::kDetachFailed ||
				update.bodyStatus == Game::BodyGeometryStatus::kBodySlotUnavailable ||
				update.bodyStatus == Game::BodyGeometryStatus::kBodyCovered) {
				ScheduleRetry();
			}
			return;
		}
		const auto coverageRevision = coverageRevision_.load(std::memory_order_acquire);
		if (preparedGeneration_ == update.generation &&
			preparedCoverageRevision_ == coverageRevision &&
			carrierRegistry_.Size() != 0) {
			return;
		}
		if (failedPreparationGeneration_ == update.generation &&
			failedPreparationCoverageRevision_ == coverageRevision) {
			return;
		}

		const auto registeredPartitions = carrierRegistry_.Size();
		const bool coverageOnlyRefresh =
			preparedGeneration_ == update.generation && registeredPartitions != 0;
		if (!coverageOnlyRefresh) {
			(void)carrier_.SetRendererReady(update.generation, false);
			carrierRegistry_.Clear();
			preparedGeneration_ = 0;
			preparedCoverageRevision_ = 0;
		}
		if (!PrepareCarrier(update.generation, coverageRevision)) {
			if (coverageOnlyRefresh) {
				SKSE::log::error(
					"Dynamic armor coverage revision {} failed; the previous visible carrier sidecars were retained",
					coverageRevision);
			} else {
				SKSE::log::error(
					"Carrier generation {} stayed hidden because its native sidecar could not be prepared",
					update.generation);
			}
			if (SchedulePreparationRetry()) {
				SKSE::log::warn(
					"Carrier preparation failure may be transient; a bounded retry was queued");
				return;
			}
			failedPreparationGeneration_ = update.generation;
			failedPreparationCoverageRevision_ = coverageRevision;
			return;
		}

		preparedGeneration_ = update.generation;
		preparedCoverageRevision_ = coverageRevision;
		failedPreparationGeneration_ = 0;
		failedPreparationCoverageRevision_ = 0;
		if (!coverageOnlyRefresh &&
			!carrier_.SetRendererReady(update.generation, true)) {
			carrierRegistry_.Clear();
			preparedGeneration_ = 0;
			preparedCoverageRevision_ = 0;
			SKSE::log::error("Carrier generation changed before it could be revealed");
			return;
		}

		if (coverageOnlyRefresh) {
			SKSE::log::info(
				"Dynamic armor coverage revision {} replaced {} carrier partition sidecar(s) without rebuilding the carrier",
				coverageRevision, carrierRegistry_.Size());
		} else {
			SKSE::log::info(
				"Carrier generation {} is visible through {} registered native partition(s)",
				update.generation, carrierRegistry_.Size());
		}
		retryBudget_.store(0, std::memory_order_release);
		preparationRetryBudget_.store(0, std::memory_order_release);
	}

	bool Controller::PrepareCarrier(
		std::uint64_t a_generation,
		std::uint64_t a_coverageRevision) noexcept
	{
		try {
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			if (!renderer) {
				return false;
			}
			auto& rendererData = renderer->GetRuntimeData();
			auto* context = rendererData.context;
			if (!rendererData.forwarder || !context) {
				return false;
			}
			{
				// Newer runtimes and renderer wrappers may replace the public
				// immediate context after DataLoaded. Revalidate that route at the
				// same lifecycle edge where live carrier buffers become available.
				RendererLockGuard routingLock(*renderer);
				if (!Render::RenderHooks::GetSingleton().InstallDrawHooks(
						context, renderRouter_)) {
					SKSE::log::error(
						"Could not refresh D3D11 draw routing for renderer context {}",
						static_cast<const void*>(context));
					return false;
				}
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* skeletonRoot = carrier_.SkeletonRoot();
			if (!skeletonRoot) {
				return false;
			}
			const auto armorSnapshot = armorCoverageLocator_.Locate(player);
			if (armorSnapshot.skeletonRoot &&
				armorSnapshot.skeletonRoot != skeletonRoot) {
				SKSE::log::warn(
					"Armor coverage snapshot belongs to a different player skeleton; retrying");
				return false;
			}

			std::unordered_set<std::string> carrierDiffusePaths;
			std::unordered_set<std::string> carrierGeometryNames;
			for (const auto& carrierGeometry : carrier_.Geometries()) {
				if (carrierGeometry) {
					auto path = Game::GeometryDiffuseTexturePath(*carrierGeometry);
					if (!path.empty()) {
						carrierDiffusePaths.insert(std::move(path));
					}
					if (carrierGeometry->name.c_str() &&
						*carrierGeometry->name.c_str() != '\0') {
						carrierGeometryNames.emplace(carrierGeometry->name.c_str());
					}
				}
			}

			std::vector<Core::Triangle> armorTriangles;
			std::size_t skippedBodyGeometry{};
			std::size_t skippedArmorGeometry{};
			if (armorSnapshot.skeletonRoot) {
				RendererLockGuard snapshotLock(*renderer);
				for (const auto& located : armorSnapshot.geometries) {
					auto* geometry = located.geometry.get();
					if (!geometry) {
						continue;
					}
					const bool matchesCarrierDiffuse = !located.diffusePath.empty() &&
						carrierDiffusePaths.contains(located.diffusePath);
					const bool matchesCarrierName = geometry->name.c_str() &&
						*geometry->name.c_str() != '\0' &&
						carrierGeometryNames.contains(geometry->name.c_str());
					if (matchesCarrierDiffuse || matchesCarrierName) {
						++skippedBodyGeometry;
						SKSE::log::info(
							"Ignoring embedded-body armor geometry '{}' ARMO 0x{:08X} (carrier {} match)",
							geometry->name.c_str(), located.armorFormID,
							matchesCarrierDiffuse ? "diffuse" : "name");
						continue;
					}

					auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
					auto* skinPartition = skin ? skin->skinPartition.get() : nullptr;
					if (skinPartition && skinPartition->numPartitions != 0 &&
						skinPartition->numPartitions <= skinPartition->partitions.size()) {
						for (std::uint32_t partitionIndex = 0;
							partitionIndex < skinPartition->numPartitions;
							++partitionIndex) {
							auto captured = CaptureSkinnedPartition(
								*geometry, *armorSnapshot.skeletonRoot,
								skinPartition->partitions[partitionIndex], partitionIndex,
								skinPartition->vertexCount, context);
							if (!captured) {
								++skippedArmorGeometry;
								SKSE::log::warn(
									"Skipping armor coverage geometry '{}' ARMO 0x{:08X} partition {}: {}",
									geometry->name.c_str(), located.armorFormID,
									partitionIndex, captured.error());
								continue;
							}
							AppendTriangles(*captured, armorTriangles);
						}
					} else {
						auto captured = CaptureRigidGeometry(
							*geometry, *armorSnapshot.skeletonRoot, context);
						if (!captured) {
							++skippedArmorGeometry;
							SKSE::log::warn(
								"Skipping rigid armor coverage geometry '{}' ARMO 0x{:08X}: {}",
								geometry->name.c_str(), located.armorFormID,
								captured.error());
							continue;
						}
						AppendTriangles(*captured, armorTriangles);
					}
				}
			}

			SKSE::log::info(
				"Dynamic armor coverage revision {}: {} equipped ARMO, {} blacklisted ARMO, {} live geometries, {} triangles ({} embedded body geometries filtered, {} unsupported partitions skipped)",
				a_coverageRevision, armorSnapshot.armorCount,
				armorSnapshot.blacklistedArmorCount,
				armorSnapshot.geometries.size(), armorTriangles.size(),
				skippedBodyGeometry, skippedArmorGeometry);

			std::optional<Core::ArmorCoverageQuery> armorQuery;
			if (!armorTriangles.empty()) {
				auto created = Core::ArmorCoverageQuery::Create(profile_, armorTriangles);
				if (!created) {
					SKSE::log::error(
						"Could not build the shared armor coverage BVH: {}", created.error());
					return false;
				}
				armorQuery.emplace(std::move(*created));
			}

			std::vector<Render::CarrierRegistration> staged;
			REX::W32::ID3D11DeviceContext* registeredSourceContext{};
			for (const auto& geometryPointer : carrier_.Geometries()) {
				auto* geometry = geometryPointer.get();
				if (!geometry) {
					return false;
				}

				auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
				auto* skinPartition = skin ? skin->skinPartition.get() : nullptr;
				if (!skinPartition || skinPartition->numPartitions == 0 ||
					skinPartition->numPartitions > skinPartition->partitions.size() ||
					skinPartition->vertexCount == 0) {
					return false;
				}

				for (std::uint32_t index = 0; index < skinPartition->numPartitions; ++index) {
					const auto& partition = skinPartition->partitions[index];
					auto* buffers = partition.buffData;
					if (!buffers || !buffers->vertexBuffer || !buffers->indexBuffer ||
						partition.triangles == 0) {
						return false;
					}

					auto vertexDesc = buffers->vertexDesc;
					const auto vertexStride = vertexDesc.GetSize();
					if (!vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL)) {
						SKSE::log::info(
							"Carrier geometry {} partition {} has no packed normals; reconstructing extrusion normals from topology",
							static_cast<const void*>(geometry), index);
					}
					auto partitionVertexDesc = partition.vertexDesc;
					if (std::bit_cast<std::uint64_t>(partitionVertexDesc) !=
						std::bit_cast<std::uint64_t>(vertexDesc)) {
						return false;
					}
					const auto vertexCount = skinPartition->vertexCount;
					const auto vertexBytes = static_cast<std::size_t>(vertexCount) * vertexStride;
					if (vertexStride == 0 || vertexBytes == 0) {
						return false;
					}

					auto* vertexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(
						buffers->vertexBuffer);
					auto* indexBuffer = reinterpret_cast<REX::W32::ID3D11Buffer*>(
						buffers->indexBuffer);
					REX::W32::ComPtr<REX::W32::ID3D11Device> sourceDevice;
					vertexBuffer->GetDevice(sourceDevice.ReleaseAndGetAddressOf());
					auto* device = sourceDevice.Get();
					if (!device) {
						return false;
					}
					REX::W32::ComPtr<REX::W32::ID3D11DeviceContext> sourceContext;
					device->GetImmediateContext(sourceContext.ReleaseAndGetAddressOf());
					if (!sourceContext.Get()) {
						return false;
					}
					if (sourceContext.Get() != registeredSourceContext) {
						RendererLockGuard routingLock(*renderer);
						if (!Render::RenderHooks::GetSingleton().InstallAdditionalDrawContext(
								sourceContext.Get())) {
							SKSE::log::error(
								"Could not register source-device D3D11 context {}",
								static_cast<const void*>(sourceContext.Get()));
							return false;
						}
						registeredSourceContext = sourceContext.Get();
					}
					if (device != rendererData.forwarder) {
						SKSE::log::info(
							"Carrier buffers belong to device {} while the renderer exposes {}; using the source-buffer device for wrapper compatibility",
							static_cast<const void*>(device),
							static_cast<const void*>(rendererData.forwarder));
					}
					REX::W32::D3D11_BUFFER_DESC vertexDescription{};
					vertexBuffer->GetDesc(&vertexDescription);
					const auto duplicate = std::ranges::find_if(
						staged,
						[geometry, vertexBuffer, indexBuffer](const auto& a_registration) {
							const auto& sidecar = a_registration.sidecar;
							return sidecar && sidecar->geometry == geometry &&
								sidecar->sourceVertexBuffer.Get() == vertexBuffer &&
								sidecar->sourceIndexBuffer.Get() == indexBuffer;
						});
					if (duplicate != staged.end()) {
						continue;
					}
					const auto reusable = std::ranges::find_if(
						staged,
						[geometry, vertexBuffer](const auto& a_registration) {
							const auto& sidecar = a_registration.sidecar;
							return sidecar && sidecar->geometry == geometry &&
								sidecar->sourceVertexBuffer.Get() == vertexBuffer;
						});
					const std::shared_ptr<const Render::NativeCarrierSidecar>
						sharedVertexResources =
							reusable != staged.end() &&
							vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL) ?
							reusable->sidecar : nullptr;

					REX::W32::D3D11_BUFFER_DESC indexDescription{};
					indexBuffer->GetDesc(&indexDescription);
					const auto topologyIndexCount =
						static_cast<std::size_t>(partition.triangles) * 3U;
					const auto topologyIndexBytes =
						topologyIndexCount * sizeof(std::uint16_t);
					if (topologyIndexCount == 0 ||
						topologyIndexBytes > indexDescription.byteWidth ||
						(indexDescription.bindFlags & REX::W32::D3D11_BIND_INDEX_BUFFER) == 0) {
						return false;
					}

					std::expected<PosedPartitionSnapshot, std::string> captured =
						std::unexpected("carrier snapshot was not attempted");
					{
						// CopyResource/Map touches the immediate context, so only this short
						// snapshot is serialized with Skyrim's renderer. BVH construction and
						// immutable sidecar creation happen after the lock is released.
						RendererLockGuard snapshotLock(*renderer);
						captured = CaptureSkinnedPartition(
							*geometry, *skeletonRoot, partition, index,
							vertexCount, context);
					}
					if (!captured) {
						SKSE::log::error(
							"Carrier geometry '{}' partition {} snapshot failed: {}",
							geometry->name.c_str(), index, captured.error());
						return false;
					}

					std::optional<Core::CoverageField> coverage;
					std::optional<Generator::AdaptiveShellMeshResult> refinedMesh;
					std::optional<Generator::CompactShellIndexMaskResult> compactedMesh;
					if (armorQuery) {
						auto solved = armorQuery->Sample(captured->posedVertices);
						if (!solved) {
							SKSE::log::error(
								"Armor coverage solve failed for carrier '{}' partition {}: {}",
								geometry->name.c_str(), index, solved.error());
							return false;
						}
						coverage = std::move(*solved);

						const auto descriptorBits = std::bit_cast<std::uint64_t>(vertexDesc);
						const auto influences = static_cast<std::uint32_t>(
							partition.bonesPerVertex);
						if (descriptorBits == kAdaptive3BaVertexDescriptor &&
							influences >= 1 && influences <= 4) {
							auto generated = Generator::GenerateAdaptiveShellMesh({
								.sourceVertices = captured->rawVertices,
								.sourceIndices = captured->rawIndices,
								.referenceVertices = captured->referenceVertices,
								.posedVertices = captured->posedVertices,
								.query = std::addressof(*armorQuery),
								.vertexStride = vertexStride,
								.positionOffset = vertexDesc.GetAttributeOffset(
									RE::BSGraphics::Vertex::VA_POSITION),
								.uvOffset = vertexDesc.GetAttributeOffset(
									RE::BSGraphics::Vertex::VA_TEXCOORD0),
								.skinningOffset = vertexDesc.GetAttributeOffset(
									RE::BSGraphics::Vertex::VA_SKINNING),
								.influencesPerVertex = influences
							});
							if (generated && generated->stats.topologyChanged) {
								refinedMesh.emplace(std::move(*generated));
								auto posedRefined = Game::PoseVertices({
									.geometry = geometry,
									.skeletonRoot = skeletonRoot,
									.partition = std::addressof(partition),
									.packedVertices = refinedMesh->packedVertices,
									.sourceIndices = refinedMesh->indices,
									.referenceVertices = refinedMesh->referenceVertices,
									.vertexStride = vertexStride,
									.skinningOffset = vertexDesc.GetAttributeOffset(
										RE::BSGraphics::Vertex::VA_SKINNING)
								});
								if (!posedRefined) {
									SKSE::log::warn(
										"Adaptive carrier '{}' partition {} could not be re-skinned ({}); using binary coverage",
										geometry->name.c_str(), index, posedRefined.error());
									refinedMesh.reset();
								} else {
									auto refinedCoverage = armorQuery->Sample(*posedRefined);
									auto centroids = BuildTriangleCentroidSamples(
										*posedRefined, refinedMesh->indices);
									if (!refinedCoverage || !centroids) {
										SKSE::log::warn(
											"Adaptive carrier '{}' partition {} final samples failed; using binary coverage",
											geometry->name.c_str(), index);
										refinedMesh.reset();
									} else {
										auto centroidCoverage = armorQuery->Sample(*centroids);
										if (!centroidCoverage) {
											SKSE::log::warn(
												"Adaptive carrier '{}' partition {} centroid coverage failed ({}); using binary coverage",
												geometry->name.c_str(), index,
												centroidCoverage.error());
											refinedMesh.reset();
										} else {
											auto compacted = Generator::GenerateCompactShellIndexMask({
												.sourceIndices = refinedMesh->indices,
												.vertexCoverage = refinedCoverage->Values(),
												.triangleCoverage = centroidCoverage->Values(),
												.vertexCount = static_cast<std::uint32_t>(
													refinedMesh->referenceVertices.size()),
												.shellCount = profile_.shellCount
											});
											if (!compacted) {
												SKSE::log::warn(
													"Adaptive carrier '{}' partition {} compaction failed; using binary coverage",
													geometry->name.c_str(), index);
												refinedMesh.reset();
											} else {
												compactedMesh.emplace(std::move(*compacted));
												const bool actuallyChanged =
													refinedMesh->stats.coarseHiddenSourceTriangleCount != 0 ||
													compactedMesh->hiddenTriangleCount != 0;
												if (!actuallyChanged) {
													refinedMesh.reset();
													compactedMesh.reset();
												}
											}
										}
									}
								}
							} else if (!generated) {
								SKSE::log::warn(
									"Adaptive boundary unavailable for carrier '{}' partition {} ({}); using safe binary coverage",
									geometry->name.c_str(), index,
									AdaptiveMeshErrorName(generated.error()));
							}
						} else {
							SKSE::log::info(
								"Carrier '{}' partition {} descriptor {:016X} is outside the adaptive 3BA contract; using binary coverage",
								geometry->name.c_str(), index, descriptorBits);
						}
					}

					const bool useRefinedMesh = refinedMesh.has_value() &&
						compactedMesh.has_value();
					if (useRefinedMesh) {
						SKSE::log::info(
							"Adaptive armor boundary on '{}' partition {}: depth {}, {} source triangles refined, {} vertices added, {} candidate micro-triangles, {} removed after exact pose, {} visible",
							geometry->name.c_str(), index,
							refinedMesh->stats.usedDepth,
							refinedMesh->stats.refinedSourceTriangleCount,
							refinedMesh->stats.addedVertexCount,
							refinedMesh->stats.candidateTriangleCount,
							compactedMesh->hiddenTriangleCount +
								refinedMesh->stats.coarseHiddenSourceTriangleCount,
							compactedMesh->visibleTriangleCount);
					}

					const auto effectiveSharedVertexResources = useRefinedMesh ?
						std::shared_ptr<const Render::NativeCarrierSidecar>{} :
						sharedVertexResources;
					const std::span<const std::byte> rawVertices =
						effectiveSharedVertexResources ? std::span<const std::byte>{} :
						std::span<const std::byte>{ captured->rawVertices };
					const std::span<const std::uint16_t> rawIndices{
						captured->rawIndices
					};

					auto sidecar = Render::BuildNativeSidecar({
						.geometry = geometry,
						.device = device,
						.sourceVertexBuffer = vertexBuffer,
						.sourceIndexBuffer = indexBuffer,
						.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
						.vertexDesc = vertexDesc,
						.rawVertexData = rawVertices,
						.rawIndexData = rawIndices,
						.shellVertexData = useRefinedMesh ?
							std::span<const std::byte>{ refinedMesh->packedVertices } :
							std::span<const std::byte>{},
						.shellIndexData = useRefinedMesh ?
							std::span<const std::uint16_t>{ compactedMesh->indices } :
							std::span<const std::uint16_t>{},
						.shellCarrierVertices = useRefinedMesh ?
							std::span<const Core::CarrierVertex>{ refinedMesh->referenceVertices } :
							std::span<const Core::CarrierVertex>{},
						.shellVertexCount = useRefinedMesh ?
							static_cast<std::uint32_t>(refinedMesh->referenceVertices.size()) : 0U,
						.coverage = !useRefinedMesh && coverage ? coverage->Values() :
							std::span<const Core::FirstHiddenShell>{},
						.sharedVertexResources = effectiveSharedVertexResources,
						.profile = profile_,
						.vertexCount = vertexCount,
						.vertexStride = vertexStride,
						.sourceVertexOffset = 0,
						.sourceIndexOffset = 0,
						.geometryRevision =
							(a_generation << 32U) ^ a_coverageRevision,
						.forceVisibleAlpha = false,
						.correctShellPipeline = true
					});
					if (!sidecar) {
						SKSE::log::error(
							"Sidecar build rejected geometry {} partition {}: {} ({}) [descriptor {:016X}, flags {:03X}, stride {}, vertices {} / VB {} bytes, indices {} / IB {} bytes, source device {}, renderer device {}]",
							static_cast<const void*>(geometry), index,
							SidecarBuildErrorName(sidecar.error()),
							static_cast<std::uint32_t>(sidecar.error()),
							std::bit_cast<std::uint64_t>(vertexDesc),
							static_cast<std::uint16_t>(vertexDesc.GetFlags()),
							vertexStride, vertexCount, vertexDescription.byteWidth,
							topologyIndexCount, indexDescription.byteWidth,
							static_cast<const void*>(device),
							static_cast<const void*>(rendererData.forwarder));
						return false;
					}
					if ((*sidecar)->hiddenTriangleCount != 0) {
						SKSE::log::info(
							"Dynamic armor coverage hid {}/{} carrier triangles on '{}' partition {}",
							(*sidecar)->hiddenTriangleCount, partition.triangles,
							geometry->name.c_str(), index);
					}
					staged.push_back({
						.sidecar = std::move(*sidecar),
						.viewShellCount = profile_.shellCount,
						.shadowShellCount = profile_.shadowShellCount,
						.stateRevision = a_generation,
						.enabled = true
					});
				}
			}

			if (staged.empty()) {
				return false;
			}

			return carrierRegistry_.ReplaceAll(staged) &&
				carrierRegistry_.Size() == staged.size();
		} catch (const std::exception& error) {
			SKSE::log::error("Carrier preparation failed: {}", error.what());
		} catch (...) {
			SKSE::log::error("Carrier preparation failed with an unknown exception");
		}

		return false;
	}
}
