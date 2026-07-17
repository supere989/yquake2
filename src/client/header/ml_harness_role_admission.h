#ifndef ML_HARNESS_ROLE_ADMISSION_H
#define ML_HARNESS_ROLE_ADMISSION_H

#include <stdint.h>

#define ML_HARNESS_CAUSAL_TRANSITION_TRAINABLE (1u << 16)
#define ML_HARNESS_CAUSAL_ROLE_PLAYING (1u << 20)
#define ML_HARNESS_CAUSAL_ROLE_PUBLIC_PM_NORMAL (1u << 21)

typedef struct
{
	uint32_t pending_server_frame;
	uint32_t pending_flags;
	int pending;
} ml_harness_role_admission_t;

static inline int
ML_HarnessRoleFlagsValid(uint32_t flags)
{
	return (flags & ML_HARNESS_CAUSAL_ROLE_PLAYING) != 0 &&
		(!(flags & ML_HARNESS_CAUSAL_ROLE_PUBLIC_PM_NORMAL) ||
		(flags & ML_HARNESS_CAUSAL_ROLE_PLAYING));
}

static inline int
ML_HarnessRoleArm(ml_harness_role_admission_t *state,
		uint32_t server_frame, uint32_t flags)
{
	uint32_t role_flags = flags &
		(ML_HARNESS_CAUSAL_ROLE_PLAYING |
		 ML_HARNESS_CAUSAL_ROLE_PUBLIC_PM_NORMAL |
		 ML_HARNESS_CAUSAL_TRANSITION_TRAINABLE);

	if (!state || server_frame == 0 || !ML_HarnessRoleFlagsValid(flags))
		return -1;
	if (state->pending)
		return state->pending_server_frame == server_frame &&
			state->pending_flags == role_flags ? 0 : -1;
	state->pending_server_frame = server_frame;
	state->pending_flags = role_flags;
	state->pending = 1;
	return 1;
}

/* Validate the exact ordinary protocol playerstate corresponding to the
 * authenticated causal packet.  A spectator is never an admitted lifecycle
 * state.  Dead/GIB and intermission freeze are permitted only when the
 * engine marks the transition nontrainable. */
static inline int
ML_HarnessRoleSnapshotReady(ml_harness_role_admission_t *state,
		uint32_t snapshot_server_frame, int public_pm_normal,
		int public_pm_spectator, int public_nontrainable_lifecycle)
{
	int causal_pm_normal;
	int causal_trainable;

	if (!state || !state->pending)
		return 0;
	if (snapshot_server_frame < state->pending_server_frame)
		return 0;
	if (snapshot_server_frame > state->pending_server_frame)
		return -1;
	causal_pm_normal =
		(state->pending_flags &
		 ML_HARNESS_CAUSAL_ROLE_PUBLIC_PM_NORMAL) != 0;
	causal_trainable =
		(state->pending_flags &
		 ML_HARNESS_CAUSAL_TRANSITION_TRAINABLE) != 0;
	if (public_pm_spectator || causal_pm_normal != public_pm_normal ||
		(!public_pm_normal &&
		 (!public_nontrainable_lifecycle || causal_trainable)))
		return -1;
	state->pending = 0;
	return 1;
}

static inline int
ML_HarnessBootstrapRoleReady(int valid_frame, int public_pm_normal)
{
	return valid_frame && public_pm_normal;
}

#endif
