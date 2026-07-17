#include "../src/client/header/ml_harness_role_admission.h"
#include "../src/client/header/ml_harness_view_rebase.h"

#include <assert.h>
#include <string.h>

int main(void)
{
	ml_harness_role_admission_t state;
	ml_harness_view_rebase_t view;
	uint32_t playing = ML_HARNESS_CAUSAL_ROLE_PLAYING;
	uint32_t normal = playing | ML_HARNESS_CAUSAL_ROLE_PUBLIC_PM_NORMAL;
	uint32_t playing_e1_f1_t0 = playing | (1u << 14) | (1u << 15);
	uint32_t normal_e1_f1_t0 = normal | (1u << 14) | (1u << 15);

	memset(&state, 0, sizeof(state));
	assert(ML_HarnessBootstrapRoleReady(1, 1));
	assert(!ML_HarnessBootstrapRoleReady(0, 1));
	assert(!ML_HarnessBootstrapRoleReady(1, 0));

	assert(ML_HarnessRoleArm(&state, 18, normal) == 1);
	assert(ML_HarnessRoleSnapshotReady(&state, 17, 1, 0, 0) == 0);
	assert(ML_HarnessRoleSnapshotReady(&state, 19, 1, 0, 0) == -1);
	assert(ML_HarnessRoleSnapshotReady(&state, 18, 1, 0, 0) == 1);

	assert(ML_HarnessRoleArm(&state, 19, normal) == 1);
	assert(ML_HarnessRoleSnapshotReady(&state, 19, 0, 1, 0) == -1);
	memset(&state, 0, sizeof(state));
	assert(ML_HarnessRoleArm(&state, 19, playing) == 1);
	assert(ML_HarnessRoleSnapshotReady(&state, 19, 0, 1, 0) == -1);

	memset(&state, 0, sizeof(state));
	assert(ML_HarnessRoleArm(&state, 20, playing) == 1);
	/* Exact intermission PM_FREEZE is role-valid only as nontrainable. */
	assert(ML_HarnessRoleSnapshotReady(&state, 20, 0, 0, 1) == 1);
	assert(ML_HarnessRoleArm(&state, 21, playing |
		ML_HARNESS_CAUSAL_TRANSITION_TRAINABLE) == 1);
	assert(ML_HarnessRoleSnapshotReady(&state, 21, 0, 0, 1) == -1);
	/* PM_SPECTATOR remains fatal even with an otherwise valid playing fact. */
	memset(&state, 0, sizeof(state));
	assert(ML_HarnessRoleArm(&state, 22, playing) == 1);
	assert(ML_HarnessRoleSnapshotReady(&state, 22, 0, 1, 1) == -1);

	/* BeginIntermission may advance life then publish exact PM_FREEZE.  The
	 * playing/non-normal E1/F1/T0 role is accepted, but life acceptance and
	 * settle classification must leave the view transaction fully unarmed. */
	memset(&state, 0, sizeof(state));
	memset(&view, 0, sizeof(view));
	assert(ML_HarnessViewAcceptLife(&view, 8, 30, 29, 0, 1) == 0);
	assert(ML_HarnessRoleArm(&state, 31, playing_e1_f1_t0) == 1);
	assert(ML_HarnessViewAcceptLife(&view, 9, 31, 30, 1, 0) == 0);
	assert(ML_HarnessViewLeavePublicNormal(&view) == 0);
	assert(view.accepted_life_epoch == 9 && !view.pending &&
		!view.settle_tracking && !view.settling_seen &&
		view.pending_kind == 0 && view.pending_life_epoch == 0 &&
		view.pending_server_frame == 0 &&
		view.pending_applied_action_tick == 0);
	assert(ML_HarnessRoleSnapshotReady(&state, 31, 0, 0, 1) == 1);

	/* The public-normal equivalent advances the same identity and arms the
	 * exact life view rebase. */
	memset(&state, 0, sizeof(state));
	memset(&view, 0, sizeof(view));
	assert(ML_HarnessViewAcceptLife(&view, 8, 30, 29, 0, 1) == 0);
	assert(ML_HarnessRoleArm(&state, 31, normal_e1_f1_t0) == 1);
	assert(ML_HarnessViewAcceptLife(&view, 9, 31, 30, 1, 1) == 1);
	assert(view.pending &&
		view.pending_kind == ML_HARNESS_VIEW_REBASE_LIFE);
	assert(ML_HarnessRoleSnapshotReady(&state, 31, 1, 0, 0) == 1);

	memset(&state, 0, sizeof(state));
	assert(ML_HarnessRoleArm(&state, 23, 0) == -1);
	assert(ML_HarnessRoleArm(&state, 23,
		ML_HARNESS_CAUSAL_ROLE_PUBLIC_PM_NORMAL) == -1);
	return 0;
}
