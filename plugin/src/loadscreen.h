#pragma once

// Progress of in-game loading screens (saves, doors, fast travel) from the engine's own load queue counters.
// The queues and their counter sites follow Skyrim Load Progress by ahzaab (GPL-3.0), github.com/ahzaab/sky-load-progress
namespace LoadScreen
{
	// The engine's loading work, as its own loading diagnostic counts it.
	enum Queue : std::size_t
	{
		kCritical,
		kRefs,
		kDistant,
		kBackground,
		kTasks,
		kPostProcessing,
		kQueues
	};
	inline constexpr std::string_view queueNames[kQueues] = { "critical refs", "refs", "distant refs", "background", "tasks",
		"post-processing" };

	// Hooks the queue counters and the calls that queue references. Call once from SKSEPlugin_Load.
	void Install();

	// A load started (save load, loading screen) or ended. Begin while one is going on does nothing.
	void Begin(std::string_view a_why);
	void End(std::string_view a_why);
	bool Active();

	// File hooks: a save file opened during a load, and bytes read from any handle.
	void OnSaveOpened(HANDLE a_handle);
	void OnRead(HANDLE a_handle, DWORD a_bytes);

	struct Snapshot
	{
		std::uint32_t id = 0;  // counts loads, 0 before the first
		bool          active = false;
		double        startedAt = 0;  // seconds since process start
		double        endedAt = 0;
		std::string   why;
		bool          counted[kQueues]{};  // the queue's counters are hooked on this runtime
		std::uint64_t done[kQueues]{};
		std::uint64_t total[kQueues]{};  // waiting at the start plus queued since
		std::uint64_t saveRead = 0;
		std::uint64_t saveSize = 0;
		std::string   cell;    // the cell the last reference was queued for
		std::string   item;    // its base object's name, or what kind of object it is
		std::string   plugin;  // the plugin that added the base object

		std::uint64_t Done() const;
		std::uint64_t Total() const;
		// 0..1, each queue with work is an equal step. Refs include the critical ones, so the rest of them are their own step.
		double Progress() const;
	};
	// Counts are frozen at End. Names are looked up here, so only call it from the overlay thread.
	Snapshot Take();
}
