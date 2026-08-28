#include "DynamicFur/Render/RenderBackend.h"

#include "DynamicFur/Generator/ShellVertexGenerator.h"
#include "DynamicFur/Generator/ShellIndexMask.h"
#include "DynamicFur/Core/ShellMath.h"

#include "RE/B/BSBatchRenderer.h"
#include "RE/B/BSLightingShader.h"
#include "RE/B/BSRenderPass.h"
#include "RE/B/BSShader.h"
#include "RE/B/BSUtilityShader.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

namespace DynamicFur::Render
{
	namespace
	{
		thread_local const FurPassContext* currentPassContext{};

		[[nodiscard]] bool HasUtilityFlag(
			std::uint32_t a_technique,
			RE::BSUtilityShader::Flags a_flag) noexcept
		{
			return (a_technique & static_cast<std::uint32_t>(a_flag)) != 0;
		}

		[[nodiscard]] bool HasLightingFlag(
			std::uint32_t a_technique,
			RE::BSLightingShader::TechniqueFlag a_flag) noexcept
		{
			return (a_technique & static_cast<std::uint32_t>(a_flag)) != 0;
		}

		[[nodiscard]] std::size_t HashPointer(const void* a_pointer) noexcept
		{
			return std::hash<const void*>{}(a_pointer);
		}

		[[nodiscard]] bool AdditionFits(
			std::uint32_t a_lhs,
			std::size_t a_rhs,
			std::uint32_t a_limit) noexcept
		{
			return a_rhs <= a_limit &&
				a_lhs <= a_limit - static_cast<std::uint32_t>(a_rhs);
		}

		[[nodiscard]] std::uint64_t HashCombine(
			std::uint64_t a_hash,
			std::uint64_t a_value) noexcept
		{
			constexpr std::uint64_t prime = 1099511628211ULL;
			for (std::size_t index = 0; index < sizeof(a_value); ++index) {
				a_hash ^= (a_value >> (index * 8U)) & 0xFFU;
				a_hash *= prime;
			}
			return a_hash;
		}

		[[nodiscard]] std::uint64_t ProfileFingerprint(
			const Core::ShellProfile& a_profile) noexcept
		{
			std::uint64_t hash = 14695981039346656037ULL;
			hash = HashCombine(hash, a_profile.shellCount);
			hash = HashCombine(hash, std::bit_cast<std::uint32_t>(a_profile.furLength));
			hash = HashCombine(
				hash, std::bit_cast<std::uint32_t>(a_profile.distributionExponent));
			hash = HashCombine(
				hash, std::bit_cast<std::uint32_t>(a_profile.baseAlphaReference));
			for (std::uint32_t layer = 0; layer < a_profile.shellCount; ++layer) {
				hash = HashCombine(
					hash, std::bit_cast<std::uint32_t>(a_profile.alphaThresholds[layer]));
			}
			return hash;
		}

		[[nodiscard]] bool BelongsToDevice(
			REX::W32::ID3D11Buffer* a_buffer,
			REX::W32::ID3D11Device* a_device) noexcept
		{
			if (!a_buffer || !a_device) {
				return false;
			}
			REX::W32::ComPtr<REX::W32::ID3D11Device> owner;
			a_buffer->GetDevice(owner.ReleaseAndGetAddressOf());
			return owner.Get() == a_device;
		}

		[[nodiscard]] bool IsSupportedPass(FurPassKind a_kind) noexcept
		{
			switch (a_kind) {
			case FurPassKind::Lighting:
			case FurPassKind::Depth:
			case FurPassKind::Normal:
			case FurPassKind::ShadowMap:
			case FurPassKind::ShadowMask:
				return true;
			default:
				return false;
			}
		}
	}

	bool NativeCarrierSidecar::IsValid() const noexcept
	{
		if (!geometry || !sourceVertexBuffer.Get() || !sourceIndexBuffer.Get() ||
			!expandedVertexBuffer.Get() || vertexStride == 0 || sourceVertexCount == 0 ||
			sourceIndexCount == 0 || shellVertexCount == 0 || shellIndexCount == 0 ||
			profileShellCount == 0 || profileShellCount > Core::kMaximumShellCount) {
			return false;
		}
		if (correctShellPipeline && !shellDepthState.Get()) {
			return false;
		}
		if ((hiddenTriangleCount != 0 || refinedShellTopology) && !shellIndexBuffer.Get()) {
			return false;
		}

		for (std::uint32_t layer = 0; layer < profileShellCount; ++layer) {
			if (!alphaReferenceBuffers[layer].Get()) {
				return false;
			}
		}
		return true;
	}

	bool PreparedShellDraw::IsValid() const noexcept
	{
		if (!expandedVertexBuffer.Get() || vertexBufferSlot >= 32 ||
			vertexStride == 0 || indexCount == 0 ||
			layerCount == 0 || layerCount > layers.size()) {
			return false;
		}
		for (std::uint32_t layer = 0; layer < layerCount; ++layer) {
			if (!layers[layer].alphaReferenceBuffer.Get()) {
				return false;
			}
		}
		return true;
	}

	std::size_t CarrierRegistry::KeyHash::operator()(const Key& a_key) const noexcept
	{
		auto hash = HashPointer(a_key.geometry);
		hash ^= HashPointer(a_key.vertexBuffer) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
		hash ^= HashPointer(a_key.indexBuffer) + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
		return hash;
	}

