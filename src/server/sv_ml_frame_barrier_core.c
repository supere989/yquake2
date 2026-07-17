#include "header/sv_ml_frame_barrier_core.h"

#include <string.h>

static void
ML_Fault(ml_frame_barrier_t *barrier, ml_barrier_fault_t fault)
{
	if (barrier && barrier->phase != ML_BARRIER_FAULT)
	{
		barrier->fault = fault;
		barrier->phase = ML_BARRIER_FAULT;
	}
}

static int
ML_SlotValid(const ml_frame_barrier_t *barrier, uint32_t slot)
{
	return barrier && barrier->enabled &&
		slot < barrier->configured_clients &&
		slot < ML_FRAME_BARRIER_MAX_CLIENTS;
}

static void
ML_ClearTransactions(ml_frame_barrier_t *barrier)
{
	uint32_t slot;
	for (slot = 0; slot < barrier->configured_clients; slot++)
	{
		barrier->slots[slot].command_ready = 0;
		barrier->slots[slot].action_ready = 0;
		barrier->slots[slot].command_generation = 0;
		barrier->slots[slot].action_tick = 0;
		barrier->slots[slot].command_fingerprint = 0;
	}
}

void
ML_FrameBarrierInit(ml_frame_barrier_t *barrier, int enabled,
	uint32_t configured_clients, uint32_t timeout_ms)
{
	if (!barrier)
	{
		return;
	}
	memset(barrier, 0, sizeof(*barrier));
	barrier->enabled = enabled ? 1u : 0u;
	barrier->configured_clients = configured_clients;
	barrier->timeout_ms = timeout_ms;
	barrier->phase = enabled ? ML_BARRIER_WAIT_ROSTER : ML_BARRIER_DISABLED;
	if (enabled && (!configured_clients ||
		configured_clients > ML_FRAME_BARRIER_MAX_CLIENTS || !timeout_ms))
	{
		ML_Fault(barrier, ML_BARRIER_FAULT_CONFIGURATION);
	}
}

void
ML_FrameBarrierResetMap(ml_frame_barrier_t *barrier)
{
	uint32_t slot;
	if (!barrier || !barrier->enabled)
	{
		return;
	}
	barrier->fault = ML_BARRIER_FAULT_NONE;
	barrier->expected_action_tick = 0;
	barrier->hold_started_ms = 0;
	barrier->phase = ML_BARRIER_WAIT_ROSTER;
	barrier->map_epoch++;
	ML_ClearTransactions(barrier);
	for (slot = 0; slot < barrier->configured_clients; slot++)
	{
		barrier->slots[slot].registered = 0;
	}
}

ml_barrier_input_result_t
ML_FrameBarrierConnect(ml_frame_barrier_t *barrier, uint32_t slot,
	uint32_t suffix, uint64_t now_ms)
{
	ml_barrier_slot_t *member;
	if (!ML_SlotValid(barrier, slot) || suffix != slot)
	{
		ML_Fault(barrier, ML_BARRIER_FAULT_ROSTER);
		return ML_BARRIER_INPUT_REJECTED;
	}
	member = &barrier->slots[slot];
	if (member->connected)
	{
		return ML_BARRIER_INPUT_IDEMPOTENT;
	}
	member->connected = 1;
	if (!barrier->hold_started_ms)
	{
		barrier->hold_started_ms = now_ms ? now_ms : 1;
	}
	return ML_BARRIER_INPUT_ACCEPTED;
}

ml_barrier_input_result_t
ML_FrameBarrierRegister(ml_frame_barrier_t *barrier, uint32_t slot)
{
	ml_barrier_slot_t *member;
	if (!ML_SlotValid(barrier, slot) || !barrier->slots[slot].connected)
	{
		ML_Fault(barrier, ML_BARRIER_FAULT_ROSTER);
		return ML_BARRIER_INPUT_REJECTED;
	}
	member = &barrier->slots[slot];
	if (member->registered)
	{
		return ML_BARRIER_INPUT_IDEMPOTENT;
	}
	member->registered = 1;
	return ML_BARRIER_INPUT_ACCEPTED;
}

