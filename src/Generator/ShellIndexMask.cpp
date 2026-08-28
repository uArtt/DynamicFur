#include "DynamicFur/Generator/ShellIndexMask.h"

#include <algorithm>

namespace DynamicFur::Generator
{
	ShellIndexMaskGenerationResult GenerateBinaryShellIndexMask(
		const ShellIndexMaskRequest& a_request)
	{
		if (a_request.vertexCount == 0 || a_request.shellCount == 0 ||
			a_request.shellCount > Core::kMaximumShellCount ||
			a_request.coverage.size() != a_request.vertexCount ||
			a_request.sourceIndices.empty() ||
			(a_request.sourceIndices.size() % 3U) != 0) {
			return std::unexpected(ShellIndexMaskError::InvalidArgument);
		}

		ShellIndexMaskResult result;
		result.indices.assign(
			a_request.sourceIndices.begin(), a_request.sourceIndices.end());
		for (std::size_t triangle = 0; triangle < result.indices.size(); triangle += 3) {
			const auto first = static_cast<std::size_t>(result.indices[triangle]);
			const auto second = static_cast<std::size_t>(result.indices[triangle + 1]);
			const auto third = static_cast<std::size_t>(result.indices[triangle + 2]);
			if (first >= a_request.vertexCount || second >= a_request.vertexCount ||
				third >= a_request.vertexCount) {
				return std::unexpected(ShellIndexMaskError::InvalidIndex);
			}

			const auto firstHidden = std::min({
				a_request.coverage[first],
				a_request.coverage[second],
				a_request.coverage[third]
			});
			if (firstHidden < a_request.shellCount) {
				result.indices[triangle + 1] = result.indices[triangle];
				result.indices[triangle + 2] = result.indices[triangle];
				++result.hiddenTriangleCount;
			}
		}

		return result;
	}

	CompactShellIndexMaskGenerationResult GenerateCompactShellIndexMask(
		const CompactShellIndexMaskRequest& a_request)
	{
		if (a_request.vertexCount == 0 || a_request.shellCount == 0 ||
			a_request.shellCount > Core::kMaximumShellCount ||
			a_request.vertexCoverage.size() != a_request.vertexCount ||
			a_request.sourceIndices.empty() ||
			(a_request.sourceIndices.size() % 3U) != 0 ||
			(!a_request.triangleCoverage.empty() &&
			 a_request.triangleCoverage.size() != a_request.sourceIndices.size() / 3U)) {
			return std::unexpected(ShellIndexMaskError::InvalidArgument);
		}

		CompactShellIndexMaskResult result;
		result.indices.reserve(a_request.sourceIndices.size());
		for (std::size_t triangle = 0;
			triangle < a_request.sourceIndices.size(); triangle += 3) {
			const auto first = static_cast<std::size_t>(a_request.sourceIndices[triangle]);
			const auto second = static_cast<std::size_t>(a_request.sourceIndices[triangle + 1]);
			const auto third = static_cast<std::size_t>(a_request.sourceIndices[triangle + 2]);
			if (first >= a_request.vertexCount || second >= a_request.vertexCount ||
				third >= a_request.vertexCount) {
				return std::unexpected(ShellIndexMaskError::InvalidIndex);
			}

			auto firstHidden = std::min({
				a_request.vertexCoverage[first],
				a_request.vertexCoverage[second],
				a_request.vertexCoverage[third]
			});
			if (!a_request.triangleCoverage.empty()) {
				firstHidden = std::min(
					firstHidden, a_request.triangleCoverage[triangle / 3U]);
			}
			if (firstHidden < a_request.shellCount) {
				++result.hiddenTriangleCount;
				continue;
			}

			result.indices.insert(
				result.indices.end(),
				a_request.sourceIndices.begin() + triangle,
				a_request.sourceIndices.begin() + triangle + 3);
			++result.visibleTriangleCount;
		}

		if (result.indices.empty()) {
			result.indices = { 0, 0, 0 };
			result.emittedSafetyDegenerate = true;
		}
		return result;
	}
}
