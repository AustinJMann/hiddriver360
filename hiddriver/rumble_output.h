#pragma once

#include <stdint.h>

namespace RumbleOutput {

// SDL resends every 40 ms for a roughly 50 ms firmware timeout. Leave more
// scheduling headroom for the shared control endpoint on the Xbox.
static const uint32_t kRefreshMs = 30;
static const uint32_t kServiceMs = 5;

inline uint64_t Request(uint32_t generation, uint16_t left, uint16_t right) {
	return ((uint64_t)generation << 32) | ((uint32_t)left << 16) | right;
}
inline uint32_t Generation(uint64_t request) { return (uint32_t)(request >> 32); }
inline uint16_t Left(uint64_t request) { return (uint16_t)(request >> 16); }
inline uint16_t Right(uint64_t request) { return (uint16_t)request; }
inline bool Active(uint64_t request) { return (uint32_t)request != 0; }

// Zero-initializable; used only on the USB processor at dispatch IRQL.
// desired is a snapshot of the atomic, generation-tagged XAM mailbox.
struct State {
	uint64_t desired;
	uint64_t acknowledged;
	uint64_t inFlight;
	uint32_t submittedAt;
	uint32_t retryAt;
	uint32_t failures;
	bool pending;

	void Update(uint64_t request) {
		if (Generation(desired) != Generation(request)) {
			acknowledged = 0;
			failures = 0;
		}
		desired = request;
	}

	bool Due(uint32_t now) const {
		if (pending || Generation(desired) == 0) return false;
		if (failures && (int32_t)(now - retryAt) < 0) return false;
		return failures != 0 || desired != acknowledged ||
			(Active(desired) && (uint32_t)(now - submittedAt) >= kRefreshMs);
	}

	void Submitted(uint32_t now) {
		inFlight = desired;
		submittedAt = now;
		pending = true;
	}

	void Complete(bool success, uint32_t now) {
		pending = false;
		// A completion from the previous binding must not acknowledge the new one.
		if (Generation(inFlight) != Generation(desired)) return;
		if (success) {
			acknowledged = inFlight;
			failures = 0;
		} else {
			if (failures < 6) ++failures;
			uint32_t delay = 10u << (failures - 1);
			retryAt = now + (delay > 250 ? 250 : delay);
		}
	}
};

} // namespace RumbleOutput
