#include "../src/client/header/ml_harness_view_rebase.h"

#include <assert.h>
#include <math.h>
#include <string.h>

int
main(void)
{
	ml_harness_view_rebase_t state;
	float command_pitch;
	float intended_pitch;
	int16_t command_short;
	int16_t intended_short;

	memset(&state, 0, sizeof(state));
	assert(ML_HarnessViewAcceptLife(&state, 1, 3, 2, 0, 1) == 0);
	assert(!state.pending && state.accepted_life_epoch == 1);
	assert(ML_HarnessViewAcceptLife(&state, 1, 17, 16, 1, 1) == 0);
	assert(ML_HarnessViewAcceptLife(&state, 3, 18, 17, 1, 1) == -1);
	assert(ML_HarnessViewAcceptLife(&state, 2, 18, 17, 1, 1) == 1);
	assert(state.pending && state.prior_life_epoch == 1 &&
		state.pending_life_epoch == 2 && state.pending_server_frame == 18 &&
		state.pending_applied_action_tick == 17);
	/* Same-life telemetry cannot release or move the pending view gate. */
	assert(ML_HarnessViewAcceptLife(&state, 2, 18, 17, 1, 1) == 0);
	assert(state.pending && state.pending_server_frame == 18);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 2, 18, 17, 1, 1, 0) == 0);
	assert(state.settling_seen);
	assert(!ML_HarnessViewSnapshotReady(&state, 17));
	assert(state.pending);
	assert(ML_HarnessViewSnapshotReady(&state, 19) == -1);
	assert(state.pending);
	assert(ML_HarnessViewSnapshotReady(&state, 18));
	assert(!state.pending);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 2, 19, 18, 1, 1, 0) == 1);
	/* Telemetry-before-playerstate waits; playerstate-before-telemetry is the
	 * same exact-frame check invoked immediately by the packet path. */
	assert(!ML_HarnessViewSnapshotReady(&state, 18));
	assert(ML_HarnessViewSnapshotReady(&state, 19));
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 2, 20, 19, 1, 1, 1) == 0);
	assert(!state.pending && !state.settle_tracking);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 2, 21, 20, 1, 1, 1) == 0);

	/* A respawn from a +24 degree corpse command angle has a -24 degree
	 * delta angle and an authoritative level server view. */
	command_pitch = ML_HarnessViewCommandAngle(0.0f, (int16_t)-4369);
	assert(fabsf(command_pitch - 23.999634f) < 0.00001f);
	command_short = (int16_t)((int)(command_pitch *
		(65536.0f / 360.0f)) & 65535);
	intended_short = (int16_t)(command_short + (int16_t)-4369);
	intended_pitch = (float)intended_short * (360.0f / 65536.0f);
	assert(fabsf(intended_pitch) < 0.01f);
	/* Applying the next -2 degree decision in command space yields the exact
	 * server-space -2 degree change after the same delta transform. */
	command_short = (int16_t)((int)((command_pitch - 2.0f) *
		(65536.0f / 360.0f)) & 65535);
	intended_short = (int16_t)(command_short + (int16_t)-4369);
	intended_pitch = (float)intended_short * (360.0f / 65536.0f);
	assert(fabsf(intended_pitch - -2.0f) < 0.01f);

	/* Arbitrary non-cancelling policy pitch cannot drift command space while
	 * stock PMF_TIME_TELEPORT suppresses pitch. */
	command_pitch = ML_HarnessViewCommandAngle(0.0f, (int16_t)-4369);
	command_pitch = ML_HarnessViewApplyPolicyPitch(command_pitch, 3.0f, 1);
	command_pitch = ML_HarnessViewApplyPolicyPitch(command_pitch, 4.0f, 1);
	assert(fabsf(command_pitch - 23.999634f) < 0.00001f);
	command_pitch = ML_HarnessViewApplyPolicyPitch(command_pitch, 5.0f, 0);
	command_short = (int16_t)((int)(command_pitch *
		(65536.0f / 360.0f)) & 65535);
	intended_short = (int16_t)(command_short + (int16_t)-4369);
	intended_pitch = (float)intended_short * (360.0f / 65536.0f);
	assert(fabsf(intended_pitch - 5.0f) < 0.01f);

	/* A map epoch establishes a new identity without carrying a life gate. */
	assert(ML_HarnessViewAcceptLife(&state, 9, 2, 1, 0, 1) == 0);
	assert(!state.pending && state.accepted_life_epoch == 9 &&
		state.pending_server_frame == 0 &&
		state.pending_applied_action_tick == 0);

	memset(&state, 0, sizeof(state));
	state.accepted_life_epoch = UINT32_MAX;
	assert(ML_HarnessViewAcceptLife(&state, 1, 99, 98, 1, 1) == 1);

	/* Initial-spawn and mid-life teleport holds use the same generic causal
	 * release transaction; neither requires a life-epoch change. */
	memset(&state, 0, sizeof(state));
	assert(ML_HarnessViewAcceptLife(&state, 4, 2, 1, 0, 1) == 0);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 2, 1, 1, 1, 0) == 1);
	assert(ML_HarnessViewSnapshotReady(&state, 2));
	assert(state.settle_tracking && state.settling_seen);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 3, 2, 1, 1, 0) == 1);
	assert(ML_HarnessViewSnapshotReady(&state, 3));
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 4, 3, 1, 1, 1) == 0);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 5, 4, 1, 1, 1) == 0);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 8, 7, 1, 1, 0) == 1);
	assert(ML_HarnessViewSnapshotReady(&state, 8) == 1);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 9, 8, 1, 1, 1) == 0);

	/* Intermission PM_FREEZE and death/GIB never call the settle observer.
	 * Their non-normal role boundary clears a consumed T0 history without
	 * arming a rebase. */
	assert(!state.pending && !state.settle_tracking);
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 10, 9, 1, 1, 0) == 1);
	assert(ML_HarnessViewSnapshotReady(&state, 10) == 1);
	assert(state.settle_tracking && state.settling_seen);
	assert(ML_HarnessViewLeavePublicNormal(&state) == 0);
	assert(!state.pending && !state.settle_tracking && !state.settling_seen);

	/* A non-normal lifecycle packet cannot overtake an exact T0 snapshot. */
	assert(ML_HarnessViewObserveSettleCausal(
		&state, 4, 11, 10, 1, 1, 0) == 1);
	assert(ML_HarnessViewLeavePublicNormal(&state) == -1);
	assert(state.pending && state.settle_tracking);
	assert(ML_HarnessViewSnapshotReady(&state, 11) == 1);
	assert(ML_HarnessViewLeavePublicNormal(&state) == 0);
	return 0;
}
