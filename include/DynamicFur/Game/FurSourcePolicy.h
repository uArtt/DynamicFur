#pragma once

#include <cstdint>
#include <string>

namespace RE
{
	class Actor;
	class BGSKeyword;
	class TESObjectARMO;
}

namespace DynamicFur::Game
{
	struct FurSourceKeywordConfig
	{
		std::string pluginName{ "TrueShell.esp" };
		std::uint32_t localFormID{ 0x800U };
	};

	enum class FurSourceStatus : std::uint8_t
	{
		kEligible,
		kKeywordUnresolved,
		kActorUnavailable,
		kSkinUnavailable,
		kSkinUnmarked
	};

	struct FurSourceEvaluation
	{
		FurSourceStatus status{ FurSourceStatus::kKeywordUnresolved };
		RE::TESObjectARMO* skin{};
		std::uint32_t skinFormID{};
		std::uint32_t markedArmorCount{};
		bool skinMarked{};

		[[nodiscard]] bool IsEligible() const noexcept
		{
			return status == FurSourceStatus::kEligible;
		}
	};

	// Resolves one game-native keyword after DataLoaded and evaluates the
	// actor's effective Skin plus active editor-slot ARMO records. The policy
	// deliberately knows nothing about meshes or rendering.
	class FurSourcePolicy final
	{
	public:
		explicit FurSourcePolicy(FurSourceKeywordConfig a_config = {});

		[[nodiscard]] bool Resolve() noexcept;
		[[nodiscard]] FurSourceEvaluation Evaluate(RE::Actor* a_actor) const noexcept;
		[[nodiscard]] bool IsResolved() const noexcept;
		[[nodiscard]] RE::BGSKeyword* Keyword() const noexcept;
		[[nodiscard]] const FurSourceKeywordConfig& Config() const noexcept;

	private:
		FurSourceKeywordConfig config_;
		RE::BGSKeyword* keyword_{};
	};
}
