#include "loadscreen.h"
#include "state.h"

#include <numeric>

// Counts the engine's loading work with small code patches on the instructions that change its queue counters.
// The counter sites, registers and runtime offsets come from Skyrim Load Progress by ahzaab (GPL-3.0), checked there
// against 1.5.97, 1.6.1170, 1.7.99 and 1.7.104. Each site's bytes are compared before it is patched, a mismatch leaves it alone.

namespace
{
	std::atomic<std::uint64_t> queued[LoadScreen::kQueues]{};
	std::atomic<std::uint64_t> finished[LoadScreen::kQueues]{};
	bool                       counted[LoadScreen::kQueues]{};
	std::atomic<std::uintptr_t> lastRef{ 0 };   // the reference queued last, written by the patched calls
	std::atomic<std::uintptr_t> lastCell{ 0 };  // the cell it was queued for

	std::mutex    lock;
	std::atomic<bool> active{ false };
	std::uint32_t loads = 0;
	double        startedAt = 0;
	double        endedAt = 0;
	std::string   why;
	std::uint64_t startQueued[LoadScreen::kQueues]{};
	std::uint64_t startDone[LoadScreen::kQueues]{};
	std::uint64_t waiting[LoadScreen::kQueues]{};  // queued and not done when the load started
	std::uint64_t endQueued[LoadScreen::kQueues]{};
	std::uint64_t endDone[LoadScreen::kQueues]{};

	std::atomic<HANDLE>        save{ nullptr };
	std::atomic<std::uint64_t> saveRead{ 0 };
	std::atomic<std::uint64_t> saveSize{ 0 };

	// Machine code for the stubs, little endian immediates.
	struct Code
	{
		std::vector<std::uint8_t> b;

		Code& operator()(std::initializer_list<std::uint8_t> a_bytes)
		{
			b.insert(b.end(), a_bytes);
			return *this;
		}
		Code& Q(std::uint64_t a_value)
		{
			const auto* p = reinterpret_cast<const std::uint8_t*>(&a_value);
			b.insert(b.end(), p, p + 8);
			return *this;
		}
		Code& Q(const void* a_p) { return Q(reinterpret_cast<std::uint64_t>(a_p)); }
		// jmp qword ptr [rip+0] to a_to
		Code& JumpTo(std::uintptr_t a_to) { return (*this)({ 0xFF, 0x25, 0, 0, 0, 0 }).Q(a_to); }
	};

