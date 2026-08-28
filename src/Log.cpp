#include "PCH.h"

#include "DynamicFur/Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace DynamicFur::Log
{
	void Initialize()
	{
		const auto logDirectory = SKSE::log::log_directory();
		if (!logDirectory) {
			SKSE::stl::report_and_fail("SKSE did not provide a log directory");
		}

		const auto logPath = *logDirectory / (std::string{ DYNAMICFUR_PROJECT_NAME } + ".log");
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
		auto logger = std::make_shared<spdlog::logger>("DynamicFur", std::move(sink));

		spdlog::set_default_logger(std::move(logger));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		spdlog::set_level(spdlog::level::info);
		spdlog::flush_on(spdlog::level::info);
	}
}
