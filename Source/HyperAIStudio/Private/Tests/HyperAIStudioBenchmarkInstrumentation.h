// Games by Hyper 2026.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::BenchmarkInstrumentation
{
	/** Installs the opt-in benchmark proxies when -HyperAIBenchmarkInstrumentation is present. */
	void Startup();

	/** Removes pending work and restores every proxied ToolsetRegistry handler exactly. */
	void Shutdown();
}

#endif // WITH_DEV_AUTOMATION_TESTS