	std::uintptr_t Place(const Code& a_code)
	{
		auto* cave = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(a_code.b.size()));
		std::memcpy(cave, a_code.b.data(), a_code.b.size());
		return reinterpret_cast<std::uintptr_t>(cave);
	}

	// Jumps from a_at to the stub, the rest of a patched instruction becomes NOPs (the stub resumes after it).
	void Branch(std::uintptr_t a_at, std::size_t a_size, std::uintptr_t a_stub)
	{
		static_cast<void>(SKSE::GetTrampoline().write_branch<5>(a_at, a_stub, true));
		if (a_size > 5) {
			const std::vector<std::uint8_t> nops(a_size - 5, 0x90);
			REL::safe_write(a_at + 5, nops.data(), nops.size());
		}
	}

	// A lock inc or lock dec of an engine counter, replayed in the stub before it counts it.
	struct CounterSite
	{
		REL::RelocationID          function;
		std::ptrdiff_t             offset;
		std::vector<std::uint8_t> bytes;
		LoadScreen::Queue          queue;
		bool                       enqueue;
	};

	bool Matches(std::uintptr_t a_at, const std::vector<std::uint8_t>& a_bytes)
	{
		return a_at && std::memcmp(reinterpret_cast<const void*>(a_at), a_bytes.data(), a_bytes.size()) == 0;
	}

	// The original instruction, then pushfq, push rax, mov rax counter, lock inc qword [rax], pop rax, popfq, jmp back.
	void HookCounter(std::uintptr_t a_at, const CounterSite& a_site)
	{
		auto* counter = a_site.enqueue ? &queued[a_site.queue] : &finished[a_site.queue];
		Code  c;
		c.b = a_site.bytes;
		c({ 0x9C, 0x50, 0x48, 0xB8 }).Q(counter)({ 0xF0, 0x48, 0xFF, 0x00, 0x58, 0x9D }).JumpTo(a_at + a_site.bytes.size());
		Branch(a_at, a_site.bytes.size(), Place(c));
	}

	void HookCounters(bool a_se)
	{
		using Q = LoadScreen::Queue;
		std::vector<CounterSite> sites = {
			{ REL::RelocationID(18675, 19155), 0x07, { 0xF0, 0xFF, 0x80, 0x6C, 0x01, 0x00, 0x00 }, LoadScreen::kCritical, true },
			{ REL::RelocationID(18676, 19156), 0x0C, { 0xF0, 0xFF, 0x88, 0x6C, 0x01, 0x00, 0x00 }, LoadScreen::kCritical, false },
			{ REL::RelocationID(18672, 19151), 0x07, { 0xF0, 0xFF, 0x80, 0x70, 0x01, 0x00, 0x00 }, LoadScreen::kRefs, true },
			{ REL::RelocationID(18673, 19152), 0x0C, { 0xF0, 0xFF, 0x88, 0x70, 0x01, 0x00, 0x00 }, LoadScreen::kRefs, false },
			{ REL::RelocationID(18677, 19159), 0x4E, { 0xF0, 0xFF, 0x81, 0x74, 0x01, 0x00, 0x00 }, LoadScreen::kDistant, true },
			{ REL::RelocationID(18678, 19160), 0x69, { 0xF0, 0xFF, 0x88, 0x74, 0x01, 0x00, 0x00 }, LoadScreen::kDistant, false },
		};
		// On SE the background counter instruction is 4 bytes, too short for a jump without decoding the next one.
		if (!a_se) {
			sites.push_back({ REL::RelocationID(12902, 13049), 0xA0, { 0xF0, 0x41, 0xFF, 0x47, 0x68 }, LoadScreen::kBackground, true });
			sites.push_back({ REL::RelocationID(12902, 13049), 0x363, { 0xF0, 0x41, 0xFF, 0x4F, 0x68 }, LoadScreen::kBackground, false });
		}
		// A queue counts only when both its sites match, the other queues still count.
		std::uintptr_t at[16]{};
		bool           ok[LoadScreen::kQueues]{};
		std::ranges::fill(ok, true);
		bool listed[LoadScreen::kQueues]{};
		for (std::size_t i = 0; i < sites.size(); ++i) {
			const auto& s = sites[i];
			const auto  function = REL::Relocation<std::uintptr_t>(s.function).address();
			at[i] = function ? function + s.offset : 0;
			listed[s.queue] = true;
			if (!Matches(at[i], s.bytes)) {
				ok[s.queue] = false;
				logger::warn("Load queue {} {} site doesn't match this runtime, left uncounted", LoadScreen::queueNames[s.queue],
					s.enqueue ? "enqueue" : "completion");
			}
		}
		for (std::size_t i = 0; i < sites.size(); ++i) {
			if (ok[sites[i].queue]) {
				HookCounter(at[i], sites[i]);
			}
		}
		for (std::size_t q = 0; q < static_cast<std::size_t>(Q::kTasks); ++q) {
			counted[q] = listed[q] && ok[q];
		}
	}

	// Four-byte counter methods ending in ret, too short to patch, so their vtable slots are replaced.
	using Mutate_t = void (*)(void*);
	Mutate_t ioQueued = nullptr;
	Mutate_t ioDone = nullptr;
	Mutate_t postQueued = nullptr;
	Mutate_t postDone = nullptr;

	void IOQueued(void* a_manager)
	{
		ioQueued(a_manager);
		queued[LoadScreen::kTasks].fetch_add(1, std::memory_order_relaxed);
	}
	void IODone(void* a_manager)
	{
		ioDone(a_manager);
		finished[LoadScreen::kTasks].fetch_add(1, std::memory_order_relaxed);
	}

	// The queue type is shared, only IOManager's post-processing queue (its pointer at +0xE0) counts.
	bool IsPostQueue(void* a_queue)
	{
		const auto manager = reinterpret_cast<std::uintptr_t>(RE::IOManager::GetSingleton());
		return manager && *reinterpret_cast<void**>(manager + 0xE0) == a_queue;
	}
	void PostQueued(void* a_queue)
	{
		postQueued(a_queue);
		if (IsPostQueue(a_queue)) {
			queued[LoadScreen::kPostProcessing].fetch_add(1, std::memory_order_relaxed);
		}
	}
	void PostDone(void* a_queue)
	{
		postDone(a_queue);
		if (IsPostQueue(a_queue)) {
			finished[LoadScreen::kPostProcessing].fetch_add(1, std::memory_order_relaxed);
		}
	}

	std::uintptr_t Slot(std::uintptr_t a_vtable, std::size_t a_index) { return *reinterpret_cast<std::uintptr_t*>(a_vtable + a_index * 8); }

	void HookTaskQueues()
	{
		// lock inc/dec dword [rcx+0x30], ret
		REL::Relocation<std::uintptr_t> io{ RE::VTABLE_IOManager[0] };
		if (Matches(Slot(io.address(), 14), { 0xF0, 0xFF, 0x41, 0x30, 0xC3 }) && Matches(Slot(io.address(), 15), { 0xF0, 0xFF, 0x49, 0x30, 0xC3 })) {
			ioQueued = reinterpret_cast<Mutate_t>(io.write_vfunc(14, &IOQueued));
			ioDone = reinterpret_cast<Mutate_t>(io.write_vfunc(15, &IODone));
			counted[LoadScreen::kTasks] = true;
		} else {
			logger::warn("Load queue tasks: IOManager counter methods don't match this runtime, left uncounted");
		}
		// lock inc/dec dword [rcx+0x14], ret, and the count getter mov eax [rcx+0x14], ret
		REL::Relocation<std::uintptr_t> post{ RE::VTABLE_SynchronizedPriorityQueue_NiPointer_IOTask__[0] };
		const auto enqueue = REL::Relocation<std::uintptr_t>(REL::RelocationID(13095, 13241)).address();
		const auto complete = REL::Relocation<std::uintptr_t>(REL::RelocationID(13073, 13222)).address();
		const auto count = REL::Relocation<std::uintptr_t>(REL::RelocationID(13089, 13235)).address();
		if (Slot(post.address(), 1) == enqueue && Slot(post.address(), 2) == complete && Slot(post.address(), 3) == count &&
			Matches(enqueue, { 0xF0, 0xFF, 0x41, 0x14, 0xC3 }) && Matches(complete, { 0xF0, 0xFF, 0x49, 0x14, 0xC3 }) &&
			Matches(count, { 0x8B, 0x41, 0x14, 0xC3 })) {
			postQueued = reinterpret_cast<Mutate_t>(post.write_vfunc(1, &PostQueued));
			postDone = reinterpret_cast<Mutate_t>(post.write_vfunc(2, &PostDone));
			counted[LoadScreen::kPostProcessing] = true;
		} else {
			logger::warn("Load queue post-processing: queue methods don't match this runtime, left uncounted");
		}
	}

	// The one call from a_caller to a_callee, found by its target inside the caller's bounds from the unwind table.
	std::uintptr_t FindCall(REL::RelocationID a_caller, REL::RelocationID a_callee)
	{
		const auto caller = REL::Relocation<std::uintptr_t>(a_caller).address();
		const auto callee = REL::Relocation<std::uintptr_t>(a_callee).address();
		DWORD64    image = 0;
		const auto* entry = caller && callee ? RtlLookupFunctionEntry(caller, &image, nullptr) : nullptr;
		if (!entry || image + entry->BeginAddress != caller) {
			return 0;
		}
		std::uintptr_t found = 0;
		for (auto p = caller; p + 5 <= image + entry->EndAddress; ++p) {
			std::int32_t rel;
			std::memcpy(&rel, reinterpret_cast<const void*>(p + 1), 4);
			if (*reinterpret_cast<const std::uint8_t*>(p) == 0xE8 && p + 5 + rel == callee) {
				if (found) {
					return 0;  // more than one, can't tell which
				}
				found = p;
			}
		}
		return found;
	}

	// Replaces a call that queues a reference: stores the reference and the cell (rcx), then makes the call.
	// push rax, mov rax lastRef, mov [rax] ref, mov rax lastCell, mov [rax] rcx, pop rax, call [callee], jmp back
	void HookRefCall(REL::RelocationID a_caller, REL::RelocationID a_callee, std::uint8_t a_storeModrm, std::string_view a_name)
	{
		const auto at = FindCall(a_caller, a_callee);
		if (!at) {
			logger::warn("Load status: the {} call wasn't found on this runtime, those names aren't shown", a_name);
			return;
		}
		const auto callee = REL::Relocation<std::uintptr_t>(a_callee).address();
		Code       c;
		c({ 0x50, 0x48, 0xB8 }).Q(&lastRef)({ 0x48, 0x89, a_storeModrm, 0x48, 0xB8 }).Q(&lastCell)({ 0x48, 0x89, 0x08, 0x58 });
		c({ 0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08 }).Q(callee).JumpTo(at + 5);
		Branch(at, 5, Place(c));
	}

	void Copy(char* a_to, std::size_t a_size, const char* a_from)
	{
		std::size_t i = 0;
		for (; a_from && a_from[i] && i + 1 < a_size; ++i) {
			a_to[i] = a_from[i];
		}
		a_to[i] = 0;
	}

	// Through the kernel, which fails on unmapped memory. clang-cl's __try only guards calls, not a plain read.
	bool ReadFormID(std::uintptr_t a_form, RE::FormID& a_id)
	{
		SIZE_T read = 0;
		return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a_form + 0x14), &a_id, sizeof(a_id), &read) &&
		       read == sizeof(a_id);
	}

	// The form at a_p if it is still the live form with its own ID, the reference may be gone by now.
	RE::TESForm* Live(std::uintptr_t a_p)
	{
		RE::FormID id = 0;
		if (!a_p || !ReadFormID(a_p, id) || !id) {
			return nullptr;
		}
		const auto form = RE::TESForm::LookupByID(id);
		return reinterpret_cast<std::uintptr_t>(form) == a_p ? form : nullptr;
	}

	struct Names
	{
		char         cell[128];
		char         item[128];
		char         plugin[128];
		RE::FormType type;
	};

	// Reads the names under SEH, the forms are live but another thread may be changing them.
	bool ReadNames(RE::TESForm* a_ref, RE::TESForm* a_cell, Names& a_out)
	{
		__try {
			if (const auto ref = a_ref ? a_ref->AsReference() : nullptr) {
				if (const auto base = ref->GetBaseObject()) {
					a_out.type = base->GetFormType();
					const auto name = base->GetName();
					Copy(a_out.item, sizeof(a_out.item), name && *name ? name : "");
					if (const auto file = base->GetFile(0)) {
						Copy(a_out.plugin, sizeof(a_out.plugin), file->GetFilename().data());
					}
				}
			}
			if (const auto cell = a_cell ? a_cell->As<RE::TESObjectCELL>() : nullptr) {
				const char* name = cell->GetFullName();
				if ((!name || !*name) && !cell->IsInteriorCell()) {
					if (const auto world = cell->GetRuntimeData().worldSpace) {
						name = world->GetFullName();
					}
				}
				Copy(a_out.cell, sizeof(a_out.cell), name);
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// What an unnamed base object is, most of what a cell queues has no name.
	std::string_view Kind(RE::FormType a_type)
	{
		using T = RE::FormType;
		switch (a_type) {
		case T::Tree:
		case T::Flora:
		case T::Grass:
			return "plants";
		case T::Light:
			return "lights";
		case T::Activator:
		case T::TalkingActivator:
			return "activators";
		case T::Furniture:
			return "furniture";
		case T::Container:
			return "containers";
		case T::Door:
			return "doors";
		case T::NPC:
			return "actors";
		case T::Sound:
			return "sounds";
		default:
			return "objects";
		}
	}

	std::string Counts(const std::uint64_t (&a_done)[LoadScreen::kQueues], const std::uint64_t (&a_total)[LoadScreen::kQueues])
	{
		std::string s;
		for (std::size_t q = 0; q < LoadScreen::kQueues; ++q) {
			if (counted[q]) {
				s += std::format("{}{} {}/{}", s.empty() ? "" : ", ", LoadScreen::queueNames[q], a_done[q], a_total[q]);
			}
		}
		return s;
	}
}

namespace LoadScreen
{
	void Install()
	{
		if (REL::Module::IsVR()) {
			logger::info("Loading screen progress isn't available on VR");
			return;
		}
		SKSE::AllocTrampoline(1 << 12);
		const bool se = REL::Module::get().version() < REL::Version(1, 6, 0, 0);
		HookCounters(se);
		HookTaskQueues();
		HookRefCall(REL::RelocationID(12910, 13057), REL::RelocationID(18672, 19151), 0x38, "object reference");  // mov [rax], rdi
		HookRefCall(REL::RelocationID(19391, 19818), REL::RelocationID(18672, 19151), 0x18, "moved reference");   // mov [rax], rbx
		HookRefCall(REL::RelocationID(17811, 18223), REL::RelocationID(18677, 19159), 0x10, "distant reference"); // mov [rax], rdx
		std::string list;
		for (std::size_t q = 0; q < kQueues; ++q) {
			list += std::format("{}{} {}", list.empty() ? "" : ", ", queueNames[q], counted[q] ? "yes" : "no");
		}
		logger::info("[{:6.1f} s] Loading screen progress hooked, queues counted: {} ({} trampoline bytes left)", Status::Uptime(),
			list, SKSE::GetTrampoline().free_size());
	}

	void Begin(std::string_view a_why)
	{
		std::scoped_lock l{ lock };
		if (active) {
			return;
		}
		++loads;
		why = a_why;
		startedAt = Status::Uptime();
		endedAt = 0;
		std::uint64_t total = 0;
		for (std::size_t q = 0; q < kQueues; ++q) {
			startDone[q] = finished[q].load();
			startQueued[q] = queued[q].load();
			waiting[q] = startQueued[q] > startDone[q] ? startQueued[q] - startDone[q] : 0;
			total += waiting[q];
		}
		save = nullptr;
		saveRead = 0;
		saveSize = 0;
		lastRef = 0;
		lastCell = 0;
		active = true;
		std::uint64_t none[kQueues]{};
		logger::info("[{:6.1f} s] Load {} started ({}), waiting at start: {}", startedAt, loads, why, total ? Counts(none, waiting) : "nothing"s);
	}

	void End(std::string_view a_why)
	{
		std::scoped_lock l{ lock };
		if (!active) {
			return;
		}
		endedAt = Status::Uptime();
		for (std::size_t q = 0; q < kQueues; ++q) {
			endDone[q] = finished[q].load();
			endQueued[q] = queued[q].load();
		}
		active = false;
		save = nullptr;
		logger::info("[{:6.1f} s] Load {} ended ({}) after {:.1f} s", endedAt, loads, a_why, endedAt - startedAt);
	}

	bool Active() { return active.load(std::memory_order_relaxed); }

	void OnSaveOpened(HANDLE a_handle)
	{
		LARGE_INTEGER size{};
		if (!active || !GetFileSizeEx(a_handle, &size)) {
			return;
		}
		saveRead = 0;
		saveSize = static_cast<std::uint64_t>(size.QuadPart);
		save = a_handle;
	}

	void OnRead(HANDLE a_handle, DWORD a_bytes)
	{
		if (a_handle && a_handle == save.load(std::memory_order_relaxed)) {
			saveRead.fetch_add(a_bytes, std::memory_order_relaxed);
		}
	}

	std::uint64_t Snapshot::Done() const { return std::accumulate(std::begin(done), std::end(done), std::uint64_t{ 0 }); }
	std::uint64_t Snapshot::Total() const { return std::accumulate(std::begin(total), std::end(total), std::uint64_t{ 0 }); }

	double Snapshot::Progress() const
	{
		double sum = 0;
		int    steps = 0;
		for (std::size_t q = 0; q < kQueues; ++q) {
			auto d = done[q];
			auto t = total[q];
			if (q == kRefs && counted[kCritical]) {
				d -= std::min(d, done[kCritical]);
				t -= std::min(t, total[kCritical]);
			}
			if (t) {
				sum += double(std::min(d, t)) / t;
				++steps;
			}
		}
		return steps ? sum / steps : 0.0;
	}

	Snapshot Take()
	{
		Snapshot s;
		{
			std::scoped_lock l{ lock };
			s.id = loads;
			s.active = active;
			s.startedAt = startedAt;
			s.endedAt = endedAt;
			s.why = why;
			for (std::size_t q = 0; q < kQueues; ++q) {
				const auto nowQueued = s.active ? queued[q].load() : endQueued[q];
				const auto nowDone = s.active ? finished[q].load() : endDone[q];
				s.counted[q] = counted[q];
				s.total[q] = waiting[q] + (nowQueued - startQueued[q]);
				s.done[q] = std::min(nowDone - startDone[q], s.total[q]);
			}
		}
		s.saveRead = saveRead;
		s.saveSize = saveSize;
		Names n{};
		if (s.active && ReadNames(Live(lastRef), Live(lastCell), n)) {
			s.cell = n.cell;
			s.item = *n.item ? std::string(n.item) : *n.plugin ? std::string(Kind(n.type)) : ""s;
			s.plugin = n.plugin;
		}
		return s;
	}
}