void
ML_FrameBarrierDisconnect(ml_frame_barrier_t *barrier, uint32_t slot)
{
	if (!ML_SlotValid(barrier, slot))
	{
		return;
	}
	barrier->slots[slot].connected = 0;
	barrier->slots[slot].registered = 0;
	ML_Fault(barrier, ML_BARRIER_FAULT_DISCONNECT);
}

void
ML_FrameBarrierDeath(ml_frame_barrier_t *barrier, uint32_t slot)
{
	ml_barrier_slot_t *member;
	if (!ML_SlotValid(barrier, slot))
	{
		return;
	}
	member = &barrier->slots[slot];
	member->command_ready = 0;
	member->action_ready = 0;
	member->command_generation = 0;
	member->action_tick = 0;
	member->command_fingerprint = 0;
}

void
ML_FrameBarrierProtocolFault(ml_frame_barrier_t *barrier)
{
	ML_Fault(barrier, ML_BARRIER_FAULT_PROTOCOL);
}

static ml_barrier_input_result_t
ML_Stage(ml_frame_barrier_t *barrier, uint32_t slot, uint32_t generation,
	uint64_t fingerprint)
{
	ml_barrier_slot_t *member;
	if (!ML_SlotValid(barrier, slot) || !fingerprint ||
		barrier->phase == ML_BARRIER_FAULT)
	{
		return ML_BARRIER_INPUT_REJECTED;
	}
	member = &barrier->slots[slot];
	if (!member->connected || !member->registered)
	{
		ML_Fault(barrier, ML_BARRIER_FAULT_ROSTER);
		return ML_BARRIER_INPUT_REJECTED;
	}
	if (member->command_ready)
	{
		if (member->command_generation == generation &&
			member->command_fingerprint == fingerprint)
		{
			return ML_BARRIER_INPUT_IDEMPOTENT;
		}
		ML_Fault(barrier, ML_BARRIER_FAULT_COMMAND_CONFLICT);
		return ML_BARRIER_INPUT_REJECTED;
	}
	member->command_generation = generation;
	member->command_fingerprint = fingerprint;
	member->command_ready = 1;
	return ML_BARRIER_INPUT_ACCEPTED;
}

ml_barrier_input_result_t
ML_FrameBarrierStageBootstrap(ml_frame_barrier_t *barrier, uint32_t slot,
	uint64_t fingerprint)
{
	if (!barrier || (barrier->phase != ML_BARRIER_WAIT_ROSTER &&
		barrier->phase != ML_BARRIER_WAIT_BOOTSTRAP))
	{
		return ML_BARRIER_INPUT_STALE;
	}
	return ML_Stage(barrier, slot, 0, fingerprint);
}

ml_barrier_input_result_t
ML_FrameBarrierStageAction(ml_frame_barrier_t *barrier, uint32_t slot,
	uint32_t generation, uint64_t fingerprint)
{
	uint32_t expected;
	if (!barrier || barrier->phase != ML_BARRIER_WAIT_ACTIONS)
	{
		return ML_BARRIER_INPUT_STALE;
	}
	expected = barrier->expected_action_tick %
		ML_FRAME_BARRIER_ACTION_GENERATIONS;
	if (generation != expected)
	{
		return ML_BARRIER_INPUT_STALE;
	}
	return ML_Stage(barrier, slot, generation, fingerprint);
}

ml_barrier_input_result_t
ML_FrameBarrierReady(ml_frame_barrier_t *barrier, uint32_t slot,
	uint32_t action_tick)
{
	ml_barrier_slot_t *member;
	if (!ML_SlotValid(barrier, slot) ||
		barrier->phase != ML_BARRIER_WAIT_ACTIONS)
	{
		return ML_BARRIER_INPUT_STALE;
	}
	member = &barrier->slots[slot];
	if (member->action_ready)
	{
		if (member->action_tick == action_tick)
		{
			return ML_BARRIER_INPUT_IDEMPOTENT;
		}
		ML_Fault(barrier, ML_BARRIER_FAULT_PROTOCOL);
		return ML_BARRIER_INPUT_REJECTED;
	}
	if (action_tick < barrier->expected_action_tick)
	{
		return ML_BARRIER_INPUT_STALE;
	}
	if (action_tick > barrier->expected_action_tick)
	{
		ML_Fault(barrier, ML_BARRIER_FAULT_FUTURE_READY);
		return ML_BARRIER_INPUT_REJECTED;
	}
	member->action_ready = 1;
	member->action_tick = action_tick;
	return ML_BARRIER_INPUT_ACCEPTED;
}