	RegistrationResult CarrierRegistry::Register(const CarrierRegistration& a_registration)
	{
		const auto& sidecar = a_registration.sidecar;
		if (!sidecar || !sidecar->IsValid() ||
			a_registration.viewShellCount == 0 ||
			a_registration.viewShellCount > sidecar->profileShellCount ||
			a_registration.shadowShellCount == 0 ||
			a_registration.shadowShellCount > sidecar->profileShellCount) {
			return RegistrationResult::Invalid;
		}

		const Key key{
			sidecar->geometry,
			sidecar->sourceVertexBuffer.Get(),
			sidecar->sourceIndexBuffer.Get()
		};

		std::unique_lock lock(mutex_);
		const auto [iterator, added] = entries_.insert_or_assign(key, a_registration);
		static_cast<void>(iterator);
		if (added) {
			++geometryCounts_[sidecar->geometry];
		}
		return added ? RegistrationResult::Added : RegistrationResult::Replaced;
	}

	bool CarrierRegistry::ReplaceAll(
		std::span<const CarrierRegistration> a_registrations)
	{
		if (a_registrations.empty()) {
			return false;
		}

		try {
			std::unordered_map<Key, CarrierRegistration, KeyHash> replacement;
			std::unordered_map<RE::BSGeometry*, std::size_t> replacementCounts;
			replacement.reserve(a_registrations.size());
			for (const auto& registration : a_registrations) {
				const auto& sidecar = registration.sidecar;
				if (!sidecar || !sidecar->IsValid() ||
					registration.viewShellCount == 0 ||
					registration.viewShellCount > sidecar->profileShellCount ||
					registration.shadowShellCount == 0 ||
					registration.shadowShellCount > sidecar->profileShellCount) {
					return false;
				}
				const Key key{
					sidecar->geometry,
					sidecar->sourceVertexBuffer.Get(),
					sidecar->sourceIndexBuffer.Get()
				};
				if (!replacement.emplace(key, registration).second) {
					return false;
				}
				++replacementCounts[sidecar->geometry];
			}

			std::unique_lock lock(mutex_);
			entries_.swap(replacement);
			geometryCounts_.swap(replacementCounts);
			return true;
		} catch (...) {
			return false;
		}
	}

	bool CarrierRegistry::UpdateState(
		RE::BSGeometry* a_geometry,
		REX::W32::ID3D11Buffer* a_vertexBuffer,
		REX::W32::ID3D11Buffer* a_indexBuffer,
		std::uint32_t a_viewShellCount,
		std::uint32_t a_shadowShellCount,
		bool a_enabled,
		std::uint64_t a_stateRevision)
	{
		if (!a_geometry || !a_vertexBuffer || !a_indexBuffer) {
			return false;
		}

		std::unique_lock lock(mutex_);
		const auto iterator = entries_.find({ a_geometry, a_vertexBuffer, a_indexBuffer });
		if (iterator == entries_.end()) {
			return false;
		}

		const auto profileCount = iterator->second.sidecar->profileShellCount;
		if (a_viewShellCount == 0 || a_viewShellCount > profileCount ||
			a_shadowShellCount == 0 || a_shadowShellCount > profileCount) {
			return false;
		}

		iterator->second.viewShellCount = a_viewShellCount;
		iterator->second.shadowShellCount = a_shadowShellCount;
		iterator->second.enabled = a_enabled;
		iterator->second.stateRevision = a_stateRevision;
		return true;
	}

	bool CarrierRegistry::Unregister(
		RE::BSGeometry* a_geometry,
		REX::W32::ID3D11Buffer* a_vertexBuffer,
		REX::W32::ID3D11Buffer* a_indexBuffer)
	{
		std::unique_lock lock(mutex_);
		const auto iterator = entries_.find({ a_geometry, a_vertexBuffer, a_indexBuffer });
		if (iterator == entries_.end()) {
			return false;
		}
		entries_.erase(iterator);
		if (const auto count = geometryCounts_.find(a_geometry);
			count != geometryCounts_.end() && --count->second == 0) {
			geometryCounts_.erase(count);
		}
		return true;
	}

	std::size_t CarrierRegistry::UnregisterGeometry(RE::BSGeometry* a_geometry)
	{
		std::unique_lock lock(mutex_);
		std::size_t removed{};
		for (auto iterator = entries_.begin(); iterator != entries_.end();) {
			if (iterator->first.geometry == a_geometry) {
				iterator = entries_.erase(iterator);
				++removed;
			} else {
				++iterator;
			}
		}
		geometryCounts_.erase(a_geometry);
		return removed;
	}

	void CarrierRegistry::Clear()
	{
		std::unique_lock lock(mutex_);
		entries_.clear();
		geometryCounts_.clear();
	}

	std::size_t CarrierRegistry::Size() const noexcept
	{
		try {
			std::shared_lock lock(mutex_);
			return entries_.size();
		} catch (...) {
			return 0;
		}
	}

	CarrierResolveResult CarrierRegistry::Resolve(
		const CarrierLookup& a_lookup) const noexcept
	{
		try {
			if (!a_lookup.geometry) {
				return {};
			}

			std::shared_lock lock(mutex_);
			const bool knownGeometry = geometryCounts_.contains(a_lookup.geometry);
			if (!a_lookup.vertexBuffer || !a_lookup.indexBuffer ||
				a_lookup.indexFormat != REX::W32::DXGI_FORMAT_R16_UINT) {
				return {
					knownGeometry ? CarrierResolveStatus::CarrierIdentityMismatch :
						CarrierResolveStatus::NotCarrierGeometry,
					{}
				};
			}
			const auto iterator = entries_.find({
				a_lookup.geometry, a_lookup.vertexBuffer, a_lookup.indexBuffer
			});
			if (iterator == entries_.end()) {
				return {
					knownGeometry ? CarrierResolveStatus::CarrierIdentityMismatch :
						CarrierResolveStatus::NotCarrierGeometry,
					{}
				};
			}

			const auto& registration = iterator->second;
			const auto& sidecar = registration.sidecar;
			if (!sidecar || sidecar->vertexStride != a_lookup.vertexStride ||
				sidecar->sourceVertexOffset != a_lookup.vertexOffset ||
				sidecar->sourceIndexOffset != a_lookup.indexOffset) {
				return { CarrierResolveStatus::CarrierIdentityMismatch, {} };
			}

			return {
				CarrierResolveStatus::ExactMatch,
				ResolvedCarrier{
					sidecar,
					registration.viewShellCount,
					registration.shadowShellCount,
					registration.stateRevision,
					registration.enabled
				}
			};
		} catch (...) {
			return {};
		}
	}

