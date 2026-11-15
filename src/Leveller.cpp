#include "PCH.h"

#include "Duck.h"
#include "Leveller.h"
#include "Settings.h"

#include <RE/X/XAPO.h>

#include <chrono>
#include <format>
#include <mutex>
#include <xmmintrin.h>

namespace
{
	constexpr GUID          kIUnknown{ 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
	constexpr GUID          kIXAPO{ 0xA90BC001, 0xE897, 0xE897, { 0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00 } };
	constexpr std::int32_t  kOk = 0;
	constexpr std::int32_t  kNoInterface = static_cast<std::int32_t>(0x80004002);
	constexpr std::int32_t  kFormatUnsupported = static_cast<std::int32_t>(0x88970001);
	constexpr std::uint32_t kFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x10;

	constexpr std::size_t   kPlayImpl = 0x15;
	constexpr std::size_t   kUpdate = 0x0E;
	constexpr int           kLogLines = 500;
	constexpr std::uint64_t kSummaryMs = 60000;
	constexpr float         kDuckedGain = 0.944f;

	struct Key
	{
		std::uint32_t file{ 0 };
		std::uint32_t extension{ 0 };
		std::uint32_t folder{ 0 };

		bool operator==(const Key&) const = default;
	};
	static_assert(sizeof(RE::BSResource::ID) == sizeof(Key));

	Settings::Group GroupOf(std::uint32_t a_form, bool& a_known)
	{
		a_known = true;
		switch (a_form) {
		case 0x0876BB:
			return Settings::kVoice;
		case 0x0876BC:
		case 0x0876BD:
			return Settings::kBackground;
		case 0x07F80B:
		case 0x071E63:
			return Settings::kOther;
		case 0x071E64:
			return Settings::kMusic;
		case 0x0F5FFC:
		case 0x000F72:
		case 0x064451:
		case 0x0172A1:
		case 0x000E9E:
		case 0x01A0BD:
		case 0x09F254:
		case 0x0DDDC5:
		case 0x0EA51F:
		case 0x0F23BB:
			return Settings::kOther;
		default:
			a_known = false;
			return Settings::kOther;
		}
	}

	class Tap;

	std::mutex                                                       lock;
	std::unordered_map<const RE::BSISoundCategory*, Settings::Group> groups;
	std::unordered_set<const RE::BSISoundCategory*>                  records;
	std::unordered_map<RE::IXAudio2SourceVoice*, Tap*>               taps;
	const RE::BSISoundCategory*                                      engineCategory{ nullptr };

	std::atomic<bool>          dialogueOpen{ false };
	std::atomic<std::uint64_t> menuState{ 0 };

	std::int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	std::atomic<std::uint32_t> failures{ 0 };
	std::atomic<int>           heardLines{ kLogLines };

	struct Tally
	{
		std::atomic<std::uint64_t> frames{ 0 };
		std::atomic<std::uint64_t> ducked{ 0 };
		std::atomic<std::uint64_t> depth{ 0 };
	};
	Tally                      musicTally;
	std::atomic<std::uint32_t> lines{ 0 };
	std::atomic<std::uint32_t> backgroundLines{ 0 };
	std::atomic<std::uint64_t> lastSummary{ 0 };

	Settings::Group GroupOf(const RE::BSISoundCategory* a_category)
	{
		if (a_category && !records.contains(a_category)) {
			if (!engineCategory) {
				engineCategory = a_category;
				if (Settings::GetSingleton().log) {
					logger::info("category object {:X} is not a record: such objects are treated as background voices",
						*reinterpret_cast<const std::uintptr_t*>(a_category) - REL::Module::get().base());
				}
			}
			return Settings::kBackground;
		}
		const auto found = groups.find(a_category);
		if (found != groups.end()) {
			return found->second;
		}
		Settings::Group group = Settings::kOther;
		if (const auto* form = static_cast<const RE::BGSSoundCategory*>(a_category)) {
			bool known = false;
			group = GroupOf(form->GetFormID(), known);
			for (const auto* parent = form->parentCategory; parent && !known; parent = parent->parentCategory) {
				group = GroupOf(parent->GetFormID(), known);
			}
		}
		groups.emplace(a_category, group);
		return group;
	}

	struct Plan
	{
		Key    key;
		float  duckDb{ 0.0f };
		Tally* tally{ nullptr };
	};

	class Tap final : public RE::IXAPO
	{
	public:
		Tap(const Plan& a_plan, RE::IXAudio2SourceVoice* a_voice) :
			plan(a_plan), voice(a_voice)
		{}

		std::int32_t QueryInterface(const REX::W32::GUID& a_iid, void** a_out) override
		{
			if (!a_out) {
				return kNoInterface;
			}
			if (std::memcmp(&a_iid, &kIUnknown, sizeof(GUID)) == 0 || std::memcmp(&a_iid, &kIXAPO, sizeof(GUID)) == 0) {
				*a_out = this;
				AddRef();
				return kOk;
			}
			*a_out = nullptr;
			return kNoInterface;
		}

		std::uint32_t AddRef() override { return ++references; }

		std::uint32_t Release() override
		{
			const std::uint32_t left = --references;
			if (left == 0) {
				Finish();
				delete this;
			}
			return left;
		}

		std::int32_t GetRegistrationProperties(RE::XAPO_REGISTRATION_PROPERTIES** a_properties) override
		{
			auto* properties = static_cast<RE::XAPO_REGISTRATION_PROPERTIES*>(CoTaskMemAlloc(sizeof(RE::XAPO_REGISTRATION_PROPERTIES)));
			if (!properties) {
				return static_cast<std::int32_t>(0x8007000E);
			}
			std::memset(properties, 0, sizeof(*properties));
			wcscpy_s(properties->friendlyName, L"Dialogue Ducker");
			properties->majorVersion = 1;
			properties->flags = kFlags;
			properties->minInputBufferCount = properties->maxInputBufferCount = 1;
			properties->minOutputBufferCount = properties->maxOutputBufferCount = 1;
			*a_properties = properties;
			return kOk;
		}

		std::int32_t IsInputFormatSupported(const RE::WAVEFORMATEX*, const RE::WAVEFORMATEX* a_requested, RE::WAVEFORMATEX** a_supported) override
		{
			if (a_supported) {
				*a_supported = nullptr;
			}
			return IsFloat(a_requested) ? kOk : kFormatUnsupported;
		}

		std::int32_t IsOutputFormatSupported(const RE::WAVEFORMATEX*, const RE::WAVEFORMATEX* a_requested, RE::WAVEFORMATEX** a_supported) override
		{
			if (a_supported) {
				*a_supported = nullptr;
			}
			return IsFloat(a_requested) ? kOk : kFormatUnsupported;
		}

		std::int32_t Initialize(const void*, std::uint32_t) override { return kOk; }
		void         Reset() override {}

		std::int32_t LockForProcess(std::uint32_t, const RE::XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* a_input, std::uint32_t, const RE::XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS*) override
		{
			const auto& format = reinterpret_cast<const WAVEFORMATEX&>(*a_input->format);
			channels = format.nChannels;
			rate = format.nSamplesPerSec ? format.nSamplesPerSec : 44100;
			envelope.Init(static_cast<float>(rate));
			locked = true;
			return kOk;
		}

		void UnlockForProcess() override {}

		void Process(std::uint32_t, const RE::XAPO_PROCESS_BUFFER_PARAMETERS* a_input, std::uint32_t, RE::XAPO_PROCESS_BUFFER_PARAMETERS* a_output, BOOL) override
		{
			const std::uint32_t frames = a_input->validFrameCount;
			a_output->validFrameCount = frames;
			a_output->bufferFlags = a_input->bufferFlags;
			const auto* in = static_cast<const float*>(a_input->buffer);
			auto*       out = static_cast<float*>(a_output->buffer);
			const bool  silent = a_input->bufferFlags == RE::XAPO_BUFFER_FLAGS::XAPO_BUFFER_SILENT;
			if (silent || !locked) {
				if (in != out) {
					std::memcpy(out, in, static_cast<std::size_t>(frames) * channels * sizeof(float));
				}
				return;
			}

			const float target = Duck::Target(plan.duckDb, Duck::Conversation(menuState.load(std::memory_order_relaxed), NowMs()));
			if (envelope.value == 1.0f && target == 1.0f) {
				if (in != out) {
					std::memcpy(out, in, static_cast<std::size_t>(frames) * channels * sizeof(float));
				}
			} else {
				const auto state = _mm_getcsr();
				_mm_setcsr(state | 0x8040);
				for (std::uint32_t i = 0; i < frames; ++i) {
					const float gain = envelope.Step(target);
					for (std::uint32_t c = 0; c < channels; ++c) {
						out[i * channels + c] = in[i * channels + c] * gain;
					}
				}
				envelope.Settle(target);
				_mm_setcsr(state);
			}
			if (plan.tally) {
				plan.tally->frames.fetch_add(frames, std::memory_order_relaxed);
				if (envelope.value < kDuckedGain) {
					plan.tally->ducked.fetch_add(frames, std::memory_order_relaxed);
					const auto hundredths = static_cast<std::uint64_t>(-2000.0f * std::log10(envelope.value));
					plan.tally->depth.fetch_add(hundredths * frames, std::memory_order_relaxed);
				}
			}
		}

		std::uint32_t CalcInputFrames(std::uint32_t a_frames) override { return a_frames; }
		std::uint32_t CalcOutputFrames(std::uint32_t a_frames) override { return a_frames; }

		const Plan plan;

	private:
		static bool IsFloat(const RE::WAVEFORMATEX* a_format)
		{
			const auto& format = reinterpret_cast<const WAVEFORMATEX&>(*a_format);
			return format.wBitsPerSample == 32 && (format.wFormatTag == 3 || format.wFormatTag == 0xFFFE);
		}

		void Finish()
		{
			{
				std::scoped_lock guard(lock);
				const auto       found = taps.find(voice);
				if (found != taps.end() && found->second == this) {
					taps.erase(found);
				}
			}
		}

		std::atomic<std::uint32_t> references{ 1 };
		RE::IXAudio2SourceVoice*   voice;
		std::uint32_t              channels{ 1 };
		std::uint32_t              rate{ 44100 };
		Duck::Envelope             envelope;
		bool                       locked{ false };
	};

	bool MakePlan(RE::BSXAudio2GameSound* a_sound, RE::IXAudio2SourceVoice* a_voice, Plan& a_plan)
	{
		const auto& settings = Settings::GetSingleton();
		std::memcpy(&a_plan.key, &a_sound->resourceID, sizeof(Key));

		std::scoped_lock guard(lock);
		const auto       playing = taps.find(a_voice);
		if (playing != taps.end() && playing->second->plan.key == a_plan.key) {
			return false;
		}
		const auto group = GroupOf(a_sound->category);
		const bool music = group == Settings::kMusic || a_sound->IsMusic();
		if (group == Settings::kVoice) {
			++lines;
			if (settings.log && heardLines.fetch_sub(1) > 0) {
				const auto* category = a_sound->category;
				const auto  name = category && records.contains(category) ?
				                       std::format("{:08X}", static_cast<const RE::BGSSoundCategory*>(category)->GetFormID()) :
				                       std::format("engine object {:X}", reinterpret_cast<std::uintptr_t>(category));
				logger::info("voice {:08X}.{:08X} in category {}: dialogue menu {}, distance {:.1f} m", a_plan.key.folder, a_plan.key.file, name,
					dialogueOpen.load(std::memory_order_relaxed) ? "open (ducking)" : "closed (no duck)", a_sound->listenerDistance / 70.0f);
			}
			return true;
		}
		if (group == Settings::kBackground) {
			++backgroundLines;
			if (settings.log && heardLines.fetch_sub(1) > 0) {
				const auto* category = a_sound->category;
				const auto  name = category && records.contains(category) ?
				                       std::format("{:08X}", static_cast<const RE::BGSSoundCategory*>(category)->GetFormID()) :
				                       std::format("engine object {:X}", reinterpret_cast<std::uintptr_t>(category));
				logger::info("background voice {:08X}.{:08X} in category {}: does not duck, distance {:.1f} m", a_plan.key.folder, a_plan.key.file,
					name, a_sound->listenerDistance / 70.0f);
			}
			return false;
		}
		if (music && settings.duck > 0.0f) {
			a_plan.duckDb = -settings.duck;
			a_plan.tally = &musicTally;
			return true;
		}
		return false;
	}

	bool Attach(RE::BSXAudio2GameSound* a_sound)
	{
		auto* voice = a_sound->sourceVoice;
		if (!voice) {
			return false;
		}
		Plan plan;
		if (!MakePlan(a_sound, voice, plan)) {
			return true;
		}
		RE::XAUDIO2_VOICE_DETAILS details{};
		voice->GetVoiceDetails(&details);
		auto*                         tap = new Tap(plan, voice);
		RE::XAUDIO2_EFFECT_DESCRIPTOR descriptor{};
		descriptor.pEffect = reinterpret_cast<decltype(descriptor.pEffect)>(tap);
		descriptor.InitialState = TRUE;
		descriptor.OutputChannels = details.InputChannels;
		RE::XAUDIO2_EFFECT_CHAIN chain{ 1, &descriptor };
		const auto result = voice->SetEffectChain(&chain);
		if (result >= 0) {
			std::scoped_lock guard(lock);
			taps[voice] = tap;
		} else if (failures.fetch_add(1) < 5) {
			logger::warn("SetEffectChain failed: {:08X} ({} channels)", static_cast<std::uint32_t>(result), details.InputChannels);
		}
		tap->Release();
		return true;
	}

	void Summary()
	{
		if (!Settings::GetSingleton().log) {
			return;
		}
		const auto now = GetTickCount64();
		auto       last = lastSummary.load(std::memory_order_relaxed);
		if (last == 0) {
			lastSummary.compare_exchange_strong(last, now);
			return;
		}
		if (now < last + kSummaryMs || !lastSummary.compare_exchange_strong(last, now)) {
			return;
		}
		const auto part = [](Tally& a_tally) {
			const auto frames = a_tally.frames.exchange(0);
			const auto ducked = a_tally.ducked.exchange(0);
			const auto depth = a_tally.depth.exchange(0);
			if (frames == 0) {
				return "not playing"s;
			}
			return std::format("ducked {:.0f}% of the time{}", 100.0 * ducked / frames, ducked ? std::format(", {:.1f} dB on average then", depth / 100.0 / ducked) : ""s);
		};
		const auto music = part(musicTally);
		logger::info("last {:.0f} s: {} dialogue lines, {} background voice lines (no ducking); music {}", (now - last) / 1000.0, lines.exchange(0),
			backgroundLines.exchange(0), music);
	}

	struct UpdateHook
	{
		static bool Thunk(RE::BSXAudio2GameSound* a_this)
		{
			const bool result = original(a_this);
			Summary();
			return result;
		}

		static inline REL::Relocation<decltype(Thunk)> original;
	};

	struct PlayHook
	{
		static void Thunk(RE::BSXAudio2GameSound* a_this)
		{
			const bool early = Attach(a_this);
			original(a_this);
			if (!early) {
				Attach(a_this);
			}
		}

		static inline REL::Relocation<decltype(Thunk)> original;
	};

	class MenuWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::DialogueMenu::MENU_NAME) {
				dialogueOpen.store(a_event->opening, std::memory_order_relaxed);
				menuState.store(Duck::MenuChange(menuState.load(std::memory_order_relaxed), a_event->opening, NowMs()), std::memory_order_relaxed);
				if (Settings::GetSingleton().log && heardLines.load(std::memory_order_relaxed) > 0) {
					logger::info("dialogue menu {}", a_event->opening ? "opened" : "closed");
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	MenuWatch menuWatch;
}

void Leveller::Install()
{
	if (!Settings::GetSingleton().enabled) {
		logger::info("Disabled in the INI: nothing is hooked");
		return;
	}
	auto* data = RE::TESDataHandler::GetSingleton();
	if (!data) {
		logger::error("No data handler: nothing is hooked");
		return;
	}
	{
		std::scoped_lock guard(lock);
		for (auto* category : data->GetFormArray<RE::BGSSoundCategory>()) {
			if (category) {
				records.insert(static_cast<const RE::BSISoundCategory*>(category));
				GroupOf(static_cast<const RE::BSISoundCategory*>(category));
			}
		}
		logger::info("Sound categories {}", groups.size());
	}

	auto* ui = RE::UI::GetSingleton();
	if (!ui) {
		logger::error("No UI: nothing is hooked");
		return;
	}
	ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuWatch);

	REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSXAudio2GameSound[0] };
	PlayHook::original = vtable.write_vfunc(kPlayImpl, PlayHook::Thunk);
	if (Settings::GetSingleton().log) {
		UpdateHook::original = vtable.write_vfunc(kUpdate, UpdateHook::Thunk);
	}
	logger::info("Hooked sound playback: music ducks {:.1f} dB while the dialogue menu is open", Settings::GetSingleton().duck);
}
