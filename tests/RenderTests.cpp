#include "DynamicFur/Generator/ShellVertexGenerator.h"
#include "DynamicFur/Render/RenderBackend.h"
#include "DynamicFur/Render/RenderHooks.h"
#include "REX/W32/D3DCOMPILER.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace
{
	using namespace DynamicFur;

	struct TestContext
	{
		void Check(
			bool a_condition,
			std::string_view a_description,
			const std::source_location& a_location = std::source_location::current())
		{
			if (!a_condition) {
				++failures;
				std::cerr << a_location.file_name() << ':' << a_location.line()
					<< ": check failed: " << a_description << '\n';
			}
		}

		int failures{};
	};

	struct DeviceContext
	{
		REX::W32::ComPtr<REX::W32::ID3D11Device> device;
		REX::W32::ComPtr<REX::W32::ID3D11DeviceContext> context;
	};

	[[nodiscard]] DeviceContext CreateWarpDevice(TestContext& a_tests)
	{
		DeviceContext result;
		constexpr std::array featureLevels{
			REX::W32::D3D_FEATURE_LEVEL_11_0
		};
		REX::W32::D3D_FEATURE_LEVEL selected{};
		const auto operation = REX::W32::D3D11CreateDevice(
			nullptr,
			REX::W32::D3D_DRIVER_TYPE_WARP,
			nullptr,
			0,
			featureLevels.data(),
			static_cast<std::uint32_t>(featureLevels.size()),
			7,
			result.device.ReleaseAndGetAddressOf(),
			&selected,
			result.context.ReleaseAndGetAddressOf());
		a_tests.Check(operation >= 0, "D3D11 WARP device creation");
		a_tests.Check(
			selected == REX::W32::D3D_FEATURE_LEVEL_11_0,
			"D3D11 feature level 11_0");
		return result;
	}

	[[nodiscard]] REX::W32::ComPtr<REX::W32::ID3D11Buffer> CreateBuffer(
		TestContext& a_tests,
		REX::W32::ID3D11Device* a_device,
		std::span<const std::byte> a_bytes,
		std::uint32_t a_bindFlags)
	{
		REX::W32::D3D11_BUFFER_DESC description{};
		description.byteWidth = static_cast<std::uint32_t>(a_bytes.size());
		description.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
		description.bindFlags = a_bindFlags;
		REX::W32::D3D11_SUBRESOURCE_DATA initialData{};
		initialData.sysMem = a_bytes.data();

		REX::W32::ComPtr<REX::W32::ID3D11Buffer> result;
		const auto operation = a_device->CreateBuffer(
			&description, &initialData, result.ReleaseAndGetAddressOf());
		a_tests.Check(operation >= 0 && result.Get(), "immutable buffer creation");
		return result;
	}

	[[nodiscard]] REX::W32::ComPtr<REX::W32::ID3D11PixelShader> CreatePixelShader(
		TestContext& a_tests,
		REX::W32::ID3D11Device* a_device)
	{
		constexpr std::string_view source =
			"float4 main() : SV_Target { return float4(1, 1, 1, 1); }";
		REX::W32::ComPtr<REX::W32::ID3DBlob> bytecode;
		REX::W32::ComPtr<REX::W32::ID3DBlob> errors;
		const auto compiled = REX::W32::D3DCompile(
			source.data(), source.size(), "DynamicFurRenderTests", nullptr, nullptr,
			"main", "ps_5_0", 0, 0, bytecode.ReleaseAndGetAddressOf(),
			errors.ReleaseAndGetAddressOf());
		a_tests.Check(compiled >= 0 && bytecode.Get(), "test pixel shader compilation");
		if (compiled < 0 || !bytecode.Get()) {
			return {};
		}

		REX::W32::ComPtr<REX::W32::ID3D11PixelShader> result;
		const auto created = a_device->CreatePixelShader(
			bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
			result.ReleaseAndGetAddressOf());
		a_tests.Check(created >= 0 && result.Get(), "test pixel shader creation");
		return result;
	}

	[[nodiscard]] std::vector<std::byte> ReadBuffer(
		TestContext& a_tests,
		const DeviceContext& a_d3d,
		REX::W32::ID3D11Buffer* a_source)
	{
		REX::W32::D3D11_BUFFER_DESC sourceDescription{};
		a_source->GetDesc(&sourceDescription);

		REX::W32::D3D11_BUFFER_DESC stagingDescription{};
		stagingDescription.byteWidth = sourceDescription.byteWidth;
		stagingDescription.usage = REX::W32::D3D11_USAGE_STAGING;
		stagingDescription.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_READ;

		REX::W32::ComPtr<REX::W32::ID3D11Buffer> staging;
		const auto created = a_d3d.device->CreateBuffer(
			&stagingDescription, nullptr, staging.ReleaseAndGetAddressOf());
		a_tests.Check(created >= 0 && staging.Get(), "staging buffer creation");
		if (created < 0 || !staging.Get()) {
			return {};
		}

		std::vector<std::byte> result(sourceDescription.byteWidth);
		a_d3d.context->CopyResource(staging.Get(), a_source);
		REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
		const auto mappedResult = a_d3d.context->Map(
			staging.Get(), 0, REX::W32::D3D11_MAP_READ, 0, &mapped);
		a_tests.Check(mappedResult >= 0 && mapped.data, "staging buffer map");
		if (mappedResult < 0 || !mapped.data) {
			return {};
		}

		std::memcpy(result.data(), mapped.data, result.size());
		a_d3d.context->Unmap(staging.Get(), 0);
		return result;
	}

	[[nodiscard]] float ReadFloat(
		std::span<const std::byte> a_bytes,
		std::size_t a_offset)
	{
		float result{};
		if (a_offset + sizeof(result) <= a_bytes.size()) {
			std::memcpy(&result, a_bytes.data() + a_offset, sizeof(result));
		}
		return result;
	}

	void TestSidecarAndRegistry(TestContext& a_tests, const DeviceContext& a_d3d)
	{
		auto vertexDesc = std::bit_cast<RE::BSGraphics::VertexDesc>(
			0x0004300050000408ULL);
		const auto stride = vertexDesc.GetSize();
		a_tests.Check(stride == 32, "3BA-style vertex stride without stored normals");
		a_tests.Check(
			std::bit_cast<std::uint64_t>(vertexDesc) == 0x0004300050000408ULL,
			"synthetic descriptor matches the observed 3BA layout");

		constexpr std::uint32_t vertexCount = 3;
		std::vector<std::byte> vertices(static_cast<std::size_t>(stride) * vertexCount);
		const std::array<std::array<float, 4>, vertexCount> positions{
			std::array{ 0.0F, 0.0F, 0.0F, 1.0F },
			std::array{ 1.0F, 0.0F, 0.0F, 1.0F },
			std::array{ 0.0F, 1.0F, 0.0F, 1.0F }
		};
		for (std::uint32_t index = 0; index < vertexCount; ++index) {
			auto* destination = vertices.data() + static_cast<std::size_t>(index) * stride;
			std::memcpy(destination, positions[index].data(), sizeof(positions[index]));
		}

		constexpr std::array<std::uint16_t, 3> indices{ 0, 1, 2 };
		const auto vertexBuffer = CreateBuffer(
			a_tests, a_d3d.device.Get(), vertices,
			REX::W32::D3D11_BIND_VERTEX_BUFFER);
		const auto indexBytes = std::as_bytes(std::span{ indices });
		const auto indexBuffer = CreateBuffer(
			a_tests, a_d3d.device.Get(), indexBytes,
			REX::W32::D3D11_BIND_INDEX_BUFFER);
		if (!vertexBuffer.Get() || !indexBuffer.Get()) {
			return;
		}

		auto profile = Core::ShellProfile::Legacy17();
		const auto generatedVertices = Generator::GenerateShellVertices({
			.sourceVertices = vertices,
			.profile = std::addressof(profile),
			.vertexCount = vertexCount,
			.vertexStride = stride,
			.positionOffset = 0,
			.packedNormalOffset = std::nullopt,
			.sourceIndices = indices
		});
		a_tests.Check(
			generatedVertices && generatedVertices->size() ==
				static_cast<std::size_t>(stride) * vertexCount * profile.shellCount,
			"standalone shell generator produces the complete packed stream");
		auto* geometry = reinterpret_cast<RE::BSGeometry*>(static_cast<std::uintptr_t>(1));
		auto sidecar = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = vertices,
			.rawIndexData = indices,
			.profile = profile,
			.vertexCount = vertexCount,
			.vertexStride = stride,
			.geometryRevision = 9,
			.correctShellPipeline = true
		});
		a_tests.Check(sidecar.has_value(), "native shell sidecar build");
		if (!sidecar) {
			return;
		}
		a_tests.Check(
			(*sidecar)->shellDepthState.Get(),
			"shell depth state is created once in the sidecar");
		const std::array<Core::FirstHiddenShell, vertexCount> armorCoverage{
			17, 0, 17
		};
		auto coveredSidecar = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = {},
			.rawIndexData = indices,
			.coverage = armorCoverage,
			.sharedVertexResources = *sidecar,
			.profile = profile,
			.vertexCount = vertexCount,
			.vertexStride = stride,
			.geometryRevision = 9,
			.correctShellPipeline = true
		});
		a_tests.Check(coveredSidecar.has_value(),
			"binary armor coverage builds an immutable sidecar index buffer");
		if (coveredSidecar) {
			a_tests.Check((*coveredSidecar)->shellIndexBuffer.Get() &&
				(*coveredSidecar)->hiddenTriangleCount == 1,
				"covered sidecar records the removed triangle");
			const auto maskedIndices = ReadBuffer(
				a_tests, a_d3d, (*coveredSidecar)->shellIndexBuffer.Get());
			std::array<std::uint16_t, 3> decodedIndices{};
			if (maskedIndices.size() == sizeof(decodedIndices)) {
				std::memcpy(decodedIndices.data(), maskedIndices.data(),
					sizeof(decodedIndices));
			}
			a_tests.Check(decodedIndices == std::array<std::uint16_t, 3>{ 0, 0, 0 },
				"covered renderer triangle is degenerate without changing draw size");
		}

		constexpr std::uint32_t refinedVertexCount = 4;
		std::vector<std::byte> refinedVertices(
			static_cast<std::size_t>(stride) * refinedVertexCount);
		const std::array<std::array<float, 4>, refinedVertexCount> refinedPositions{
			std::array{ 0.0F, 0.0F, 0.0F, 1.0F },
			std::array{ 1.0F, 0.0F, 0.0F, 1.0F },
			std::array{ 1.0F, 1.0F, 0.0F, 1.0F },
			std::array{ 0.0F, 1.0F, 0.0F, 1.0F }
		};
		std::array<Core::CarrierVertex, refinedVertexCount> refinedCarrierVertices{};
		for (std::uint32_t index = 0; index < refinedVertexCount; ++index) {
			auto* destination = refinedVertices.data() +
				static_cast<std::size_t>(index) * stride;
			std::memcpy(destination, refinedPositions[index].data(),
				sizeof(refinedPositions[index]));
			refinedCarrierVertices[index] = {
				{ refinedPositions[index][0], refinedPositions[index][1],
					refinedPositions[index][2] },
				{ 0.0F, 0.0F, 1.0F }
			};
		}
		constexpr std::array<std::uint16_t, 6> refinedIndices{
			0, 1, 2, 0, 2, 3
		};
		auto refinedSidecar = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = vertices,
			.rawIndexData = indices,
			.shellVertexData = refinedVertices,
			.shellIndexData = refinedIndices,
			.shellCarrierVertices = refinedCarrierVertices,
			.shellVertexCount = refinedVertexCount,
			.profile = profile,
			.vertexCount = vertexCount,
			.vertexStride = stride,
			.geometryRevision = 10,
			.correctShellPipeline = true
		});
		a_tests.Check(refinedSidecar.has_value(),
			"refined sidecar accepts a packed 4-vertex/6-index shell mesh");
		if (refinedSidecar) {
			a_tests.Check(
				(*refinedSidecar)->sourceVertexCount == vertexCount &&
				(*refinedSidecar)->sourceIndexCount == indices.size() &&
				(*refinedSidecar)->shellVertexCount == refinedVertexCount &&
				(*refinedSidecar)->shellIndexCount == refinedIndices.size() &&
				(*refinedSidecar)->refinedShellTopology &&
				(*refinedSidecar)->shellIndexBuffer.Get(),
				"refined sidecar keeps source identity separate from shell topology");
			const auto refinedIndexBytes = ReadBuffer(
				a_tests, a_d3d, (*refinedSidecar)->shellIndexBuffer.Get());
			std::array<std::uint16_t, refinedIndices.size()> decodedRefinedIndices{};
			if (refinedIndexBytes.size() == sizeof(decodedRefinedIndices)) {
				std::memcpy(decodedRefinedIndices.data(), refinedIndexBytes.data(),
					sizeof(decodedRefinedIndices));
			}
			a_tests.Check(decodedRefinedIndices == refinedIndices,
				"refined immutable index buffer preserves the generated topology");
			const auto refinedExpanded = ReadBuffer(
				a_tests, a_d3d, (*refinedSidecar)->expandedVertexBuffer.Get());
			a_tests.Check(
				refinedExpanded.size() == static_cast<std::size_t>(stride) *
					refinedVertexCount * profile.shellCount,
				"expanded refined buffer uses shellVertexCount rather than source count");
		}

		auto invalidRefinedIndices = refinedIndices;
		invalidRefinedIndices.back() = refinedVertexCount;
		const auto invalidRefinedSidecar = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = vertices,
			.rawIndexData = indices,
			.shellVertexData = refinedVertices,
			.shellIndexData = invalidRefinedIndices,
			.shellCarrierVertices = refinedCarrierVertices,
			.shellVertexCount = refinedVertexCount,
			.profile = profile,
			.vertexCount = vertexCount,
			.vertexStride = stride
		});
		a_tests.Check(!invalidRefinedSidecar &&
			invalidRefinedSidecar.error() == Render::SidecarBuildError::InvalidIndex,
			"refined sidecar rejects an index outside shellVertexCount");
		const auto oversizedRefinedSidecar = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = vertices,
			.rawIndexData = indices,
			.shellVertexData = refinedVertices,
			.shellIndexData = refinedIndices,
			.shellCarrierVertices = refinedCarrierVertices,
			.shellVertexCount =
				static_cast<std::uint32_t>((std::numeric_limits<std::uint16_t>::max)()) + 1U,
			.profile = profile,
			.vertexCount = vertexCount,
			.vertexStride = stride
		});
		a_tests.Check(!oversizedRefinedSidecar &&
			oversizedRefinedSidecar.error() ==
				Render::SidecarBuildError::ResourceTooLarge,
			"refined R16 topology enforces its vertex-count hard cap");
		REX::W32::D3D11_DEPTH_STENCIL_DESC shellDepthDescription{};
		(*sidecar)->shellDepthState->GetDesc(&shellDepthDescription);
		a_tests.Check(
			shellDepthDescription.depthEnable &&
				shellDepthDescription.depthWriteMask ==
					REX::W32::D3D11_DEPTH_WRITE_MASK_ALL &&
				shellDepthDescription.depthFunc ==
					REX::W32::D3D11_COMPARISON_LESS_EQUAL &&
				!shellDepthDescription.stencilEnable,
			"shell state writes fur depth with LESS_EQUAL and leaves stencil disabled");
		auto reusedSidecar = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = {},
			.rawIndexData = indices,
			.sharedVertexResources = *sidecar,
			.profile = profile,
			.vertexCount = vertexCount,
			.vertexStride = stride,
			.geometryRevision = 9,
			.correctShellPipeline = true
		});
		a_tests.Check(reusedSidecar.has_value(), "partition sidecar resource reuse");
		if (reusedSidecar) {
			a_tests.Check(
				(*reusedSidecar)->expandedVertexBuffer.Get() ==
					(*sidecar)->expandedVertexBuffer.Get(),
				"partitions share one expanded shell vertex buffer");
			a_tests.Check(
				(*reusedSidecar)->alphaReferenceBuffers[0].Get() ==
					(*sidecar)->alphaReferenceBuffers[0].Get(),
				"partitions share immutable alpha-reference buffers");
		}
		auto changedProfile = profile;
		changedProfile.furLength += 0.25F;
		const auto mismatchedSharedProfile = Render::BuildNativeSidecar({
			.geometry = geometry,
			.device = a_d3d.device.Get(),
			.sourceVertexBuffer = vertexBuffer.Get(),
			.sourceIndexBuffer = indexBuffer.Get(),
			.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
			.vertexDesc = vertexDesc,
			.rawVertexData = {},
			.rawIndexData = indices,
			.sharedVertexResources = *sidecar,
			.profile = changedProfile,
			.vertexCount = vertexCount,
			.vertexStride = stride,
			.geometryRevision = 9
		});
		a_tests.Check(
			!mismatchedSharedProfile &&
				mismatchedSharedProfile.error() == Render::SidecarBuildError::InvalidArgument,
			"shared resources reject a different shell profile");

		const auto secondVertexBuffer = CreateBuffer(
			a_tests, a_d3d.device.Get(), vertices,
			REX::W32::D3D11_BIND_VERTEX_BUFFER);
		if (secondVertexBuffer.Get()) {
			const auto mismatchedSharedVertex = Render::BuildNativeSidecar({
				.geometry = geometry,
				.device = a_d3d.device.Get(),
				.sourceVertexBuffer = secondVertexBuffer.Get(),
				.sourceIndexBuffer = indexBuffer.Get(),
				.indexFormat = REX::W32::DXGI_FORMAT_R16_UINT,
				.vertexDesc = vertexDesc,
				.rawVertexData = {},
				.rawIndexData = indices,
				.sharedVertexResources = *sidecar,
				.profile = profile,
				.vertexCount = vertexCount,
				.vertexStride = stride,
				.geometryRevision = 9
			});
			a_tests.Check(
				!mismatchedSharedVertex &&
					mismatchedSharedVertex.error() == Render::SidecarBuildError::InvalidArgument,
				"shared resources reject a different source vertex buffer");
		}

		const auto expanded = ReadBuffer(
			a_tests, a_d3d, (*sidecar)->expandedVertexBuffer.Get());
		const auto layerBytes = static_cast<std::size_t>(stride) * vertexCount;
		a_tests.Check(
			expanded.size() == layerBytes * profile.shellCount,
			"expanded buffer contains one packed copy per shell");
		if (expanded.size() == layerBytes * profile.shellCount) {
			a_tests.Check(
				std::abs(ReadFloat(expanded, 2 * sizeof(float))) < 1.0e-4F,
				"inner shell retains the source position");
			const auto outerZ = ReadFloat(
				expanded, layerBytes * (profile.shellCount - 1) + 2 * sizeof(float));
			a_tests.Check(
				std::abs(outerZ - profile.furLength) < 0.01F,
				"outer shell is extruded by the configured fur length");
		}

		const auto outerAlpha = ReadBuffer(
			a_tests, a_d3d,
			(*sidecar)->alphaReferenceBuffers[profile.shellCount - 1].Get());
		a_tests.Check(
			std::abs(ReadFloat(outerAlpha, 0) -
				profile.alphaThresholds[profile.shellCount - 1]) < 1.0e-6F,
			"outer-shell b11 stores the canonical alpha threshold");

		Render::CarrierRegistry registry;
		const Render::CarrierRegistration registration{
			.sidecar = *sidecar,
			.viewShellCount = profile.shellCount,
			.shadowShellCount = profile.shadowShellCount,
			.stateRevision = 9,
			.enabled = true
		};
		a_tests.Check(
			registry.Register(registration) == Render::RegistrationResult::Added,
			"registry accepts a valid sidecar");
		const auto resolved = registry.Resolve({
			geometry,
			vertexBuffer.Get(),
			indexBuffer.Get(),
			REX::W32::DXGI_FORMAT_R16_UINT,
			stride,
			0,
			0
		});
		a_tests.Check(
			resolved.status == Render::CarrierResolveStatus::ExactMatch &&
				resolved.carrier &&
				resolved.carrier->viewShellCount == profile.shellCount,
			"registry resolves an exact IA/geometry identity");
		if (refinedSidecar) {
			Render::CarrierRegistry refinedRegistry;
			const Render::CarrierRegistration refinedRegistration{
				.sidecar = *refinedSidecar,
				.viewShellCount = profile.shellCount,
				.shadowShellCount = profile.shadowShellCount,
				.stateRevision = 10,
				.enabled = true
			};
			a_tests.Check(
				refinedRegistry.Register(refinedRegistration) ==
					Render::RegistrationResult::Added,
				"registry accepts refined topology under the source binding identity");
			const auto sourceResolved = refinedRegistry.Resolve({
				geometry, vertexBuffer.Get(), indexBuffer.Get(),
				REX::W32::DXGI_FORMAT_R16_UINT, stride, 0, 0
			});
			a_tests.Check(sourceResolved.carrier &&
				sourceResolved.carrier->sidecar == *refinedSidecar,
				"refined sidecar resolves through the original VB/IB");
			const auto shellBindingRejected = refinedRegistry.Resolve({
				geometry, (*refinedSidecar)->expandedVertexBuffer.Get(),
				(*refinedSidecar)->shellIndexBuffer.Get(),
				REX::W32::DXGI_FORMAT_R16_UINT, stride, 0, 0
			});
			a_tests.Check(
				shellBindingRejected.status ==
					Render::CarrierResolveStatus::CarrierIdentityMismatch,
				"refined output buffers never replace the source lookup identity");
		}
		const auto mismatchedFormat = registry.Resolve({
				geometry,
				vertexBuffer.Get(),
				indexBuffer.Get(),
				REX::W32::DXGI_FORMAT_R32_UINT,
				stride,
				0,
				0
			});
		a_tests.Check(
			mismatchedFormat.status ==
				Render::CarrierResolveStatus::CarrierIdentityMismatch,
			"registry rejects a mismatched index format");
		const auto unrelatedGeometry = registry.Resolve({
			reinterpret_cast<RE::BSGeometry*>(static_cast<std::uintptr_t>(2)),
			vertexBuffer.Get(),
			indexBuffer.Get(),
			REX::W32::DXGI_FORMAT_R16_UINT,
			stride,
			0,
			0
		});
		a_tests.Check(
			unrelatedGeometry.status ==
				Render::CarrierResolveStatus::NotCarrierGeometry,
			"registry distinguishes unrelated geometry from a stale carrier");
		if (coveredSidecar) {
			const std::array replacement{
				Render::CarrierRegistration{
					.sidecar = *coveredSidecar,
					.viewShellCount = profile.shellCount,
					.shadowShellCount = profile.shadowShellCount,
					.stateRevision = 10,
					.enabled = true
				}
			};
			a_tests.Check(registry.ReplaceAll(replacement),
				"registry atomically replaces a complete coverage revision");
			const auto replaced = registry.Resolve({
				geometry, vertexBuffer.Get(), indexBuffer.Get(),
				REX::W32::DXGI_FORMAT_R16_UINT, stride, 0, 0
			});
			a_tests.Check(replaced.carrier &&
				replaced.carrier->sidecar == *coveredSidecar &&
				replaced.carrier->stateRevision == 10,
				"replacement revision is immediately observable as one snapshot");

			const std::array invalidReplacement{ Render::CarrierRegistration{} };
			a_tests.Check(!registry.ReplaceAll(invalidReplacement),
				"invalid atomic replacement is rejected");
			const auto retained = registry.Resolve({
				geometry, vertexBuffer.Get(), indexBuffer.Get(),
				REX::W32::DXGI_FORMAT_R16_UINT, stride, 0, 0
			});
			a_tests.Check(retained.carrier &&
				retained.carrier->sidecar == *coveredSidecar,
				"failed replacement retains the previous visible coverage revision");
		}

		auto* activeVertexForRouter = vertexBuffer.Get();
		std::uint32_t routerOffset{};
		a_d3d.context->IASetVertexBuffers(
			0, 1, &activeVertexForRouter, &stride, &routerOffset);
		a_d3d.context->IASetIndexBuffer(
			indexBuffer.Get(), REX::W32::DXGI_FORMAT_R16_UINT, 0);
		Render::RenderRouter router(registry);
		Render::FurPassContext carrierPass;
		carrierPass.geometry = geometry;
		const auto rejectedCarrier = router.Prepare(
			a_d3d.context.Get(), carrierPass,
			{ static_cast<std::uint32_t>(indices.size()), 0, 0 });
		a_tests.Check(
			rejectedCarrier.carrierMatched &&
				rejectedCarrier.status == Render::PrepareStatus::UnsupportedBackend,
			"unsupported backend remains fail-closed for a registered carrier");
		carrierPass.geometry =
			reinterpret_cast<RE::BSGeometry*>(static_cast<std::uintptr_t>(2));
		const auto unrelatedDraw = router.Prepare(
			a_d3d.context.Get(), carrierPass,
			{ static_cast<std::uint32_t>(indices.size()), 0, 0 });
		a_tests.Check(
			!unrelatedDraw.carrierMatched &&
				unrelatedDraw.status == Render::PrepareStatus::NoCarrier,
			"unrelated geometry remains a normal passthrough draw");

		alignas(16) constexpr std::array<float, 4> originalReference{
			0.25F, 0.0F, 0.0F, 0.0F
		};
		const auto originalConstant = CreateBuffer(
			a_tests,
			a_d3d.device.Get(),
			std::as_bytes(std::span{ originalReference }),
			REX::W32::D3D11_BIND_CONSTANT_BUFFER);
		auto* originalVertex = vertexBuffer.Get();
		std::uint32_t originalOffset{};
		a_d3d.context->IASetVertexBuffers(
			0, 1, &originalVertex, &stride, &originalOffset);
		auto* originalB11 = originalConstant.Get();
		a_d3d.context->PSSetConstantBuffers(
			Render::kAlphaReferenceConstantSlot, 1, &originalB11);
		const auto pixelShader = CreatePixelShader(a_tests, a_d3d.device.Get());
		if (pixelShader.Get()) {
			a_d3d.context->PSSetShader(pixelShader.Get(), nullptr, 0);
			router.SetAdapter(std::make_shared<Render::NativeMultiDrawAdapter>());
			Render::FurPassContext inferredPass;
			inferredPass.geometry = geometry;
			inferredPass.alphaTest = true;
			inferredPass.kind = Render::FurPassKind::Lighting;
			const auto inferred = router.PrepareBoundCarrier(
				a_d3d.context.Get(), inferredPass);
			a_tests.Check(
				inferred.IsReady() && inferred.carrierMatched &&
					inferred.draw.layerCount == profile.shellCount &&
					inferred.draw.indexCount == indices.size(),
				"shader-pass fallback infers the exact source draw from live IA bindings");
		}
		if (pixelShader.Get() && refinedSidecar) {
			a_d3d.context->PSSetShader(pixelShader.Get(), nullptr, 0);
			Render::NativeMultiDrawAdapter adapter;
			Render::FurPassContext refinedPass;
			refinedPass.geometry = geometry;
			refinedPass.alphaTest = true;
			refinedPass.kind = Render::FurPassKind::Lighting;
			const Render::ResolvedCarrier refinedCarrier{
				.sidecar = *refinedSidecar,
				.viewShellCount = profile.shellCount,
				.shadowShellCount = profile.shadowShellCount,
				.stateRevision = 10,
				.enabled = true
			};
			const auto refinedPrepared = adapter.Prepare(
				a_d3d.context.Get(), refinedPass,
				{ static_cast<std::uint32_t>(indices.size()), 0, 0 }, refinedCarrier);
			a_tests.Check(refinedPrepared.IsReady() &&
				refinedPrepared.draw.shellIndexBuffer.Get() ==
					(*refinedSidecar)->shellIndexBuffer.Get() &&
				refinedPrepared.draw.indexCount == refinedIndices.size() &&
				refinedPrepared.draw.startIndex == 0,
				"prepare replaces the source 3-index draw with refined 6-index args");
			if (refinedPrepared.IsReady() && refinedPrepared.draw.layerCount >= 2) {
				a_tests.Check(
					refinedPrepared.draw.layers[0].baseVertex == 0 &&
					refinedPrepared.draw.layers[1].baseVertex ==
						static_cast<std::int32_t>(refinedVertexCount),
					"refined shell bases advance by shellVertexCount from zero");
			}
			const auto mismatchedSourceDraw = adapter.Prepare(
				a_d3d.context.Get(), refinedPass,
				{ static_cast<std::uint32_t>(indices.size() - 1), 0, 0 },
				refinedCarrier);
			a_tests.Check(
				mismatchedSourceDraw.status == Render::PrepareStatus::InvalidDraw,
				"prepare rejects a draw that does not exactly match source topology");
		}
		REX::W32::D3D11_BLEND_DESC inheritedBlendDescription{};
		inheritedBlendDescription.independentBlendEnable = 1;
		for (auto& target : inheritedBlendDescription.renderTarget) {
			target.blendEnable = 0;
			target.renderTargetWriteMask =
				REX::W32::D3D11_COLOR_WRITE_ENABLE_RED |
				REX::W32::D3D11_COLOR_WRITE_ENABLE_GREEN |
				REX::W32::D3D11_COLOR_WRITE_ENABLE_BLUE;
		}
		REX::W32::ComPtr<REX::W32::ID3D11BlendState> inheritedBlend;
		a_tests.Check(
			a_d3d.device->CreateBlendState(
				&inheritedBlendDescription,
				inheritedBlend.ReleaseAndGetAddressOf()) >= 0,
			"Skyrim-like RGB-only inherited blend state creation");
		constexpr std::array<float, 4> inheritedBlendFactor{ 1.0F, 1.0F, 1.0F, 1.0F };
		a_d3d.context->OMSetBlendState(
			inheritedBlend.Get(), inheritedBlendFactor.data(), 0xFFFFFFFFU);

		Render::PreparedShellDraw prepared;
		prepared.expandedVertexBuffer = (*sidecar)->expandedVertexBuffer;
		if (coveredSidecar) {
			prepared.shellIndexBuffer = (*coveredSidecar)->shellIndexBuffer;
		}
		prepared.shellDepthState = (*sidecar)->shellDepthState;
		prepared.vertexStride = stride;
		prepared.indexCount = static_cast<std::uint32_t>(indices.size());
		prepared.layerCount = 1;
		prepared.layers[0] = {
			0,
			0,
			(*sidecar)->alphaReferenceBuffers[0]
		};
		{
			Render::ScopedNativeDrawState state(a_d3d.context.Get(), prepared);
			a_tests.Check(state.IsApplied(), "draw state applies the expanded vertex buffer");
			a_tests.Check(state.BindLayer(prepared.layers[0]), "draw state binds shell b11");

			REX::W32::ComPtr<REX::W32::ID3D11Buffer> activeVertex;
			std::uint32_t activeStride{};
			std::uint32_t activeOffset{};
			a_d3d.context->IAGetVertexBuffers(
				0, 1, activeVertex.ReleaseAndGetAddressOf(),
				&activeStride, &activeOffset);
			a_tests.Check(
				activeVertex.Get() == (*sidecar)->expandedVertexBuffer.Get() &&
					activeStride == stride,
				"expanded vertex binding is observable inside the scope");
			REX::W32::ComPtr<REX::W32::ID3D11Buffer> activeIndex;
			REX::W32::DXGI_FORMAT activeIndexFormat{};
			std::uint32_t activeIndexOffset{};
			a_d3d.context->IAGetIndexBuffer(
				activeIndex.ReleaseAndGetAddressOf(), &activeIndexFormat,
				&activeIndexOffset);
			a_tests.Check(!coveredSidecar ||
				(activeIndex.Get() == (*coveredSidecar)->shellIndexBuffer.Get() &&
					activeIndexFormat == REX::W32::DXGI_FORMAT_R16_UINT &&
					activeIndexOffset == 0),
				"armor coverage index binding is observable inside the scope");

			REX::W32::ComPtr<REX::W32::ID3D11DepthStencilState> activeDepth;
			std::uint32_t activeStencilReference{};
			a_d3d.context->OMGetDepthStencilState(
				activeDepth.ReleaseAndGetAddressOf(), &activeStencilReference);
			a_tests.Check(
				activeDepth.Get() == (*sidecar)->shellDepthState.Get(),
				"shell depth correction is observable inside the scope");
			REX::W32::ComPtr<REX::W32::ID3D11BlendState> activeBlend;
			std::array<float, 4> activeBlendFactor{};
			std::uint32_t activeSampleMask{};
			a_d3d.context->OMGetBlendState(
				activeBlend.ReleaseAndGetAddressOf(), activeBlendFactor.data(),
				&activeSampleMask);
			a_tests.Check(
				activeBlend.Get() == inheritedBlend.Get() &&
					activeSampleMask == 0xFFFFFFFFU,
				"draw scope preserves Skyrim's inherited RGB-only blend state");
		}

		REX::W32::ComPtr<REX::W32::ID3D11Buffer> restoredVertex;
		std::uint32_t restoredStride{};
		std::uint32_t restoredOffset{};
		a_d3d.context->IAGetVertexBuffers(
			0, 1, restoredVertex.ReleaseAndGetAddressOf(),
			&restoredStride, &restoredOffset);
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> restoredB11;
		a_d3d.context->PSGetConstantBuffers(
			Render::kAlphaReferenceConstantSlot,
			1,
			restoredB11.ReleaseAndGetAddressOf());
		a_tests.Check(
			restoredVertex.Get() == vertexBuffer.Get() &&
			restoredStride == stride && restoredOffset == 0,
			"draw scope restores the original IA vertex binding");
		REX::W32::ComPtr<REX::W32::ID3D11Buffer> restoredIndex;
		REX::W32::DXGI_FORMAT restoredIndexFormat{};
		std::uint32_t restoredIndexOffset{};
		a_d3d.context->IAGetIndexBuffer(
			restoredIndex.ReleaseAndGetAddressOf(), &restoredIndexFormat,
			&restoredIndexOffset);
		a_tests.Check(restoredIndex.Get() == indexBuffer.Get() &&
			restoredIndexFormat == REX::W32::DXGI_FORMAT_R16_UINT &&
			restoredIndexOffset == 0,
			"draw scope restores the original IA index binding");
		a_tests.Check(
			restoredB11.Get() == originalConstant.Get(),
			"draw scope restores the original pixel b11 binding");
		REX::W32::ComPtr<REX::W32::ID3D11DepthStencilState> restoredDepth;
		std::uint32_t restoredStencilReference{};
		a_d3d.context->OMGetDepthStencilState(
			restoredDepth.ReleaseAndGetAddressOf(), &restoredStencilReference);
		a_tests.Check(
			!restoredDepth.Get(),
			"draw scope restores the default depth state");
		REX::W32::ComPtr<REX::W32::ID3D11BlendState> restoredBlend;
		std::array<float, 4> restoredBlendFactor{};
		std::uint32_t restoredSampleMask{};
		a_d3d.context->OMGetBlendState(
			restoredBlend.ReleaseAndGetAddressOf(), restoredBlendFactor.data(),
			&restoredSampleMask);
		a_tests.Check(
			restoredBlend.Get() == inheritedBlend.Get() &&
				restoredSampleMask == 0xFFFFFFFFU,
			"draw scope leaves the inherited blend state untouched");
	}

	void TestUtilityPassClassification(TestContext& a_tests)
	{
		// Captured from the live 3BA carrier on Skyrim 1.6.1170. This depth
		// technique deliberately has no generic Texture or Skinned flag bit.
		constexpr std::uint32_t observedDepthTechnique = 0x000020B1U;
		a_tests.Check(
			Render::ClassifyUtilityPass(observedDepthTechnique, true) ==
				Render::FurPassKind::Depth,
			"observed alpha-tested utility depth technique is accepted");
		a_tests.Check(
			Render::ClassifyUtilityPass(observedDepthTechnique, false) ==
				Render::FurPassKind::Unsupported,
			"utility depth technique still requires a live alpha-test property");
		a_tests.Check(
			Render::ClassifyUtilityPass(0x00002031U, true) ==
				Render::FurPassKind::Unsupported,
			"utility depth technique still requires the shader alpha-test flag");
	}

	void TestLightingPassClassification(TestContext& a_tests)
	{
		constexpr std::uint32_t runtime1170Technique = 0x4D506633U;
		constexpr std::uint32_t inheritedBodyTechnique = 0x4D406633U;
		a_tests.Check(
			Render::ClassifyLightingPass(runtime1170Technique, true) ==
				Render::FurPassKind::Lighting,
			"1.6.1170 alpha-tested skinned lighting technique is accepted");
		a_tests.Check(
			Render::ClassifyLightingPass(inheritedBodyTechnique, true) ==
				Render::FurPassKind::Unsupported,
			"non-alpha body permutation is rejected despite a live alpha property");
		a_tests.Check(
			Render::ClassifyLightingPass(runtime1170Technique, false) ==
				Render::FurPassKind::Unsupported,
			"lighting technique still requires a live alpha-test property");
		a_tests.Check(
			Render::ClassifyLightingPass(runtime1170Technique & ~0x2U, true) ==
				Render::FurPassKind::Unsupported,
			"lighting technique still requires skinned geometry");
	}

	void TestIndexedDrawHookInstallation(
		TestContext& a_tests,
		const DeviceContext& a_d3d)
	{
		auto* context = a_d3d.context.Get();
		auto* vtable = context ? *reinterpret_cast<std::uintptr_t**>(context) : nullptr;
		a_tests.Check(vtable != nullptr, "D3D11 immediate-context vtable");
		if (!vtable) {
			return;
		}

		const auto originalIndexed =
			vtable[Render::RenderHooks::kDrawIndexedVtableIndex];
		const auto originalInstanced =
			vtable[Render::RenderHooks::kDrawIndexedInstancedVtableIndex];
		Render::CarrierRegistry registry;
		Render::RenderRouter router(registry);
		auto& hooks = Render::RenderHooks::GetSingleton();

		const auto installed = hooks.InstallDrawHooks(context, router);
		a_tests.Check(installed && hooks.IsInstalled(),
			"both indexed D3D11 draw hooks install as one routing unit");
		if (!installed) {
			return;
		}
		a_tests.Check(
			vtable[Render::RenderHooks::kDrawIndexedVtableIndex] != originalIndexed,
			"DrawIndexed vtable entry is intercepted");
		a_tests.Check(
			vtable[Render::RenderHooks::kDrawIndexedInstancedVtableIndex] !=
				originalInstanced,
			"DrawIndexedInstanced vtable entry is intercepted");

		const auto uninstalled = hooks.UninstallDrawHooks();
		a_tests.Check(uninstalled && !hooks.IsInstalled(),
			"both indexed D3D11 draw hooks uninstall as one routing unit");
		a_tests.Check(
			vtable[Render::RenderHooks::kDrawIndexedVtableIndex] == originalIndexed &&
			vtable[Render::RenderHooks::kDrawIndexedInstancedVtableIndex] ==
				originalInstanced,
			"indexed D3D11 vtable entries are restored exactly");
	}
}

int main()
{
	TestContext tests;
	TestLightingPassClassification(tests);
	TestUtilityPassClassification(tests);
	const auto d3d = CreateWarpDevice(tests);
	if (d3d.device.Get() && d3d.context.Get()) {
		TestSidecarAndRegistry(tests, d3d);
		TestIndexedDrawHookInstallation(tests, d3d);
	}

	if (tests.failures == 0) {
		std::cout << "DynamicFur Render tests passed\n";
		return 0;
	}
	std::cerr << tests.failures << " DynamicFur Render test(s) failed\n";
	return 1;
}
