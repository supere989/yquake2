#include "../src/server/header/sv_ml_frame_barrier_core.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#define CLIENTS 4u

static int proof_duplicate;
static int proof_reorder;
static int proof_missing;
static int proof_map_reset;
static int proof_death_reset;
static int proof_future_fault;
static int proof_disconnect_fault;

static void roster(ml_frame_barrier_t *barrier, uint64_t now)
{
	uint32_t slot;
	ML_FrameBarrierInit(barrier, 1, CLIENTS, 100);
	for (slot = 0; slot < CLIENTS; slot++)
	{
		assert(ML_FrameBarrierConnect(barrier, slot, slot, now) ==
			ML_BARRIER_INPUT_ACCEPTED);
		assert(ML_FrameBarrierRegister(barrier, slot) ==
			ML_BARRIER_INPUT_ACCEPTED);
	}
}

static uint64_t microproof(void)
{
	ml_frame_barrier_t barrier;
	uint64_t digest = UINT64_C(1469598103934665603);
	uint32_t slot, tick;
	roster(&barrier, 10);
	for (slot = 0; slot < CLIENTS; slot++)
		assert(ML_FrameBarrierStageBootstrap(&barrier, slot,
			UINT64_C(1000) + slot) == ML_BARRIER_INPUT_ACCEPTED);
	assert(ML_FrameBarrierPoll(&barrier, 11) == ML_BARRIER_GATE_BOOTSTRAP);
	ML_FrameBarrierCommitted(&barrier, 1, 11);
	for (tick = 1; tick <= 32; tick++)
	{
		/* Ready and command arrival order intentionally differs by slot. */
		for (slot = CLIENTS; slot-- > 0;)
			assert(ML_FrameBarrierReady(&barrier, slot, tick) ==
				ML_BARRIER_INPUT_ACCEPTED);
		for (slot = 0; slot < CLIENTS; slot++)
		{
			uint64_t fingerprint = UINT64_C(100000) + tick * 17u + slot;
			assert(ML_FrameBarrierStageAction(&barrier, slot,
				tick % ML_FRAME_BARRIER_ACTION_GENERATIONS, fingerprint) ==
				ML_BARRIER_INPUT_ACCEPTED);
			assert(ML_FrameBarrierStageAction(&barrier, slot,
				tick % ML_FRAME_BARRIER_ACTION_GENERATIONS, fingerprint) ==
				ML_BARRIER_INPUT_IDEMPOTENT);
			digest ^= fingerprint;
			digest *= UINT64_C(1099511628211);
		}
		assert(ML_FrameBarrierPoll(&barrier, 11 + tick) ==
			ML_BARRIER_GATE_ACTIONS);
		ML_FrameBarrierCommitted(&barrier, tick + 1, 11 + tick);
	}
	proof_duplicate = 1;
	proof_reorder = 1;
	return digest;
}