	SidecarBuildResult BuildNativeSidecar(const NativeSidecarBuildRequest& a_request)
	{
		if (!a_request.geometry || !a_request.device || !a_request.sourceVertexBuffer ||
			!a_request.sourceIndexBuffer || a_request.vertexCount == 0 ||
			a_request.vertexStride == 0) {
			return std::unexpected(SidecarBuildError::InvalidArgument);
		}
		if (!a_request.profile.Validate().empty()) {
			return std::unexpected(SidecarBuildError::InvalidProfile);
		}
		if (a_request.indexFormat != REX::W32::DXGI_FORMAT_R16_UINT ||
			(a_request.sourceIndexOffset % sizeof(std::uint16_t)) != 0) {
			return std::unexpected(SidecarBuildError::UnsupportedIndexFormat);
		}

		auto vertexDesc = a_request.vertexDesc;
		const auto vertexDescriptorBits = std::bit_cast<std::uint64_t>(vertexDesc);
		auto profileFingerprint = ProfileFingerprint(a_request.profile);
		profileFingerprint = HashCombine(
			profileFingerprint, a_request.forceVisibleAlpha ? 1ULL : 0ULL);
		profileFingerprint = HashCombine(
			profileFingerprint, a_request.correctShellPipeline ? 1ULL : 0ULL);
		const auto flags = static_cast<std::uint16_t>(vertexDesc.GetFlags());
		const auto requiredFlags = static_cast<std::uint16_t>(
			RE::BSGraphics::Vertex::VF_VERTEX |
			RE::BSGraphics::Vertex::VF_SKINNED);
		const auto forbiddenFlags = static_cast<std::uint16_t>(
			RE::BSGraphics::Vertex::VF_LANDDATA |
			RE::BSGraphics::Vertex::VF_EYEDATA |
			RE::BSGraphics::Vertex::VF_INSTANCEDATA);
		// Skyrim SE/AE stream 100 stores the position as float32 even when
		// VF_FULLPREC is absent. Some valid skinned bodies also omit VF_NORMAL;
		// the generator reconstructs an extrusion normal from indexed topology.
		if ((flags & requiredFlags) != requiredFlags || (flags & forbiddenFlags) != 0) {
			return std::unexpected(SidecarBuildError::UnsupportedVertexFlags);
		}

		const auto positionOffset = vertexDesc.GetAttributeOffset(
			RE::BSGraphics::Vertex::VA_POSITION);
		const auto normalOffset = vertexDesc.GetAttributeOffset(
			RE::BSGraphics::Vertex::VA_NORMAL);
		const auto hasPackedNormal =
			(flags & RE::BSGraphics::Vertex::VF_NORMAL) != 0;
		const auto skinOffset = vertexDesc.GetAttributeOffset(
			RE::BSGraphics::Vertex::VA_SKINNING);
		if (vertexDesc.GetSize() != a_request.vertexStride || positionOffset != 0 ||
			positionOffset + sizeof(float) * 3 > a_request.vertexStride ||
			(hasPackedNormal && normalOffset + 4 > a_request.vertexStride) ||
			skinOffset + 12 > a_request.vertexStride) {
			return std::unexpected(SidecarBuildError::InvalidVertexLayout);
		}

		const bool hasAnyRefinedData = a_request.shellVertexCount != 0 ||
			!a_request.shellVertexData.empty() || !a_request.shellIndexData.empty() ||
			!a_request.shellCarrierVertices.empty();
		const bool hasRefinedData = a_request.shellVertexCount != 0 &&
			!a_request.shellVertexData.empty() && !a_request.shellIndexData.empty() &&
			!a_request.shellCarrierVertices.empty();
		if (hasAnyRefinedData != hasRefinedData ||
			(hasRefinedData &&
			 (!a_request.coverage.empty() || a_request.sharedVertexResources))) {
			return std::unexpected(SidecarBuildError::InvalidArgument);
		}

		const auto sourceVertexBytes =
			static_cast<std::size_t>(a_request.vertexCount) * a_request.vertexStride;
		if (sourceVertexBytes / a_request.vertexStride != a_request.vertexCount ||
			(!a_request.sharedVertexResources &&
			 a_request.rawVertexData.size() != sourceVertexBytes) ||
			a_request.rawIndexData.empty() ||
			(a_request.rawIndexData.size() % 3U) != 0 ||
			a_request.rawIndexData.size() >
				static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
			return std::unexpected(SidecarBuildError::MissingRawData);
		}
		if (std::ranges::any_of(a_request.rawIndexData,
				[sourceVertexCount = a_request.vertexCount](std::uint16_t a_index) {
					return a_index >= sourceVertexCount;
				})) {
			return std::unexpected(SidecarBuildError::InvalidIndex);
		}

		const auto shellVertexCount = hasRefinedData ?
			a_request.shellVertexCount : a_request.vertexCount;
		const auto shellVertexBytes =
			static_cast<std::size_t>(shellVertexCount) * a_request.vertexStride;
		const auto shellIndexCount = hasRefinedData ?
			a_request.shellIndexData.size() : a_request.rawIndexData.size();
		if (hasRefinedData && shellVertexCount >
			static_cast<std::uint32_t>((std::numeric_limits<std::uint16_t>::max)())) {
			return std::unexpected(SidecarBuildError::ResourceTooLarge);
		}
		if (shellVertexBytes / a_request.vertexStride != shellVertexCount ||
			shellIndexCount == 0 || (shellIndexCount % 3U) != 0 ||
			shellIndexCount >
				static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) ||
			(hasRefinedData &&
			 (a_request.shellVertexData.size() != shellVertexBytes ||
			  a_request.shellCarrierVertices.size() != shellVertexCount))) {
			return std::unexpected(SidecarBuildError::InvalidCoverage);
		}
		if (hasRefinedData && std::ranges::any_of(a_request.shellIndexData,
				[shellVertexCount](std::uint16_t a_index) {
					return a_index >= shellVertexCount;
				})) {
			return std::unexpected(SidecarBuildError::InvalidIndex);
		}

