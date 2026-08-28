#include "DynamicFur/Core/ShellCoverage.h"
#include "DynamicFur/Core/JsonStringArray.h"
#include "DynamicFur/Core/ShellLod.h"
#include "DynamicFur/Core/ShellMath.h"
#include "DynamicFur/Core/ShellProfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using namespace DynamicFur::Core;

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

	[[nodiscard]] bool Near(float a_lhs, float a_rhs, float a_epsilon = 1.0e-5F)
	{
		return std::abs(a_lhs - a_rhs) <= a_epsilon;
	}

	[[nodiscard]] Triangle PlaneAt(float a_z)
	{
		return {
			Vec3{ -1.0F, -1.0F, a_z },
			Vec3{ 1.0F, -1.0F, a_z },
			Vec3{ 0.0F, 1.0F, a_z }
		};
	}

	void TestProfileValidation(TestContext& a_tests)
	{
		const ShellProfile defaults;
		a_tests.Check(defaults.Validate().empty(), "the default profile is valid");

		const auto legacy = ShellProfile::Legacy17();
		a_tests.Check(legacy.Validate().empty(), "Legacy17 is valid");
		a_tests.Check(legacy.shellCount == 17, "Legacy17 has 17 shells");
		a_tests.Check(Near(legacy.alphaThresholds.front(), 0.0F),
			"Legacy17 starts at a zero alpha threshold");
		a_tests.Check(Near(legacy.alphaThresholds[16], 240.0F / 255.0F),
			"Legacy17 ends at a 240/255 alpha threshold");
		for (std::uint32_t layer = 1; layer < legacy.shellCount; ++layer) {
			a_tests.Check(
				Near(
					legacy.alphaThresholds[layer] - legacy.alphaThresholds[layer - 1],
					15.0F / 255.0F),
				"Legacy17 thresholds advance uniformly by 15/255");
		}

		auto invalid = legacy;
		invalid.shellCount = 0;
		a_tests.Check(!invalid.Validate().empty(), "zero shells are rejected");
		invalid = legacy;
		invalid.shellCount = kMaximumShellCount + 1;
		a_tests.Check(!invalid.Validate().empty(), "too many shells are rejected");
		invalid = legacy;
		invalid.alphaThresholds[4] = std::numeric_limits<float>::quiet_NaN();
		a_tests.Check(!invalid.Validate().empty(), "NaN alpha thresholds are rejected");
		invalid = legacy;
		invalid.lodBands[2].shellCount = 13;
		a_tests.Check(!invalid.Validate().empty(), "LOD shell counts cannot increase with distance");
		invalid = legacy;
		invalid.lodHysteresis = invalid.lodBands.front().maximumDistance;
		a_tests.Check(!invalid.Validate().empty(), "irreversible LOD hysteresis is rejected");
		invalid = legacy;
		invalid.furLength = std::numeric_limits<float>::max();
		invalid.armorMargin = std::numeric_limits<float>::max();
		a_tests.Check(!invalid.Validate().empty(), "overflowing coverage ray length is rejected");
		invalid = legacy;
		invalid.armorUnderSkinDepth = -0.01F;
		a_tests.Check(!invalid.Validate().empty(), "negative under-skin depth is rejected");
		invalid.armorUnderSkinDepth = std::numeric_limits<float>::infinity();
		a_tests.Check(!invalid.Validate().empty(), "non-finite under-skin depth is rejected");
		invalid = legacy;
		invalid.furLength = std::numeric_limits<float>::max();
		invalid.armorUnderSkinDepth = std::numeric_limits<float>::max();
		a_tests.Check(!invalid.Validate().empty(), "overflowing under-skin ray length is rejected");
	}

	void TestShellLayout(TestContext& a_tests)
	{
		const auto profile = ShellProfile::Legacy17();
		const auto layoutResult = ShellLayout::Create(profile);
		a_tests.Check(layoutResult.has_value(), "a valid profile creates a ShellLayout");
		if (!layoutResult) {
			return;
		}

		const auto& layout = *layoutResult;
		a_tests.Check(layout.ShellCount() == 17, "layout retains shell count");
		a_tests.Check(layout.CanonicalSamples().size() == 17, "layout exposes canonical samples");
		a_tests.Check(Near(layout.CanonicalSamples().front().offset, 0.0F),
			"first shell starts at the carrier");
		a_tests.Check(Near(layout.CanonicalSamples().back().offset, 0.868F),
			"last shell reaches full fur length");

		constexpr std::array<std::uint32_t, 4> expectedIndices{ 0, 5, 11, 16 };
		for (std::uint32_t index = 0; index < expectedIndices.size(); ++index) {
			a_tests.Check(CanonicalLayerIndex(index, 4, 17) == expectedIndices[index],
				"four-shell LOD preserves canonical endpoints and spacing");
			a_tests.Check(layout.Sample(index, 4).canonicalIndex == expectedIndices[index],
				"layout LOD sampling uses canonical mapping");
		}

		a_tests.Check(layout.FirstShellAtOrBeyond(0.0F) == 0,
			"a skin-level hit hides from shell zero");
		a_tests.Check(layout.FirstShellAtOrBeyond(0.2F) == 4,
			"distance lookup uses precomputed offsets");
		a_tests.Check(layout.FirstShellAtOrBeyond(1.0F) == 17,
			"a hit beyond fur leaves every shell visible");
		a_tests.Check(layout.FirstShellAtOrBeyond(std::numeric_limits<float>::infinity()) == 17,
			"a non-finite hit is neutral");

		auto invalid = profile;
		invalid.shellCount = std::numeric_limits<std::uint32_t>::max();
		const auto safeSample = SampleShell(invalid, invalid.shellCount, invalid.shellCount);
		a_tests.Check(safeSample.canonicalIndex < kMaximumShellCount,
			"unchecked convenience sampling cannot index outside fixed storage");
		a_tests.Check(std::isfinite(safeSample.offset) && std::isfinite(safeSample.vertexAlphaScale),
			"unchecked convenience sampling fails safely");
		a_tests.Check(!ShellLayout::Create(invalid).has_value(),
			"ShellLayout rejects an invalid profile");

		const auto hugeNormal = Normalize({ std::numeric_limits<float>::max(), 0.0F, 0.0F });
		a_tests.Check(Near(hugeNormal.x, 1.0F) && Near(hugeNormal.y, 0.0F) && Near(hugeNormal.z, 0.0F),
			"normalization avoids finite-input overflow");
	}

	void TestLod(TestContext& a_tests)
	{
		const auto profile = ShellProfile::Legacy17();
		LodSelector selector;
		a_tests.Check(selector.CurrentShellCount() == 0, "uninitialized LOD has no current count");
		a_tests.Check(selector.Update(profile, 0.0F) == 17, "near LOD selects 17 shells");
		a_tests.Check(selector.CurrentShellCount() == 17, "current LOD reports shell count, not band index");
		a_tests.Check(selector.Update(profile, 950.0F) == 17, "outward hysteresis retains near LOD");
		a_tests.Check(selector.Update(profile, 1001.0F) == 12, "outward hysteresis crosses after upper edge");
		a_tests.Check(selector.Update(profile, 850.0F) == 12, "inward hysteresis retains middle LOD");
		a_tests.Check(selector.Update(profile, 799.0F) == 17, "inward hysteresis crosses below lower edge");
		a_tests.Check(SelectShellCount(profile, 900.0F) == 17, "stateless LOD includes its maximum distance");
		a_tests.Check(SelectShellCount(profile, 900.5F) == 12, "stateless LOD advances beyond its boundary");

		selector.Reset();
		a_tests.Check(selector.CurrentShellCount() == 0, "Reset clears current LOD count");
		a_tests.Check(selector.Update(profile, std::numeric_limits<float>::infinity()) == 4,
			"infinite distance selects far LOD");
		a_tests.Check(selector.Update(profile, 0.0F) == 17,
			"selector can cross several bands back toward the camera");
	}

	void TestCoverageGeometry(TestContext& a_tests)
	{
		const auto profile = ShellProfile::Legacy17();
		const auto layoutResult = ShellLayout::Create(profile);
		if (!layoutResult) {
			a_tests.Check(false, "coverage test requires a valid layout");
			return;
		}
		const auto& layout = *layoutResult;

		const std::array vertices{ CarrierVertex{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } } };
		const std::array armor{ PlaneAt(0.2F) };
		const auto covered = SolveArmorCoverage(layout, vertices, armor);
		a_tests.Check(covered && (*covered)[0] == 4, "plane at 0.2 hides shell four and above");
		a_tests.Check(FirstHiddenShellFromDistance(profile, 0.2F) == 4,
			"profile distance convenience function matches ShellLayout");

		const std::array atSkin{ PlaneAt(0.0F) };
		const auto skinHit = SolveArmorCoverage(layout, vertices, atSkin);
		a_tests.Check(skinHit && (*skinHit)[0] == 0, "plane at skin hides every shell");

		const std::array underSkin{ PlaneAt(-0.1F) };
		const auto underSkinHit = SolveArmorCoverage(layout, vertices, underSkin);
		a_tests.Check(underSkinHit && (*underSkinHit)[0] == 0,
			"armor immediately beneath the skin hides every shell");

		const std::array behindTolerance{ PlaneAt(-0.75F) };
		const auto behindToleranceHit = SolveArmorCoverage(layout, vertices, behindTolerance);
		a_tests.Check(behindToleranceHit && (*behindToleranceHit)[0] == 17,
			"geometry beyond the short under-skin probe is ignored");

		auto noUnderSkinProfile = profile;
		noUnderSkinProfile.armorUnderSkinDepth = 0.0F;
		const auto noUnderSkinHit = SolveArmorCoverage(noUnderSkinProfile, vertices, underSkin);
		a_tests.Check(noUnderSkinHit && (*noUnderSkinHit)[0] == 17,
			"zero under-skin depth disables the inward probe");

		const std::array degenerate{ Triangle{ { 0.0F, 0.0F, 0.2F }, { 0.0F, 0.0F, 0.2F }, { 0.0F, 0.0F, 0.2F } } };
		const auto degenerateHit = SolveArmorCoverage(layout, vertices, degenerate);
		a_tests.Check(degenerateHit && (*degenerateHit)[0] == 17, "degenerate triangles are ignored");

		const std::array zeroNormal{ CarrierVertex{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 0.0F } } };
		const auto zeroNormalHit = SolveArmorCoverage(layout, zeroNormal, armor);
		a_tests.Check(zeroNormalHit && (*zeroNormalHit)[0] == 17, "zero normals remain neutral");

		const auto noArmor = SolveArmorCoverage(
			layout, vertices, std::span<const Triangle>{});
		a_tests.Check(noArmor && (*noArmor)[0] == 17, "no armor leaves every shell visible");

		auto nanTriangle = PlaneAt(0.2F);
		nanTriangle.a.x = std::numeric_limits<float>::quiet_NaN();
		const std::array invalidArmor{ nanTriangle };
		a_tests.Check(!SolveArmorCoverage(layout, vertices, invalidArmor),
			"non-finite armor geometry is rejected");

		const std::array invalidVertices{ CarrierVertex{
			{ std::numeric_limits<float>::infinity(), 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } } };
		a_tests.Check(!SolveArmorCoverage(layout, invalidVertices, armor),
			"non-finite carrier geometry is rejected");

		auto invalidProfile = profile;
		invalidProfile.shellCount = kMaximumShellCount + 1;
		a_tests.Check(!SolveArmorCoverage(invalidProfile, vertices, armor),
			"profile overload propagates layout validation errors");
	}

	void TestReusableCoverageQuery(TestContext& a_tests)
	{
		auto profile = ShellProfile::Legacy17();
		profile.furLength = 1.25F;
		const auto layoutResult = ShellLayout::Create(profile);
		if (!layoutResult) {
			a_tests.Check(false, "reusable coverage query requires a valid layout");
			return;
		}

		const std::array armor{ PlaneAt(0.2F) };
		const auto profileQuery = ArmorCoverageQuery::Create(profile, armor);
		const auto layoutQuery = ArmorCoverageQuery::Create(*layoutResult, armor);
		a_tests.Check(profileQuery.has_value(),
			"a profile and finite armor create a reusable coverage query");
		a_tests.Check(layoutQuery.has_value(),
			"a prevalidated layout creates a reusable coverage query");
		if (!profileQuery || !layoutQuery) {
			return;
		}

		a_tests.Check(!profileQuery->Empty(),
			"a query containing armor triangles is not empty");
		a_tests.Check(profileQuery->ShellCount() == profile.shellCount,
			"a query retains the profile shell count");
		a_tests.Check(Near(profileQuery->FurLength(), profile.furLength),
			"a query retains the profile fur length");
		a_tests.Check(Near(profileQuery->UnderSkinDepth(), profile.armorUnderSkinDepth),
			"a query retains the profile under-skin depth");

		const std::array firstBatch{
			CarrierVertex{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
			CarrierVertex{ { 2.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } }
		};
		const std::array secondBatch{
			CarrierVertex{ { -0.25F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
			CarrierVertex{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, -1.0F } }
		};
		const auto firstSample = profileQuery->Sample(firstBatch);
		const auto firstWrapper = SolveArmorCoverage(profile, firstBatch, armor);
		const auto secondSample = profileQuery->Sample(secondBatch);
		const auto secondWrapper = SolveArmorCoverage(*layoutResult, secondBatch, armor);
		const auto layoutSample = layoutQuery->Sample(firstBatch);
		a_tests.Check(firstSample && firstWrapper &&
			std::ranges::equal(firstSample->Values(), firstWrapper->Values()),
			"query sampling is equivalent to the profile wrapper");
		a_tests.Check(secondSample && secondWrapper &&
			std::ranges::equal(secondSample->Values(), secondWrapper->Values()),
			"one query can be reused for a second independent batch");
		a_tests.Check(firstSample && layoutSample &&
			std::ranges::equal(firstSample->Values(), layoutSample->Values()),
			"profile and layout query factories produce equivalent samples");

		const AxisAlignedBounds overlapping{
			{ -0.25F, -0.25F, 0.19F }, { 0.25F, 0.25F, 0.21F }
		};
		const AxisAlignedBounds separated{
			{ 10.0F, 10.0F, 10.0F }, { 11.0F, 11.0F, 11.0F }
		};
		a_tests.Check(profileQuery->MayIntersect(overlapping),
			"broad-phase bounds overlapping armor may intersect");
		a_tests.Check(!profileQuery->MayIntersect(separated),
			"broad-phase bounds separated from armor do not intersect");

		const AxisAlignedBounds reversed{
			{ 1.0F, 0.0F, 0.0F }, { -1.0F, 1.0F, 1.0F }
		};
		const auto nan = std::numeric_limits<float>::quiet_NaN();
		const AxisAlignedBounds nonFinite{
			{ nan, 0.0F, 0.0F }, { 1.0F, 1.0F, 1.0F }
		};
		a_tests.Check(!profileQuery->MayIntersect(reversed),
			"broad-phase rejects reversed bounds");
		a_tests.Check(!profileQuery->MayIntersect(nonFinite),
			"broad-phase rejects non-finite bounds");

		const auto emptyQuery = ArmorCoverageQuery::Create(
			*layoutResult, std::span<const Triangle>{});
		a_tests.Check(emptyQuery && emptyQuery->Empty(),
			"an empty armor snapshot creates an empty reusable query");
		if (emptyQuery) {
			const auto emptySample = emptyQuery->Sample(firstBatch);
			a_tests.Check(emptySample && std::ranges::all_of(
				emptySample->Values(), [profile](FirstHiddenShell a_value) {
					return a_value == profile.shellCount;
				}), "an empty query leaves every sampled vertex visible");
			a_tests.Check(!emptyQuery->MayIntersect(overlapping),
				"an empty query cannot intersect valid bounds");
		}

		const ArmorCoverageQuery uninitialized;
		a_tests.Check(uninitialized.Empty() && uninitialized.ShellCount() == 0 &&
			Near(uninitialized.FurLength(), 0.0F) &&
			Near(uninitialized.UnderSkinDepth(), 0.0F),
			"a default query reports neutral metadata");
		a_tests.Check(!uninitialized.Sample(firstBatch),
			"an uninitialized query cannot sample vertices");
		a_tests.Check(!uninitialized.MayIntersect(overlapping),
			"an uninitialized query cannot intersect bounds");

		auto invalidTriangle = PlaneAt(0.2F);
		invalidTriangle.b.y = std::numeric_limits<float>::infinity();
		const std::array invalidArmor{ invalidTriangle };
		a_tests.Check(!ArmorCoverageQuery::Create(profile, invalidArmor),
			"query creation rejects non-finite armor triangles");

		auto invalidProfile = profile;
		invalidProfile.shellCount = kMaximumShellCount + 1;
		a_tests.Check(!ArmorCoverageQuery::Create(invalidProfile, armor),
			"query creation propagates profile validation errors");
	}

	void TestJsonStringArray(TestContext& a_tests)
	{
		const auto empty = ParseJsonStringArray(" [ ] \r\n");
		a_tests.Check(empty && empty->empty(), "an empty JSON blacklist is accepted");

		const auto values = ParseJsonStringArray(
			"[\"Cloaks.esp|0x1234\", \"Unicode \\u00E9\", \"quote\\\"slash\\\\\"]");
		a_tests.Check(values && values->size() == 3,
			"a JSON blacklist parses several strings");
		if (values && values->size() == 3) {
			a_tests.Check((*values)[0] == "Cloaks.esp|0x1234",
				"ordinary blacklist entries retain their bytes");
			a_tests.Check((*values)[1] == "Unicode \xC3\xA9",
				"Unicode escapes decode to UTF-8");
			a_tests.Check((*values)[2] == "quote\"slash\\",
				"JSON quote and slash escapes are decoded");
		}

		const auto bom = ParseJsonStringArray("\xEF\xBB\xBF[\"A.esm|0x1\"]");
		a_tests.Check(bom && bom->size() == 1, "a Windows UTF-8 BOM is accepted");
		a_tests.Check(!ParseJsonStringArray("{}"), "a JSON object is rejected by the simple list contract");
		a_tests.Check(!ParseJsonStringArray("[1]"), "non-string list entries are rejected");
		a_tests.Check(!ParseJsonStringArray("[\"unterminated]"), "unterminated strings are rejected");
		a_tests.Check(!ParseJsonStringArray("[\"bad\\q\"]"), "unknown escapes are rejected");
		a_tests.Check(!ParseJsonStringArray("[\"A\",]"), "trailing commas are rejected");
	}

	void TestCoverageSet(TestContext& a_tests)
	{
		CoverageSet coverage(2);
		a_tests.Check(coverage.Revision() == 0, "coverage starts at revision zero");
		a_tests.Check(coverage.Aggregate()[0] == kFullyVisibleCoverage &&
			coverage.Aggregate()[1] == kFullyVisibleCoverage,
			"default aggregate is fully visible");
		a_tests.Check(IsShellVisible(31, coverage.Aggregate()[0]),
			"neutral coverage keeps the maximum canonical shell visible");

		CoverageField first(2, 17);
		first.Values()[1] = 4;
		a_tests.Check(coverage.Set(10, std::move(first)), "first source is accepted");
		a_tests.Check(coverage.Revision() == 1, "Set advances revision");

		CoverageField second(2, 7);
		second.Values()[1] = 2;
		a_tests.Check(coverage.Set(20, std::move(second)), "second source is accepted");
		a_tests.Check(coverage.Aggregate()[0] == 7 && coverage.Aggregate()[1] == 2,
			"coverage sources aggregate with min");
		a_tests.Check(!IsShellVisible(2, coverage.Aggregate()[1]),
			"first-hidden-shell semantics hide the boundary shell");

		const auto revision = coverage.Revision();
		a_tests.Check(!coverage.Set(30, CoverageField(1, 0)), "wrong-sized source is rejected");
		a_tests.Check(!coverage.Remove(999), "missing source removal reports no change");
		a_tests.Check(coverage.Revision() == revision, "rejected operations do not change revision");

		a_tests.Check(coverage.Remove(20), "existing source is removed");
		a_tests.Check(coverage.Aggregate()[0] == 17 && coverage.Aggregate()[1] == 4,
			"removal rebuilds aggregate from remaining sources");
		coverage.Clear();
		a_tests.Check(coverage.Aggregate()[0] == kFullyVisibleCoverage &&
			coverage.Aggregate()[1] == kFullyVisibleCoverage,
			"Clear restores fully visible neutral coverage");
		const auto clearedRevision = coverage.Revision();
		coverage.Clear();
		a_tests.Check(coverage.Revision() == clearedRevision, "clearing an empty set is not a revision");
	}

	void TestCoverageStress(TestContext& a_tests)
	{
		const auto layoutResult = ShellLayout::Create(ShellProfile::Legacy17());
		if (!layoutResult) {
			a_tests.Check(false, "stress test requires a valid layout");
			return;
		}

		std::vector<Triangle> triangles;
		triangles.reserve(50'000);
		triangles.push_back(PlaneAt(0.2F));
		for (std::size_t index = 1; index < 50'000; ++index) {
			const auto x = 100.0F + static_cast<float>(index % 250) * 2.0F;
			const auto y = 100.0F + static_cast<float>(index / 250) * 2.0F;
			triangles.push_back({
				{ x, y, 0.3F }, { x + 0.5F, y, 0.3F }, { x, y + 0.5F, 0.3F }
			});
		}
		const std::vector<CarrierVertex> vertices(
			20'000, CarrierVertex{ { 0.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } });

		const auto result = SolveArmorCoverage(*layoutResult, vertices, triangles);
		a_tests.Check(result.has_value(), "20k/50k synthetic coverage solve succeeds");
		if (result) {
			a_tests.Check(std::all_of(
				result->Values().begin(), result->Values().end(),
				[](FirstHiddenShell a_value) { return a_value == 4; }),
				"stress solve preserves deterministic coverage");
		}
	}
}

int main()
{
	TestContext tests;
	TestProfileValidation(tests);
	TestShellLayout(tests);
	TestLod(tests);
	TestCoverageGeometry(tests);
	TestReusableCoverageQuery(tests);
	TestJsonStringArray(tests);
	TestCoverageSet(tests);
	TestCoverageStress(tests);

	if (tests.failures == 0) {
		std::cout << "DynamicFur Core tests passed\n";
		return 0;
	}
	std::cerr << tests.failures << " DynamicFur Core test(s) failed\n";
	return 1;
}