ml_barrier_gate_result_t
ML_FrameBarrierPoll(ml_frame_barrier_t *barrier, uint64_t now_ms)
{
	uint32_t slot;
	int complete = 1;
	if (!barrier || !barrier->enabled)
	{
		return ML_BARRIER_GATE_ACTIONS;
	}
	if (barrier->phase == ML_BARRIER_FAULT)
	{
		return ML_BARRIER_GATE_FAULT;
	}
	for (slot = 0; slot < barrier->configured_clients; slot++)
	{
		if (!barrier->slots[slot].connected ||
			!barrier->slots[slot].registered)
		{
			complete = 0;
		}
	}
	if (!complete)
	{
		barrier->phase = ML_BARRIER_WAIT_ROSTER;
	}
	else if (barrier->phase == ML_BARRIER_WAIT_ROSTER)
	{
		barrier->phase = ML_BARRIER_WAIT_BOOTSTRAP;
		barrier->hold_started_ms = now_ms ? now_ms : 1;
	}

	complete = 1;
	if (barrier->phase == ML_BARRIER_WAIT_BOOTSTRAP)
	{
		for (slot = 0; slot < barrier->configured_clients; slot++)
		{
			if (!barrier->slots[slot].command_ready)
			{
				complete = 0;
			}
		}
		if (complete)
		{
			return ML_BARRIER_GATE_BOOTSTRAP;
		}
	}
	else if (barrier->phase == ML_BARRIER_WAIT_ACTIONS)
	{
		for (slot = 0; slot < barrier->configured_clients; slot++)
		{
			if (!barrier->slots[slot].command_ready ||
				!barrier->slots[slot].action_ready)
			{
				complete = 0;
			}
		}
		if (complete)
		{
			return ML_BARRIER_GATE_ACTIONS;
		}
	}
	if (barrier->phase != ML_BARRIER_WAIT_ROSTER &&
		barrier->hold_started_ms && now_ms >= barrier->hold_started_ms &&
		now_ms - barrier->hold_started_ms >= barrier->timeout_ms)
	{
		ML_Fault(barrier, ML_BARRIER_FAULT_TIMEOUT);
		return ML_BARRIER_GATE_FAULT;
	}
	return ML_BARRIER_GATE_HOLD;
}

void
ML_FrameBarrierCommitted(ml_frame_barrier_t *barrier, uint32_t server_frame,
	uint64_t now_ms)
{
	if (!barrier || !barrier->enabled || barrier->phase == ML_BARRIER_FAULT)
	{
		return;
	}
	ML_ClearTransactions(barrier);
	barrier->expected_action_tick = server_frame;
	barrier->hold_started_ms = now_ms ? now_ms : 1;
	barrier->phase = ML_BARRIER_WAIT_ACTIONS;
}

const char *
ML_FrameBarrierFaultName(ml_barrier_fault_t fault)
{
	switch (fault)
	{
		case ML_BARRIER_FAULT_NONE: return "none";
		case ML_BARRIER_FAULT_CONFIGURATION: return "configuration";
		case ML_BARRIER_FAULT_ROSTER: return "roster";
		case ML_BARRIER_FAULT_DISCONNECT: return "disconnect";
		case ML_BARRIER_FAULT_FUTURE_READY: return "future-ready";
		case ML_BARRIER_FAULT_COMMAND_CONFLICT: return "command-conflict";
		case ML_BARRIER_FAULT_TIMEOUT: return "timeout";
		case ML_BARRIER_FAULT_PROTOCOL: return "protocol";
		default: return "unknown";
	}
}
