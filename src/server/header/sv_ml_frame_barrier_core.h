/* Pure deterministic state machine for isolated network-client training. */
#ifndef SV_ML_FRAME_BARRIER_CORE_H
#define SV_ML_FRAME_BARRIER_CORE_H

#include <stdint.h>

#define ML_FRAME_BARRIER_VERSION 1u
#define ML_FRAME_BARRIER_CAPABILITY 0x00000001u
#define ML_FRAME_BARRIER_MAX_CLIENTS 64u
#define ML_FRAME_BARRIER_ACTION_GENERATIONS 192u

typedef enum {
	ML_BARRIER_DISABLED = 0,
	ML_BARRIER_WAIT_ROSTER,
	ML_BARRIER_WAIT_BOOTSTRAP,
	ML_BARRIER_WAIT_ACTIONS,
	ML_BARRIER_FAULT
} ml_barrier_phase_t;

typedef enum {
	ML_BARRIER_FAULT_NONE = 0,
	ML_BARRIER_FAULT_CONFIGURATION,
	ML_BARRIER_FAULT_ROSTER,
	ML_BARRIER_FAULT_DISCONNECT,
	ML_BARRIER_FAULT_FUTURE_READY,
	ML_BARRIER_FAULT_COMMAND_CONFLICT,
	ML_BARRIER_FAULT_TIMEOUT,
	ML_BARRIER_FAULT_PROTOCOL
} ml_barrier_fault_t;

typedef enum {
	ML_BARRIER_INPUT_ACCEPTED = 0,
	ML_BARRIER_INPUT_IDEMPOTENT,
	ML_BARRIER_INPUT_STALE,
	ML_BARRIER_INPUT_REJECTED
} ml_barrier_input_result_t;

typedef enum {
	ML_BARRIER_GATE_HOLD = 0,
	ML_BARRIER_GATE_BOOTSTRAP,
	ML_BARRIER_GATE_ACTIONS,
	ML_BARRIER_GATE_FAULT
} ml_barrier_gate_result_t;

typedef struct {
	uint8_t connected;
	uint8_t registered;
	uint8_t command_ready;
	uint8_t action_ready;
	uint32_t command_generation;
	uint32_t action_tick;
	uint64_t command_fingerprint;
} ml_barrier_slot_t;

typedef struct {
	uint32_t enabled;
	uint32_t configured_clients;
	uint32_t timeout_ms;
	uint32_t map_epoch;
	uint32_t expected_action_tick;
	uint64_t hold_started_ms;
	ml_barrier_phase_t phase;
	ml_barrier_fault_t fault;
	ml_barrier_slot_t slots[ML_FRAME_BARRIER_MAX_CLIENTS];
} ml_frame_barrier_t;

void ML_FrameBarrierInit(ml_frame_barrier_t *barrier, int enabled,
	uint32_t configured_clients, uint32_t timeout_ms);
void ML_FrameBarrierResetMap(ml_frame_barrier_t *barrier);
ml_barrier_input_result_t ML_FrameBarrierConnect(ml_frame_barrier_t *barrier,
	uint32_t slot, uint32_t suffix, uint64_t now_ms);
ml_barrier_input_result_t ML_FrameBarrierRegister(ml_frame_barrier_t *barrier,
	uint32_t slot);
void ML_FrameBarrierDisconnect(ml_frame_barrier_t *barrier, uint32_t slot);
void ML_FrameBarrierDeath(ml_frame_barrier_t *barrier, uint32_t slot);
void ML_FrameBarrierProtocolFault(ml_frame_barrier_t *barrier);
ml_barrier_input_result_t ML_FrameBarrierStageBootstrap(
	ml_frame_barrier_t *barrier, uint32_t slot, uint64_t fingerprint);
ml_barrier_input_result_t ML_FrameBarrierStageAction(
	ml_frame_barrier_t *barrier, uint32_t slot, uint32_t generation,
	uint64_t fingerprint);
ml_barrier_input_result_t ML_FrameBarrierReady(ml_frame_barrier_t *barrier,
	uint32_t slot, uint32_t action_tick);
ml_barrier_gate_result_t ML_FrameBarrierPoll(ml_frame_barrier_t *barrier,
	uint64_t now_ms);
void ML_FrameBarrierCommitted(ml_frame_barrier_t *barrier,
	uint32_t server_frame, uint64_t now_ms);
const char *ML_FrameBarrierFaultName(ml_barrier_fault_t fault);

#endif