		REX::W32::D3D11_BUFFER_DESC sourceVertexDesc{};
		REX::W32::D3D11_BUFFER_DESC sourceIndexDesc{};
		a_request.sourceVertexBuffer->GetDesc(&sourceVertexDesc);
		a_request.sourceIndexBuffer->GetDesc(&sourceIndexDesc);
		if (!BelongsToDevice(a_request.sourceVertexBuffer, a_request.device) ||
			!BelongsToDevice(a_request.sourceIndexBuffer, a_request.device)) {
			return std::unexpected(SidecarBuildError::DeviceMismatch);
		}
		if ((sourceVertexDesc.bindFlags & REX::W32::D3D11_BIND_VERTEX_BUFFER) == 0 ||
			(sourceIndexDesc.bindFlags & REX::W32::D3D11_BIND_INDEX_BUFFER) == 0) {
			return std::unexpected(SidecarBuildError::InvalidBufferBinding);
		}
		if (!AdditionFits(a_request.sourceVertexOffset, sourceVertexBytes,
				sourceVertexDesc.byteWidth) ||
			!AdditionFits(a_request.sourceIndexOffset,
				a_request.rawIndexData.size_bytes(), sourceIndexDesc.byteWidth)) {
			return std::unexpected(SidecarBuildError::InvalidBufferRange);
		}

		const auto expandedVertexCount =
			static_cast<std::uint64_t>(shellVertexCount) * a_request.profile.shellCount;
		const auto expandedBytes = expandedVertexCount * a_request.vertexStride;
		if (expandedVertexCount > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
			expandedBytes > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
			return std::unexpected(SidecarBuildError::ResourceTooLarge);
		}

		auto sidecar = std::make_shared<NativeCarrierSidecar>();
		sidecar->geometry = a_request.geometry;
		sidecar->sourceVertexBuffer = a_request.sourceVertexBuffer;
		sidecar->sourceIndexBuffer = a_request.sourceIndexBuffer;
		sidecar->vertexStride = a_request.vertexStride;
		sidecar->sourceVertexOffset = a_request.sourceVertexOffset;
		sidecar->sourceIndexOffset = a_request.sourceIndexOffset;
		sidecar->sourceVertexCount = a_request.vertexCount;
		sidecar->sourceIndexCount = static_cast<std::uint32_t>(
			a_request.rawIndexData.size());
		sidecar->shellVertexCount = shellVertexCount;
		sidecar->shellIndexCount = static_cast<std::uint32_t>(shellIndexCount);
		sidecar->profileShellCount = a_request.profile.shellCount;
		sidecar->vertexDescriptorBits = vertexDescriptorBits;
		sidecar->profileFingerprint = profileFingerprint;
		sidecar->geometryRevision = a_request.geometryRevision;
		sidecar->refinedShellTopology = hasRefinedData;
		sidecar->correctShellPipeline = a_request.correctShellPipeline;

		std::vector<std::uint16_t> replacementIndices;
		if (hasRefinedData) {
			replacementIndices.assign(
				a_request.shellIndexData.begin(), a_request.shellIndexData.end());
		} else if (!a_request.coverage.empty()) {
			auto masked = Generator::GenerateBinaryShellIndexMask({
				.sourceIndices = a_request.rawIndexData,
				.coverage = a_request.coverage,
				.vertexCount = a_request.vertexCount,
				.shellCount = a_request.profile.shellCount
			});
			if (!masked) {
				return std::unexpected(
					masked.error() == Generator::ShellIndexMaskError::InvalidIndex ?
						SidecarBuildError::InvalidIndex :
						SidecarBuildError::InvalidCoverage);
			}
			if (masked->HasCoverage()) {
				replacementIndices = std::move(masked->indices);
				sidecar->hiddenTriangleCount = masked->hiddenTriangleCount;
			}
		}
		if (!replacementIndices.empty()) {
			const auto replacementBytes =
				replacementIndices.size() * sizeof(std::uint16_t);
			if (replacementBytes >
				static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
				return std::unexpected(SidecarBuildError::ResourceTooLarge);
			}
			REX::W32::D3D11_BUFFER_DESC indexDesc{};
			indexDesc.byteWidth = static_cast<std::uint32_t>(replacementBytes);
			indexDesc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
			indexDesc.bindFlags = REX::W32::D3D11_BIND_INDEX_BUFFER;
			REX::W32::D3D11_SUBRESOURCE_DATA initialData{};
			initialData.sysMem = replacementIndices.data();
			if (a_request.device->CreateBuffer(
					&indexDesc, &initialData,
					sidecar->shellIndexBuffer.ReleaseAndGetAddressOf()) < 0) {
				return std::unexpected(SidecarBuildError::BufferCreationFailed);
			}
		}

