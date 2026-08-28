#include "DynamicFur/Game/FurSourcePolicy.h"

#include "RE/A/Actor.h"
#include "RE/B/BGSKeyword.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESObjectARMO.h"

#include <unordered_set>
#include <utility>

namespace DynamicFur::Game
{
	FurSourcePolicy::FurSourcePolicy(FurSourceKeywordConfig a_config) :
		config_(std::move(a_config))
	{}

	bool FurSourcePolicy::Resolve() noexcept
	{
		keyword_ = nullptr;
		if (config_.pluginName.empty() || config_.localFormID == 0) {
			return false;
		}

		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			return false;
		}

		keyword_ = dataHandler->LookupForm<RE::BGSKeyword>(
			config_.localFormID, config_.pluginName);
		return keyword_ != nullptr;
	}

	FurSourceEvaluation FurSourcePolicy::Evaluate(RE::Actor* a_actor) const noexcept
	{
		if (!keyword_) {
			return { FurSourceStatus::kKeywordUnresolved };
		}
		if (!a_actor) {
			return { FurSourceStatus::kActorUnavailable };
		}

		auto* skin = a_actor->GetSkin();
		const auto skinMarked = skin && skin->HasKeyword(keyword_);
		std::unordered_set<RE::TESObjectARMO*> markedArmors;
		if (skinMarked) {
			markedArmors.insert(skin);
		}

		const auto& bipedPointer = a_actor->GetBiped(false);
		auto* biped = bipedPointer.get();
		if (biped) {
			for (std::uint32_t slot = 0; slot < RE::BIPED_OBJECTS::kEditorTotal; ++slot) {
				auto* item = biped->objects[slot].item;
				auto* armor = item ? item->As<RE::TESObjectARMO>() : nullptr;
				if (armor && armor->HasKeyword(keyword_)) {
					markedArmors.insert(armor);
				}
			}
		}

		const auto status = !markedArmors.empty() ?
			FurSourceStatus::kEligible :
			(skin ? FurSourceStatus::kSkinUnmarked : FurSourceStatus::kSkinUnavailable);
		return {
			status,
			skin,
			skin ? skin->GetFormID() : 0,
			static_cast<std::uint32_t>(markedArmors.size()),
			skinMarked
		};
	}

	bool FurSourcePolicy::IsResolved() const noexcept { return keyword_ != nullptr; }
	RE::BGSKeyword* FurSourcePolicy::Keyword() const noexcept { return keyword_; }
	const FurSourceKeywordConfig& FurSourcePolicy::Config() const noexcept { return config_; }
}
