#include "PCH.h"

#include "DynamicFur/Controller.h"
#include "DynamicFur/Log.h"

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message) noexcept
	{
		if (!a_message) {
			return;
		}

		try {
			auto& controller = DynamicFur::Controller::GetSingleton();
			switch (a_message->type) {
			case SKSE::MessagingInterface::kDataLoaded:
				controller.Initialize();
				break;
			case SKSE::MessagingInterface::kPreLoadGame:
				controller.OnPreLoadGame();
				break;
			case SKSE::MessagingInterface::kPostLoadGame:
			case SKSE::MessagingInterface::kNewGame:
				controller.OnGameReady();
				break;
			default:
				break;
			}
		} catch (const std::exception& error) {
			SKSE::log::critical("Dynamic Fur message handling failed: {}", error.what());
		} catch (...) {
			SKSE::log::critical("Dynamic Fur message handling failed with an unknown exception");
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	DynamicFur::Log::Initialize();
	SKSE::Init(a_skse);

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		SKSE::log::critical("Could not register the SKSE messaging listener");
		return false;
	}

	SKSE::log::info("Dynamic Fur {} loaded; waiting for game data", DYNAMICFUR_VERSION_STRING);
	return true;
}