		if (const auto& shared = a_request.sharedVertexResources) {
			if (!shared->IsValid() || shared->geometry != a_request.geometry ||
				shared->sourceVertexBuffer.Get() != a_request.sourceVertexBuffer ||
				shared->sourceVertexOffset != a_request.sourceVertexOffset ||
				shared->vertexStride != a_request.vertexStride ||
				shared->sourceVertexCount != a_request.vertexCount ||
				shared->shellVertexCount != shellVertexCount ||
				shared->refinedShellTopology ||
				shared->profileShellCount != a_request.profile.shellCount ||
				shared->vertexDescriptorBits != vertexDescriptorBits ||
				shared->profileFingerprint != profileFingerprint ||
				!BelongsToDevice(shared->expandedVertexBuffer.Get(), a_request.device) ||
				shared->geometryRevision != a_request.geometryRevision) {
				return std::unexpected(SidecarBuildError::InvalidArgument);
			}
			REX::W32::D3D11_BUFFER_DESC expandedDescription{};
			shared->expandedVertexBuffer->GetDesc(&expandedDescription);
			if ((expandedDescription.bindFlags & REX::W32::D3D11_BIND_VERTEX_BUFFER) == 0 ||
				expandedDescription.byteWidth < expandedBytes) {
				return std::unexpected(SidecarBuildError::InvalidArgument);
			}
			for (std::uint32_t layer = 0; layer < a_request.profile.shellCount; ++layer) {
				const auto& alpha = shared->alphaReferenceBuffers[layer];
				if (!BelongsToDevice(alpha.Get(), a_request.device)) {
					return std::unexpected(SidecarBuildError::InvalidArgument);
				}
				REX::W32::D3D11_BUFFER_DESC alphaDescription{};
				alpha->GetDesc(&alphaDescription);
				if (alphaDescription.byteWidth != sizeof(float) * 4U ||
					(alphaDescription.bindFlags & REX::W32::D3D11_BIND_CONSTANT_BUFFER) == 0) {
					return std::unexpected(SidecarBuildError::InvalidArgument);
				}
			}
			sidecar->expandedVertexBuffer = shared->expandedVertexBuffer;
			sidecar->alphaReferenceBuffers = shared->alphaReferenceBuffers;
			sidecar->shellDepthState = shared->shellDepthState;
			sidecar->expandedVertexOffset = shared->expandedVertexOffset;
		} else {
			const auto generatedVertexData = hasRefinedData ?
				a_request.shellVertexData : a_request.rawVertexData;
			const auto generatedIndexData = hasRefinedData ?
				a_request.shellIndexData : a_request.rawIndexData;
			auto generated = Generator::GenerateShellVertices({
				.sourceVertices = generatedVertexData,
				.profile = std::addressof(a_request.profile),
				.vertexCount = shellVertexCount,
				.vertexStride = a_request.vertexStride,
				.positionOffset = positionOffset,
				.packedNormalOffset = hasPackedNormal ?
					std::optional<std::uint32_t>{ normalOffset } : std::nullopt,
				.sourceIndices = generatedIndexData,
				.predecodedVertices = hasRefinedData ?
					a_request.shellCarrierVertices :
					std::span<const Core::CarrierVertex>{}
			});
			if (!generated) {
				if (generated.error() == Generator::ShellVertexGenerationError::InvalidProfile) {
					return std::unexpected(SidecarBuildError::InvalidProfile);
				}
				if (generated.error() == Generator::ShellVertexGenerationError::ResourceTooLarge) {
					return std::unexpected(SidecarBuildError::ResourceTooLarge);
				}
				if (generated.error() == Generator::ShellVertexGenerationError::InvalidTopology) {
					return std::unexpected(SidecarBuildError::InvalidIndex);
				}
				return std::unexpected(SidecarBuildError::InvalidVertexLayout);
			}
			auto expandedData = std::move(*generated);

			REX::W32::D3D11_BUFFER_DESC expandedDesc{};
			expandedDesc.byteWidth = static_cast<std::uint32_t>(expandedData.size());
			expandedDesc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
			expandedDesc.bindFlags = REX::W32::D3D11_BIND_VERTEX_BUFFER;
			REX::W32::D3D11_SUBRESOURCE_DATA expandedInitialData{};
			expandedInitialData.sysMem = expandedData.data();
			if (a_request.device->CreateBuffer(
					&expandedDesc, &expandedInitialData,
					sidecar->expandedVertexBuffer.ReleaseAndGetAddressOf()) < 0) {
				return std::unexpected(SidecarBuildError::BufferCreationFailed);
			}

			for (std::uint32_t layer = 0; layer < a_request.profile.shellCount; ++layer) {
				alignas(16) const std::array<float, 4> alphaReference{
					a_request.forceVisibleAlpha ? -1.0F :
						a_request.profile.alphaThresholds[layer],
					0.0F, 0.0F, 0.0F
				};
				REX::W32::D3D11_BUFFER_DESC constantDesc{};
				constantDesc.byteWidth = static_cast<std::uint32_t>(sizeof(alphaReference));
				constantDesc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
				constantDesc.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
				REX::W32::D3D11_SUBRESOURCE_DATA constantInitialData{};
				constantInitialData.sysMem = alphaReference.data();
				if (a_request.device->CreateBuffer(
						&constantDesc, &constantInitialData,
						sidecar->alphaReferenceBuffers[layer].ReleaseAndGetAddressOf()) < 0) {
					return std::unexpected(SidecarBuildError::BufferCreationFailed);
				}
			}
		}

		if (a_request.correctShellPipeline && !a_request.sharedVertexResources) {
			REX::W32::D3D11_DEPTH_STENCIL_DESC depth{};
			depth.depthEnable = 1;
			depth.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ALL;
			depth.depthFunc = REX::W32::D3D11_COMPARISON_LESS_EQUAL;
			depth.stencilEnable = 0;
			if (a_request.device->CreateDepthStencilState(
					&depth, sidecar->shellDepthState.ReleaseAndGetAddressOf()) < 0) {
				return std::unexpected(SidecarBuildError::BufferCreationFailed);
			}
		}