static void fault_matrix(void)
{
	ml_frame_barrier_t barrier;
	uint32_t slot;

	ML_FrameBarrierInit(&barrier, 1, CLIENTS, 100);
	assert(ML_FrameBarrierConnect(&barrier, 1, 0, 1) ==
		ML_BARRIER_INPUT_REJECTED);
	assert(barrier.fault == ML_BARRIER_FAULT_ROSTER);

	roster(&barrier, 10);
	assert(ML_FrameBarrierStageBootstrap(&barrier, 0, 7) ==
		ML_BARRIER_INPUT_ACCEPTED);
	assert(ML_FrameBarrierStageBootstrap(&barrier, 0, 8) ==
		ML_BARRIER_INPUT_REJECTED);
	assert(barrier.fault == ML_BARRIER_FAULT_COMMAND_CONFLICT);

	roster(&barrier, 10);
	for (slot = 0; slot < CLIENTS; slot++)
		assert(ML_FrameBarrierStageBootstrap(&barrier, slot, 20 + slot) ==
			ML_BARRIER_INPUT_ACCEPTED);
	assert(ML_FrameBarrierPoll(&barrier, 11) == ML_BARRIER_GATE_BOOTSTRAP);
	ML_FrameBarrierCommitted(&barrier, 1, 11);
	assert(ML_FrameBarrierReady(&barrier, 0, 2) ==
		ML_BARRIER_INPUT_REJECTED);
	assert(barrier.fault == ML_BARRIER_FAULT_FUTURE_READY);
	proof_future_fault = 1;

	roster(&barrier, 10);
	assert(ML_FrameBarrierPoll(&barrier, 10) == ML_BARRIER_GATE_HOLD);
	assert(ML_FrameBarrierPoll(&barrier, 111) == ML_BARRIER_GATE_FAULT);
	assert(barrier.fault == ML_BARRIER_FAULT_TIMEOUT);
	proof_missing = 1;

	/* Loading clients are launcher-owned, not a frame-barrier timeout.  The
	 * timer starts only after the complete active/bootstrap-ready roster has
	 * registered, regardless of how long map/client loading took. */
	ML_FrameBarrierInit(&barrier, 1, CLIENTS, 100);
	for (slot = 0; slot < CLIENTS - 1; slot++)
	{
		assert(ML_FrameBarrierConnect(&barrier, slot, slot, 10 + slot) ==
			ML_BARRIER_INPUT_ACCEPTED);
		assert(ML_FrameBarrierRegister(&barrier, slot) ==
			ML_BARRIER_INPUT_ACCEPTED);
	}
	assert(ML_FrameBarrierPoll(&barrier, 10000) == ML_BARRIER_GATE_HOLD);
	assert(barrier.phase == ML_BARRIER_WAIT_ROSTER);
	assert(ML_FrameBarrierConnect(&barrier, CLIENTS - 1, CLIENTS - 1, 10001) ==
		ML_BARRIER_INPUT_ACCEPTED);
	assert(ML_FrameBarrierRegister(&barrier, CLIENTS - 1) ==
		ML_BARRIER_INPUT_ACCEPTED);
	assert(ML_FrameBarrierPoll(&barrier, 10001) == ML_BARRIER_GATE_HOLD);
	assert(barrier.phase == ML_BARRIER_WAIT_BOOTSTRAP);
	assert(ML_FrameBarrierPoll(&barrier, 10102) == ML_BARRIER_GATE_FAULT);
	assert(barrier.fault == ML_BARRIER_FAULT_TIMEOUT);

	roster(&barrier, 10);
	ML_FrameBarrierDisconnect(&barrier, 2);
	assert(barrier.fault == ML_BARRIER_FAULT_DISCONNECT);
	proof_disconnect_fault = 1;

	/* A move cannot become bootstrap fuel before the explicit registration
	 * announcement, even if it has a valid fingerprint. */
	ML_FrameBarrierInit(&barrier, 1, 1, 100);
	assert(ML_FrameBarrierConnect(&barrier, 0, 0, 10) ==
		ML_BARRIER_INPUT_ACCEPTED);
	assert(ML_FrameBarrierStageBootstrap(&barrier, 0, 9) ==
		ML_BARRIER_INPUT_REJECTED);
	assert(barrier.fault == ML_BARRIER_FAULT_ROSTER);

	roster(&barrier, 10);
	ML_FrameBarrierResetMap(&barrier);
	assert(barrier.map_epoch == 1 && barrier.phase == ML_BARRIER_WAIT_ROSTER);
	for (slot = 0; slot < CLIENTS; slot++)
		assert(barrier.slots[slot].connected &&
			!barrier.slots[slot].registered);
	proof_map_reset = 1;

	roster(&barrier, 10);
	ML_FrameBarrierDeath(&barrier, 0);
	assert(!barrier.slots[0].command_ready &&
		!barrier.slots[0].action_ready);
	proof_death_reset = 1;
}

int main(void)
{
	uint64_t first, second;
	fault_matrix();
	first = microproof();
	second = microproof();
	assert(first == second);
	assert(proof_duplicate && proof_reorder && proof_missing &&
		proof_map_reset && proof_death_reset && proof_future_fault &&
		proof_disconnect_fault);
	printf("{\"schema\":\"q2-network-client-frame-barrier-core-v1\","
		"\"clients\":4,\"frames\":32,\"digest\":\"%016" PRIx64 "\","
		"\"duplicate\":%s,\"reorder\":%s,\"missing\":%s,"
		"\"map_reset\":%s,\"death_reset\":%s,"
		"\"future_fault\":%s,\"disconnect_fault\":%s}\n", first,
		proof_duplicate ? "true" : "false",
		proof_reorder ? "true" : "false",
		proof_missing ? "true" : "false",
		proof_map_reset ? "true" : "false",
		proof_death_reset ? "true" : "false",
		proof_future_fault ? "true" : "false",
		proof_disconnect_fault ? "true" : "false");
	return 0;
}
