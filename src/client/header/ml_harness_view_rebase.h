#ifndef ML_HARNESS_VIEW_REBASE_H
#define ML_HARNESS_VIEW_REBASE_H

#include <stdint.h>

#define ML_HARNESS_VIEW_REBASE_LIFE 1
#define ML_HARNESS_VIEW_REBASE_SETTLE_FRAME 2

typedef struct
{
	uint32_t accepted_life_epoch;
	uint32_t prior_life_epoch;
	uint32_t pending_life_epoch;
	uint32_t pending_server_frame;
	uint32_t pending_applied_action_tick;
	int pending_kind;
	int pending;
	int settle_tracking;
	int settling_seen;
} ml_harness_view_rebase_t;

/* Returns -1 for an invalid/non-consecutive same-map life identity, 0 when
 * no view boundary is needed, and 1 when a normal playerstate rebase must
 * complete before another policy command may be staged. */
static inline int
ML_HarnessViewAcceptLife(ml_harness_view_rebase_t *state,
		uint32_t life_epoch, uint32_t server_frame,
		uint32_t applied_action_tick, int same_map_epoch,
		int role_public_pm_normal)
{
	uint32_t expected;

	if (!state || life_epoch == 0 || server_frame == 0 ||
		applied_action_tick == 0 || applied_action_tick != server_frame - 1u)
	{
		return -1;
	}

	if (!same_map_epoch || state->accepted_life_epoch == 0)
	{
		state->accepted_life_epoch = life_epoch;
		state->prior_life_epoch = 0;
		state->pending_life_epoch = 0;
		state->pending_server_frame = 0;
		state->pending_applied_action_tick = 0;
		state->pending_kind = 0;
		state->pending = 0;
		state->settle_tracking = 0;
		state->settling_seen = 0;
		return 0;
	}

	if (life_epoch == state->accepted_life_epoch)
	{
		return 0;
	}

	expected = state->accepted_life_epoch == UINT32_MAX
		? 1u : state->accepted_life_epoch + 1u;
	if (life_epoch != expected || state->pending)
	{
		return -1;
	}
	if (!role_public_pm_normal)
	{
		/* BeginIntermission may respawn a dead player, advance its life
		 * identity, and immediately publish PM_FREEZE/SOLID_NOT.  Accept the
		 * consecutive identity but do not invent a playable view basis. */
		state->accepted_life_epoch = life_epoch;
		state->prior_life_epoch = 0;
		state->pending_life_epoch = 0;
		state->pending_server_frame = 0;
		state->pending_applied_action_tick = 0;
		state->pending_kind = 0;
		state->pending = 0;
		state->settle_tracking = 0;
		state->settling_seen = 0;
		return 0;
	}

	state->prior_life_epoch = state->accepted_life_epoch;
	state->accepted_life_epoch = life_epoch;
	state->pending_life_epoch = life_epoch;
	state->pending_server_frame = server_frame;
	state->pending_applied_action_tick = applied_action_tick;
	state->pending_kind = ML_HARNESS_VIEW_REBASE_LIFE;
	state->pending = 1;
	state->settle_tracking = 1;
	state->settling_seen = 0;
	return 1;
}

/* ECHO_VALID + FACTS_COMPLETE + !TRANSITION_TRAINABLE is an ordinary stock
 * teleport-settling frame.  Every such packet owns an exact playerstate
 * rebase before another action may be staged; this handles both UDP orderings
 * and includes the entry-latched command that clears pm_time after Pmove. */
static inline int
ML_HarnessViewObserveSettleCausal(ml_harness_view_rebase_t *state,
		uint32_t life_epoch, uint32_t server_frame,
		uint32_t applied_action_tick, int echo_valid,
		int facts_complete, int transition_trainable)
{
	if (!state || life_epoch != state->accepted_life_epoch || server_frame == 0 ||
		applied_action_tick == 0 || applied_action_tick != server_frame - 1u ||
		!echo_valid || !facts_complete)
	{
		return state && state->settle_tracking ? -1 : 0;
	}
	if (!transition_trainable)
	{
		state->settle_tracking = 1;
		state->settling_seen = 1;
		if (state->pending)
		{
			return state->pending_server_frame == server_frame ? 0 : -1;
		}
		state->prior_life_epoch = life_epoch;
		state->pending_life_epoch = life_epoch;
		state->pending_server_frame = server_frame;
		state->pending_applied_action_tick = applied_action_tick;
		state->pending_kind = ML_HARNESS_VIEW_REBASE_SETTLE_FRAME;
		state->pending = 1;
		return 1;
	}
	if (!state->settle_tracking)
	{
		return 0;
	}
	if (!state->settling_seen || state->pending)
	{
		return -1;
	}
	state->settle_tracking = 0;
	state->settling_seen = 0;
	return 0;
}

/* A routed non-normal role packet (death/GIB/intermission freeze) is an
 * exact nontrainable lifecycle boundary, not evidence of teleport settling.
 * It may clear an already consumed T0/T0... history, but it cannot overtake
 * an exact settle-frame playerstate that is still pending. */
static inline int
ML_HarnessViewLeavePublicNormal(ml_harness_view_rebase_t *state)
{
	if (!state)
		return -1;
	if (state->pending &&
		state->pending_kind == ML_HARNESS_VIEW_REBASE_SETTLE_FRAME)
		return -1;
	state->settle_tracking = 0;
	state->settling_seen = 0;
	return 0;
}

static inline int
ML_HarnessViewSnapshotReady(ml_harness_view_rebase_t *state,
		uint32_t snapshot_server_frame)
{
	if (!state || !state->pending)
	{
		return 0;
	}
	if (snapshot_server_frame < state->pending_server_frame)
	{
		return 0;
	}
	if (snapshot_server_frame > state->pending_server_frame)
	{
		return -1;
	}
	state->pending = 0;
	return 1;
}

/* Quake usercmd angles are command-space angles.  The ordinary server
 * playerstate exposes both the accepted view and its signed delta-angle
 * transform, so no privileged aim/world value is needed for the rebase. */
static inline float
ML_HarnessViewCommandAngle(float server_view_angle, int16_t delta_angle)
{
	return server_view_angle - (float)delta_angle * (360.0f / 65536.0f);
}

static inline float
ML_HarnessViewApplyPolicyPitch(float command_pitch, float policy_delta,
		int teleport_settling)
{
	return teleport_settling ? command_pitch : command_pitch + policy_delta;
}

#endif