		return std::shared_ptr<const NativeCarrierSidecar>{ std::move(sidecar) };
	}

	BackendKind NullShellRenderAdapter::Kind() const noexcept { return BackendKind::Null; }

	PrepareResult NullShellRenderAdapter::Prepare(
		REX::W32::ID3D11DeviceContext*, const FurPassContext&,
		const IndexedDrawArgs&, const ResolvedCarrier&) const noexcept
	{
		return { PrepareStatus::UnsupportedBackend, {} };
	}

	BackendKind NativeMultiDrawAdapter::Kind() const noexcept
	{
		return BackendKind::NativeMultiDraw;
	}

	PrepareResult NativeMultiDrawAdapter::Prepare(
		REX::W32::ID3D11DeviceContext* a_context,
		const FurPassContext& a_pass,
		const IndexedDrawArgs& a_draw,
		const ResolvedCarrier& a_carrier) const noexcept
	{
		if (!a_context || !a_carrier.sidecar || !a_carrier.sidecar->IsValid()) {
			return { PrepareStatus::NoCarrier, {} };
		}
		if (!a_carrier.enabled) {
			return { PrepareStatus::DisabledCarrier, {} };
		}
		if (!IsSupportedPass(a_pass.kind) || !a_pass.alphaTest) {
			return { PrepareStatus::UnsupportedPass, {} };
		}

		REX::W32::ComPtr<REX::W32::ID3D11PixelShader> pixelShader;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> alphaReference;
		a_context->PSGetShader(pixelShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
		a_context->PSGetConstantBuffers(
			kAlphaReferenceConstantSlot, 1, alphaReference.ReleaseAndGetAddressOf());
		if (!pixelShader.Get() || !alphaReference.Get()) {
			return { PrepareStatus::UnsupportedPipeline, {} };
		}
		REX::W32::D3D11_BUFFER_DESC alphaReferenceDescription{};
		alphaReference->GetDesc(&alphaReferenceDescription);
		if (alphaReferenceDescription.byteWidth != sizeof(float) * 4U ||
			(alphaReferenceDescription.bindFlags &
			 REX::W32::D3D11_BIND_CONSTANT_BUFFER) == 0) {
			return { PrepareStatus::UnsupportedPipeline, {} };
		}

		const auto& sidecar = *a_carrier.sidecar;
		// The source topology was fully validated while building the immutable
		// sidecar. Runtime matching is deliberately O(1): this sidecar represents
		// exactly the source partition captured at startIndex/baseVertex zero.
		if (a_draw.indexCount != sidecar.sourceIndexCount ||
			a_draw.startIndex != 0 || a_draw.baseVertex != 0) {
			return { PrepareStatus::InvalidDraw, {} };
		}

		const auto requestedCount =
			(a_pass.kind == FurPassKind::ShadowMap ||
			 a_pass.kind == FurPassKind::ShadowMask) ?
				a_carrier.shadowShellCount : a_carrier.viewShellCount;
		if (requestedCount == 0 || requestedCount > sidecar.profileShellCount) {
			return { PrepareStatus::InvalidDraw, {} };
		}

		PreparedShellDraw prepared;
		prepared.expandedVertexBuffer = sidecar.expandedVertexBuffer;
		prepared.shellIndexBuffer = sidecar.shellIndexBuffer;
		prepared.shellDepthState = sidecar.shellDepthState;
		prepared.vertexStride = sidecar.vertexStride;
		prepared.vertexOffset = sidecar.expandedVertexOffset;
		prepared.indexCount = sidecar.shellIndexBuffer.Get() ?
			sidecar.shellIndexCount : a_draw.indexCount;
		prepared.startIndex = sidecar.shellIndexBuffer.Get() ? 0U : a_draw.startIndex;
		prepared.layerCount = requestedCount;

		for (std::uint32_t localLayer = 0; localLayer < requestedCount; ++localLayer) {
			const auto canonicalLayer = Core::CanonicalLayerIndex(
				localLayer, requestedCount, sidecar.profileShellCount);
			const auto expandedBaseVertex = sidecar.refinedShellTopology ?
				static_cast<std::int64_t>(canonicalLayer) * sidecar.shellVertexCount :
				static_cast<std::int64_t>(a_draw.baseVertex) +
					static_cast<std::int64_t>(canonicalLayer) * sidecar.sourceVertexCount;
			if (expandedBaseVertex < (std::numeric_limits<std::int32_t>::min)() ||
				expandedBaseVertex > (std::numeric_limits<std::int32_t>::max)()) {
				return { PrepareStatus::InvalidDraw, {} };
			}

			prepared.layers[localLayer] = {
				static_cast<std::int32_t>(expandedBaseVertex),
				canonicalLayer,
				sidecar.alphaReferenceBuffers[canonicalLayer]
			};
		}

		return { PrepareStatus::Ready, std::move(prepared) };
	}

	RenderRouter::RenderRouter(ICarrierDrawSource& a_carriers) :
		carriers_(std::addressof(a_carriers)),
		adapter_(std::make_shared<NullShellRenderAdapter>())
	{}

	void RenderRouter::SetAdapter(std::shared_ptr<IShellRenderAdapter> a_adapter)
	{
		if (!a_adapter) {
			a_adapter = std::make_shared<NullShellRenderAdapter>();
		}
		adapter_.store(std::move(a_adapter), std::memory_order_release);
	}

	std::shared_ptr<IShellRenderAdapter> RenderRouter::GetAdapter() const noexcept
	{
		return adapter_.load(std::memory_order_acquire);
	}

	PrepareResult RenderRouter::Prepare(
		REX::W32::ID3D11DeviceContext* a_context,
		const FurPassContext& a_pass,
		const IndexedDrawArgs& a_draw) const noexcept
	{
		if (!a_context || !carriers_ || !a_pass.geometry) {
			return { PrepareStatus::NoPassContext, {} };
		}

		REX::W32::ComPtr<REX::W32::ID3D11Buffer> vertexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> indexBuffer;
		std::uint32_t stride{};
		std::uint32_t vertexOffset{};
		std::uint32_t indexOffset{};
		REX::W32::DXGI_FORMAT indexFormat{ REX::W32::DXGI_FORMAT_UNKNOWN };
		a_context->IAGetVertexBuffers(
			kCarrierVertexBufferSlot, 1, vertexBuffer.ReleaseAndGetAddressOf(),
			&stride, &vertexOffset);
		a_context->IAGetIndexBuffer(
			indexBuffer.ReleaseAndGetAddressOf(), &indexFormat, &indexOffset);

		const auto resolution = carriers_->Resolve({
			a_pass.geometry, vertexBuffer.Get(), indexBuffer.Get(), indexFormat,
			stride, vertexOffset, indexOffset
		});
		if (resolution.status == CarrierResolveStatus::NotCarrierGeometry) {
			return { PrepareStatus::NoCarrier, {} };
		}
		if (resolution.status != CarrierResolveStatus::ExactMatch ||
			!resolution.carrier) {
			return { PrepareStatus::InvalidDraw, {}, true };
		}
		const auto& carrier = *resolution.carrier;
		if (!carrier.enabled) {
			return { PrepareStatus::DisabledCarrier, {}, true };
		}

		const auto adapter = GetAdapter();
		if (!adapter) {
			return { PrepareStatus::UnsupportedBackend, {}, true };
		}
		auto result = adapter->Prepare(a_context, a_pass, a_draw, carrier);
		result.carrierMatched = true;
		return result;
	}

	PrepareResult RenderRouter::PrepareBoundCarrier(
		REX::W32::ID3D11DeviceContext* a_context,
		const FurPassContext& a_pass) const noexcept
	{
		if (!a_context || !carriers_ || !a_pass.geometry) {
			return { PrepareStatus::NoPassContext, {} };
		}

		REX::W32::ComPtr<REX::W32::ID3D11Buffer> vertexBuffer;
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> indexBuffer;
		std::uint32_t stride{};
		std::uint32_t vertexOffset{};
		std::uint32_t indexOffset{};
		REX::W32::DXGI_FORMAT indexFormat{ REX::W32::DXGI_FORMAT_UNKNOWN };
		a_context->IAGetVertexBuffers(
			kCarrierVertexBufferSlot, 1, vertexBuffer.ReleaseAndGetAddressOf(),
			&stride, &vertexOffset);
		a_context->IAGetIndexBuffer(
			indexBuffer.ReleaseAndGetAddressOf(), &indexFormat, &indexOffset);

		const auto resolution = carriers_->Resolve({
			a_pass.geometry, vertexBuffer.Get(), indexBuffer.Get(), indexFormat,
			stride, vertexOffset, indexOffset
		});
		if (resolution.status == CarrierResolveStatus::NotCarrierGeometry) {
			return { PrepareStatus::NoCarrier, {} };
		}
		if (resolution.status != CarrierResolveStatus::ExactMatch ||
			!resolution.carrier || !resolution.carrier->sidecar) {
			return { PrepareStatus::InvalidDraw, {}, true };
		}
		const auto& carrier = *resolution.carrier;
		if (!carrier.enabled) {
			return { PrepareStatus::DisabledCarrier, {}, true };
		}

		const auto adapter = GetAdapter();
		if (!adapter) {
			return { PrepareStatus::UnsupportedBackend, {}, true };
		}
		const IndexedDrawArgs sourceDraw{
			carrier.sidecar->sourceIndexCount,
			0,
			0
		};
		auto result = adapter->Prepare(a_context, a_pass, sourceDraw, carrier);
		result.carrierMatched = true;
		return result;
	}

	FurPassContext DescribePass(
		RE::BSBatchRenderer* a_batch,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_technique,
		bool a_alphaTest,
		std::uint32_t a_renderFlags) noexcept
	{
		FurPassContext result{
			a_batch,
			a_pass,
			a_pass ? a_pass->geometry : nullptr,
			a_technique,
			a_renderFlags,
			a_alphaTest
		};
		if (!a_pass || !a_pass->shader || !a_pass->geometry) {
			return result;
		}

		if (a_pass->shader->shaderType == RE::BSShader::Types::Lighting) {
			result.shaderFamily = ShaderFamily::Lighting;
			result.kind = ClassifyLightingPass(a_technique, a_alphaTest);
			return result;
		}
		if (a_pass->shader->shaderType != RE::BSShader::Types::Utility) {
			return result;
		}

		result.shaderFamily = ShaderFamily::Utility;
		result.kind = ClassifyUtilityPass(a_technique, a_alphaTest);
		return result;
	}

	FurPassKind ClassifyLightingPass(
		std::uint32_t a_technique,
		bool a_alphaTest) noexcept
	{
		// A 16-byte b11 can remain bound even for a non-alpha permutation, so it
		// is not proof that changing AlphaTestRef will clip pixels. Requiring both
		// signals prevents an inherited body pass such as 0x4D406633 from drawing
		// several opaque expanded bodies.
		return a_alphaTest &&
			HasLightingFlag(
				a_technique,
				RE::BSLightingShader::TechniqueFlag::kSkinned) &&
			HasLightingFlag(
				a_technique,
				RE::BSLightingShader::TechniqueFlag::kDoAlphaTest) ?
			FurPassKind::Lighting : FurPassKind::Unsupported;
	}

	FurPassKind ClassifyUtilityPass(
		std::uint32_t a_technique,
		bool a_alphaTest) noexcept
	{
		using Flags = RE::BSUtilityShader::Flags;
		if (!a_alphaTest || !HasUtilityFlag(a_technique, Flags::AlphaTest)) {
			return FurPassKind::Unsupported;
		}
		if (HasUtilityFlag(a_technique, Flags::RenderShadowmap)) {
			return FurPassKind::ShadowMap;
		}
		if (HasUtilityFlag(a_technique, Flags::RenderShadowmask) ||
			HasUtilityFlag(a_technique, Flags::RenderShadowmaskSpot) ||
			HasUtilityFlag(a_technique, Flags::RenderShadowmaskPb) ||
			HasUtilityFlag(a_technique, Flags::RenderShadowmaskDpb)) {
			return FurPassKind::ShadowMask;
		}
		if (HasUtilityFlag(a_technique, Flags::RenderDepth)) {
			return FurPassKind::Depth;
		}
		if (HasUtilityFlag(a_technique, Flags::RenderNormal) ||
			HasUtilityFlag(a_technique, Flags::RenderNormalFalloff) ||
			HasUtilityFlag(a_technique, Flags::RenderNormalClamp) ||
			HasUtilityFlag(a_technique, Flags::RenderNormalClear)) {
			return FurPassKind::Normal;
		}
		return FurPassKind::Unsupported;
	}

	ScopedPassContext::ScopedPassContext(FurPassContext a_context) noexcept :
		context_(a_context), previous_(currentPassContext)
	{
		currentPassContext = std::addressof(context_);
	}

	ScopedPassContext::ScopedPassContext(
		RE::BSBatchRenderer* a_batch,
		RE::BSRenderPass* a_pass,
		std::uint32_t a_technique,
		bool a_alphaTest,
		std::uint32_t a_renderFlags) noexcept :
		ScopedPassContext(DescribePass(
			a_batch, a_pass, a_technique, a_alphaTest, a_renderFlags))
	{}

	ScopedPassContext::~ScopedPassContext() noexcept
	{
		currentPassContext = previous_;
	}

	const FurPassContext* ScopedPassContext::Current() noexcept
	{
		return currentPassContext;
	}

	ScopedNativeDrawState::ScopedNativeDrawState(
		REX::W32::ID3D11DeviceContext* a_context,
		const PreparedShellDraw& a_draw) noexcept :
		context_(a_context), vertexBufferSlot_(a_draw.vertexBufferSlot)
	{
		if (!context_ || !a_draw.IsValid()) {
			return;
		}

		context_->IAGetVertexBuffers(
			vertexBufferSlot_, 1, originalVertexBuffer_.ReleaseAndGetAddressOf(),
			&originalStride_, &originalOffset_);
		if (a_draw.shellIndexBuffer.Get()) {
			context_->IAGetIndexBuffer(
				originalIndexBuffer_.ReleaseAndGetAddressOf(),
				&originalIndexFormat_, &originalIndexOffset_);
			indexOverridden_ = true;
		}
		context_->PSGetConstantBuffers(
			kAlphaReferenceConstantSlot, 1,
			originalAlphaReference_.ReleaseAndGetAddressOf());
		context_->OMGetDepthStencilState(
			originalDepthState_.ReleaseAndGetAddressOf(), &originalStencilReference_);
		if (!originalVertexBuffer_.Get() || !originalAlphaReference_.Get() ||
			(a_draw.shellIndexBuffer.Get() && !originalIndexBuffer_.Get())) {
			return;
		}

		auto* expanded = a_draw.expandedVertexBuffer.Get();
		context_->IASetVertexBuffers(
			vertexBufferSlot_, 1, &expanded, &a_draw.vertexStride, &a_draw.vertexOffset);
		if (a_draw.shellIndexBuffer.Get()) {
			context_->IASetIndexBuffer(
				a_draw.shellIndexBuffer.Get(), a_draw.shellIndexFormat,
				a_draw.shellIndexOffset);
		}
		if (a_draw.shellDepthState.Get()) {
			context_->OMSetDepthStencilState(
				a_draw.shellDepthState.Get(), originalStencilReference_);
		}
		applied_ = true;
	}

	ScopedNativeDrawState::~ScopedNativeDrawState() noexcept { Restore(); }
	bool ScopedNativeDrawState::IsApplied() const noexcept { return applied_; }

	bool ScopedNativeDrawState::BindLayer(const NativeLayerDraw& a_layer) noexcept
	{
		if (!applied_ || !a_layer.alphaReferenceBuffer.Get()) {
			return false;
		}

		auto* buffer = a_layer.alphaReferenceBuffer.Get();
		context_->PSSetConstantBuffers(kAlphaReferenceConstantSlot, 1, &buffer);
		return true;
	}

	void ScopedNativeDrawState::Restore() noexcept
	{
		if (!applied_ || !context_) {
			return;
		}

		auto* alphaReference = originalAlphaReference_.Get();
		context_->PSSetConstantBuffers(kAlphaReferenceConstantSlot, 1, &alphaReference);

		auto* vertexBuffer = originalVertexBuffer_.Get();
		context_->IASetVertexBuffers(
			vertexBufferSlot_, 1, &vertexBuffer, &originalStride_, &originalOffset_);
		if (indexOverridden_) {
			context_->IASetIndexBuffer(
				originalIndexBuffer_.Get(), originalIndexFormat_, originalIndexOffset_);
			indexOverridden_ = false;
		}
		context_->OMSetDepthStencilState(
			originalDepthState_.Get(), originalStencilReference_);
		applied_ = false;
	}
}
