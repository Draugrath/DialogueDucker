#pragma once

#include <cmath>
#include <cstdint>

namespace Duck
{
	constexpr float         kDownSeconds = 1.5f;
	constexpr float         kUpSeconds = 1.0f;

	inline float ToGain(float a_db) { return std::pow(10.0f, a_db / 20.0f); }

	inline float Target(float a_duckDb, bool a_conversation)
	{
		return a_duckDb < 0.0f && a_conversation ? ToGain(a_duckDb) : 1.0f;
	}

	constexpr std::int64_t kStartDelayMs = 80;

	inline bool Conversation(std::uint64_t a_state, std::int64_t a_nowMs)
	{
		const bool         open = (a_state & 1) != 0;
		const bool         before = (a_state & 2) != 0;
		const std::int64_t changed = static_cast<std::int64_t>(a_state >> 2);
		return a_nowMs - changed >= kStartDelayMs ? open : before;
	}

	inline std::uint64_t MenuChange(std::uint64_t a_old, bool a_open, std::int64_t a_nowMs)
	{
		const bool before = Conversation(a_old, a_nowMs);
		return (static_cast<std::uint64_t>(a_nowMs) << 2) | (before ? 2u : 0u) | (a_open ? 1u : 0u);
	}

	struct Envelope
	{
		double value{ 1.0 };
		double downSamples{ 66150.0 };
		double upSamples{ 44100.0 };

		void Init(float a_rate)
		{
			const double rate = a_rate > 0.0f ? a_rate : 44100.0;
			downSamples = kDownSeconds * rate;
			upSamples = kUpSeconds * rate;
		}

		float Step(float a_target)
		{
			if (a_target == value) {
				moving = false;
				return static_cast<float>(value);
			}
			if (!moving || a_target != moveTarget) {
				moving = true;
				moveTarget = a_target;
				moveLeft = static_cast<std::int64_t>(std::ceil(a_target < value ? downSamples : upSamples));
				moveFactor = std::pow(static_cast<double>(a_target) / value, 1.0 / static_cast<double>(moveLeft));
			}
			if (--moveLeft <= 0) {
				value = a_target;
				moving = false;
			} else {
				value *= moveFactor;
			}
			return static_cast<float>(value);
		}

		void Settle(float a_target)
		{
			if (std::fabs(a_target - value) < 1e-3) {
				value = a_target;
				moving = false;
			}
		}

	private:
		bool         moving{ false };
		float        moveTarget{ 1.0f };
		std::int64_t moveLeft{ 0 };
		double       moveFactor{ 1.0 };
	};
}
