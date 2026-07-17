#include "header/sv_ml_frame_barrier.h"

#include <errno.h>
#include <stdlib.h>

#define ML_HARNESS_IMPULSE_BASE 16u
#define ML_HARNESS_ACTION_COUNT 40u
#define ML_HARNESS_BUTTON_GENERATION_SHIFT 2u
#define ML_HARNESS_BUTTON_GENERATION_MASK 0x7cu
#define ML_FRAME_BARRIER_TEST_FAULTS_V1 \
	"ML_FRAME_BARRIER_TEST_FAULTS_V1=baseline,duplicate,stale,future," \
	"brief-drop,sustained-drop,conflict,death,same-life-hold,epoch-drain," \
	"drain-sigkill,load-delay,old-telemetry"

static cvar_t *sv_ml_frame_barrier;
static cvar_t *sv_ml_frame_barrier_clients;
static cvar_t *sv_ml_frame_barrier_timeout_ms;
static cvar_t *ml_frame_barrier_epoch_drain;
static cvar_t *sv_ml_frame_barrier_test_mode;
static cvar_t *sv_ml_frame_barrier_test_fault;
static cvar_t *sv_ml_frame_barrier_test_tick;
static cvar_t *sv_ml_frame_barrier_test_map;
static ml_frame_barrier_t ml_barrier;
static usercmd_t ml_staged_commands[ML_FRAME_BARRIER_MAX_CLIENTS];
static uint32_t ml_map_epoch;
static uint64_t ml_map_reset_time;
static qboolean ml_load_delay_reported;
static const char ml_test_fault_vocabulary[] =
	ML_FRAME_BARRIER_TEST_FAULTS_V1;
static uint32_t ml_sustained_command_logged[ML_FRAME_BARRIER_MAX_CLIENTS];
static uint32_t ml_sustained_ready_logged[ML_FRAME_BARRIER_MAX_CLIENTS];

static const char *
ML_InputResultName(ml_barrier_input_result_t result)
{
	switch (result)
	{
		case ML_BARRIER_INPUT_ACCEPTED: return "accepted";
		case ML_BARRIER_INPUT_IDEMPOTENT: return "idempotent";
		case ML_BARRIER_INPUT_STALE: return "stale";
		case ML_BARRIER_INPUT_REJECTED: return "rejected";
		default: return "unknown";
	}
}

static qboolean
ML_TestScenario(const char *name, uint32_t tick)
{
	return sv_ml_frame_barrier_test_mode->value &&
		(uint32_t)sv_ml_frame_barrier_test_tick->value == tick &&
		!Q_stricmp(sv_ml_frame_barrier_test_fault->string, name);
}

static qboolean
ML_TestFaultKnown(void)
{
	static const char *known[] = {
		"", "baseline", "duplicate", "stale", "future", "brief-drop",
		"sustained-drop", "conflict", "death", "same-life-hold",
		"epoch-drain", "drain-sigkill", "load-delay", "old-telemetry"
	};
	size_t index;
	for (index = 0; index < sizeof(known) / sizeof(known[0]); index++)
		if (!Q_stricmp(sv_ml_frame_barrier_test_fault->string, known[index]))
			return true;
	return false;
}

static qboolean
ML_TestFaultIs(const char *name)
{
	return sv_ml_frame_barrier_test_mode->value &&
		!Q_stricmp(sv_ml_frame_barrier_test_fault->string, name);
}

static uint64_t
ML_Now(void)
{
	return (uint64_t)(unsigned int)svs.realtime;
}

static int
ML_ClientSlot(const client_t *client)
{
	return client && svs.clients ? (int)(client - svs.clients) : -1;
}

static qboolean
ML_ParseUnsigned(const char *text, uint32_t *value)
{
	char *end;
	unsigned long parsed;
	if (!text || !text[0] || !value)
	{
		return false;
	}
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || *end || parsed > 0xfffffffful)
	{
		return false;
	}
	*value = (uint32_t)parsed;
	return true;
}

static qboolean
ML_ParseSuffix(const char *client_id, uint32_t *suffix)
{
	const char *dash;
	if (!client_id || !client_id[0])
	{
		return false;
	}
	dash = strrchr(client_id, '-');
	return dash && dash[1] && ML_ParseUnsigned(dash + 1, suffix);
}

static qboolean
ML_IdentityValid(const char *userinfo, int slot)
{
	uint32_t suffix, version, capability;
	char client_id[MAX_INFO_VALUE];
	char barrier[MAX_INFO_VALUE];
	char version_text[MAX_INFO_VALUE];
	char capability_text[MAX_INFO_VALUE];

	/* Info_ValueForKey alternates two static buffers.  Every value used after
	 * a later lookup must be copied immediately or the next lookup can silently
	 * replace it (which previously turned the saved client id into "1"). */
	Q_strlcpy(client_id, Info_ValueForKey(userinfo, "ml_client_id"),
		sizeof(client_id));
	Q_strlcpy(barrier, Info_ValueForKey(userinfo, "ml_frame_barrier"),
		sizeof(barrier));
	Q_strlcpy(version_text, Info_ValueForKey(userinfo,
		"ml_frame_barrier_version"), sizeof(version_text));
	Q_strlcpy(capability_text, Info_ValueForKey(userinfo,
		"ml_frame_barrier_capability"), sizeof(capability_text));
	if (strcmp(barrier, "1") ||
		!ML_ParseUnsigned(version_text, &version) ||
		!ML_ParseUnsigned(capability_text, &capability) ||
		version != ML_FRAME_BARRIER_VERSION ||
		(capability & ML_FRAME_BARRIER_CAPABILITY) == 0 ||
		!ML_ParseSuffix(client_id, &suffix) || suffix != (uint32_t)slot)
	{
		return false;
	}
	return true;
}

static void
ML_CopyAdmissionLogValue(const char *userinfo, const char *key,
	char *destination, size_t destination_size)
{
	const char *source = Info_ValueForKey(userinfo, key);
	size_t index;
	if (!destination_size)
	{
		return;
	}
	for (index = 0; source[index] && index + 1 < destination_size; index++)
	{
		char value = source[index];
		destination[index] = ((value >= 'a' && value <= 'z') ||
			(value >= 'A' && value <= 'Z') ||
			(value >= '0' && value <= '9') || value == '-' ||
			value == '_' || value == '.') ? value : '_';
	}
	destination[index] = '\0';
}

static uint64_t
ML_CommandFingerprint(const usercmd_t *command)
{
	const unsigned char *bytes = (const unsigned char *)command;
	uint64_t hash = UINT64_C(1469598103934665603);
	size_t index;
	for (index = 0; index < sizeof(*command); index++)
	{
		hash ^= bytes[index];
		hash *= UINT64_C(1099511628211);
	}
	return hash ? hash : 1;
}

static qboolean
ML_IsNeutral(const usercmd_t *command)
{
	return command && command->buttons == 0 && command->forwardmove == 0 &&
		command->sidemove == 0 && command->upmove == 0 &&
		command->impulse == 0;
}

static qboolean
ML_CommandGeneration(const usercmd_t *command, uint32_t *generation)
{
	uint32_t encoded, high, low;
	if (!command || !generation || command->impulse < ML_HARNESS_IMPULSE_BASE)
	{
		return false;
	}
	encoded = (uint32_t)command->impulse - ML_HARNESS_IMPULSE_BASE;
	if (encoded >= 240u)
	{
		return false;
	}
	high = encoded / ML_HARNESS_ACTION_COUNT;
	low = ((uint32_t)command->buttons & ML_HARNESS_BUTTON_GENERATION_MASK) >>
		ML_HARNESS_BUTTON_GENERATION_SHIFT;
	*generation = high * 32u + low;
	return true;
}

static ml_barrier_input_result_t
ML_StageCandidate(int slot, const usercmd_t *candidate)
{
	uint32_t generation;
	usercmd_t sanitized;
	ml_barrier_input_result_t result;
	if (!candidate || slot < 0 || slot >= (int)ml_barrier.configured_clients)
	{
		return ML_BARRIER_INPUT_STALE;
	}
	sanitized = *candidate;
	sanitized.msec = 100;
	sanitized.lightlevel = 0;
	if (ml_barrier.phase == ML_BARRIER_WAIT_ROSTER ||
		ml_barrier.phase == ML_BARRIER_WAIT_BOOTSTRAP)
	{
		if (!ML_IsNeutral(candidate))
		{
			return ML_BARRIER_INPUT_STALE;
		}
		result = ML_FrameBarrierStageBootstrap(&ml_barrier, (uint32_t)slot,
			ML_CommandFingerprint(&sanitized));
	}
	else
	{
		if (!ML_CommandGeneration(candidate, &generation))
		{
			return ML_BARRIER_INPUT_STALE;
		}
		result = ML_FrameBarrierStageAction(&ml_barrier, (uint32_t)slot,
			generation, ML_CommandFingerprint(&sanitized));
	}
	if (result == ML_BARRIER_INPUT_ACCEPTED)
	{
		ml_staged_commands[slot] = sanitized;
	}
	return result;
}

void
SV_MLFrameBarrierInit(void)
{
	sv_ml_frame_barrier = Cvar_Get("sv_ml_frame_barrier", "0", CVAR_LATCH);
	sv_ml_frame_barrier_clients = Cvar_Get("sv_ml_frame_barrier_clients", "0",
		CVAR_LATCH);
	sv_ml_frame_barrier_timeout_ms = Cvar_Get(
		"sv_ml_frame_barrier_timeout_ms", "5000", CVAR_LATCH);
	ml_frame_barrier_epoch_drain = Cvar_Get(
		"ml_frame_barrier_epoch_drain", "0", 0);
	Cvar_Get("ml_frame_barrier_map_epoch", "0", CVAR_NOSET);
	sv_ml_frame_barrier_test_mode = Cvar_Get(
		"sv_ml_frame_barrier_test_mode", "0", 0);
	sv_ml_frame_barrier_test_fault = Cvar_Get(
		"sv_ml_frame_barrier_test_fault", "", 0);
	sv_ml_frame_barrier_test_tick = Cvar_Get(
		"sv_ml_frame_barrier_test_tick", "0", 0);
	sv_ml_frame_barrier_test_map = Cvar_Get(
		"sv_ml_frame_barrier_test_map", "q2dm2", 0);
	ML_FrameBarrierInit(&ml_barrier, 0, 0, 0);
	memset(ml_staged_commands, 0, sizeof(ml_staged_commands));
	memset(ml_sustained_command_logged, 0xff,
		sizeof(ml_sustained_command_logged));
	memset(ml_sustained_ready_logged, 0xff,
		sizeof(ml_sustained_ready_logged));
}

qboolean
SV_MLFrameBarrierEnabled(void)
{
	return ml_barrier.enabled &&
		(!ml_frame_barrier_epoch_drain->value ||
		 ml_barrier.phase == ML_BARRIER_FAULT) ?
		true : false;
}

qboolean
SV_MLFrameBarrierModeEnabled(void)
{
	return ml_barrier.enabled ? true : false;
}

qboolean
SV_MLFrameBarrierEpochDrain(void)
{
	return ml_barrier.enabled && ml_frame_barrier_epoch_drain->value ?
		true : false;
}

void
SV_MLFrameBarrierResetMap(void)
{
	uint32_t count = (uint32_t)sv_ml_frame_barrier_clients->value;
	uint32_t timeout_ms = (uint32_t)sv_ml_frame_barrier_timeout_ms->value;
	int slot;
	ML_FrameBarrierInit(&ml_barrier, sv_ml_frame_barrier->value != 0,
		count, timeout_ms);
	Cvar_Set("ml_frame_barrier_epoch_drain", "0");
	memset(ml_staged_commands, 0, sizeof(ml_staged_commands));
	memset(ml_sustained_command_logged, 0xff,
		sizeof(ml_sustained_command_logged));
	memset(ml_sustained_ready_logged, 0xff,
		sizeof(ml_sustained_ready_logged));
	if (!ml_barrier.enabled)
	{
		return;
	}
	/* Qualification controls are configuration inputs only.  The private cfg
	 * is allowed to set them before its map command; the first map load seals
	 * them against console, rcon, game-module, and later cfg mutation. */
	sv_ml_frame_barrier_test_mode->flags |= CVAR_NOSET;
	sv_ml_frame_barrier_test_fault->flags |= CVAR_NOSET;
	sv_ml_frame_barrier_test_tick->flags |= CVAR_NOSET;
	sv_ml_frame_barrier_test_map->flags |= CVAR_NOSET;
	ml_map_epoch++;
	ml_map_reset_time = ML_Now();
	ml_load_delay_reported = false;
	Cvar_ForceSet("ml_frame_barrier_map_epoch", va("%u", ml_map_epoch));
	if (sv_ml_frame_barrier_test_mode->value &&
		(Cvar_VariableValue("dedicated") != 1 ||
		 (int)count != (int)maxclients->value || !ML_TestFaultKnown()))
	{
		Com_Printf("ML_FRAME_BARRIER_EVENT event=test_controls_reject "
			"fault=%s dedicated=%d clients=%u maxclients=%d known=%d\n",
			sv_ml_frame_barrier_test_fault->string,
			(int)Cvar_VariableValue("dedicated"), count,
			(int)maxclients->value, ML_TestFaultKnown());
		ML_FrameBarrierProtocolFault(&ml_barrier);
		return;
	}
	if (sv_ml_frame_barrier_test_mode->value)
	{
		Com_Printf("ML_FRAME_BARRIER_EVENT event=test_mode_sealed value=1 "
			"dedicated=1 roster=exact controls_immutable=1 "
			"fault_vocabulary=v1 marker=%s\n", ml_test_fault_vocabulary);
	}
	if ((int)count != (int)maxclients->value ||
		Cvar_VariableValue("ml_async") != 0)
	{
		ML_FrameBarrierProtocolFault(&ml_barrier);
		return;
	}
	Com_Printf("ML_FRAME_BARRIER_EVENT event=map_reset map=%s map_epoch=%u "
		"clients=%u timeout_ms=%u wire=8 barrier=1 capability=1 "
		"test_mode=%d test_fault=%s test_tick=%d test_map=%s "
		"test_controls_sealed=1 dedicated=%d\n", sv.name, ml_map_epoch,
		count, timeout_ms, (int)sv_ml_frame_barrier_test_mode->value,
		sv_ml_frame_barrier_test_fault->string,
		(int)sv_ml_frame_barrier_test_tick->value,
		Cvar_VariableString("sv_ml_frame_barrier_test_map"),
		(int)Cvar_VariableValue("dedicated"));
	for (slot = 0; slot < (int)count; slot++)
	{
		if (svs.clients[slot].state >= cs_connected)
		{
			if (!ML_IdentityValid(svs.clients[slot].userinfo, slot))
			{
				ML_FrameBarrierProtocolFault(&ml_barrier);
				return;
			}
			ML_FrameBarrierConnect(&ml_barrier, (uint32_t)slot,
				(uint32_t)slot, ML_Now());
		}
	}
}

qboolean
SV_MLFrameBarrierAdmit(const char *userinfo, int slot, char *reason,
	size_t reason_size)
{
	char client_id[MAX_INFO_VALUE], barrier[MAX_INFO_VALUE];
	char version[MAX_INFO_VALUE], capability[MAX_INFO_VALUE];
	if (!ml_barrier.enabled)
	{
		return true;
	}
	if (slot < 0 || slot >= (int)ml_barrier.configured_clients ||
		!ML_IdentityValid(userinfo, slot))
	{
		ML_CopyAdmissionLogValue(userinfo, "ml_client_id", client_id,
			sizeof(client_id));
		ML_CopyAdmissionLogValue(userinfo, "ml_frame_barrier", barrier,
			sizeof(barrier));
		ML_CopyAdmissionLogValue(userinfo, "ml_frame_barrier_version",
			version, sizeof(version));
		ML_CopyAdmissionLogValue(userinfo, "ml_frame_barrier_capability",
			capability, sizeof(capability));
		Com_Printf("ML_FRAME_BARRIER_EVENT event=admission_reject slot=%d "
			"reason=roster_or_capability client_id=%s barrier=%s "
			"version=%s capability=%s expected_slot=%d\n", slot,
			client_id, barrier, version, capability, slot);
		Q_strlcpy(reason, "isolated ML frame barrier roster mismatch",
			reason_size);
		return false;
	}
	return true;
}

void
SV_MLFrameBarrierConnected(client_t *client)
{
	int slot;
	if (!ml_barrier.enabled)
	{
		return;
	}
	slot = ML_ClientSlot(client);
	ML_FrameBarrierConnect(&ml_barrier, (uint32_t)slot, (uint32_t)slot,
		ML_Now());
	Com_Printf("ML_FRAME_BARRIER_EVENT event=connected slot=%d\n", slot);
}

void
SV_MLFrameBarrierDisconnected(client_t *client, const char *reason)
{
	int slot;
	if (!SV_MLFrameBarrierModeEnabled())
	{
		return;
	}
	slot = ML_ClientSlot(client);
	if (slot >= 0)
	{
		Com_Printf("ML_FRAME_BARRIER_EVENT event=disconnect slot=%d frame=%d "
			"reason=%s\n", slot, sv.framenum,
			reason && reason[0] ? reason : "engine-drop");
		ML_FrameBarrierDisconnect(&ml_barrier, (uint32_t)slot);
	}
}

void
SV_MLFrameBarrierDrainMove(client_t *client, const usercmd_t *newcmd)
{
	usercmd_t neutral;
	int slot = ML_ClientSlot(client);
	if (!SV_MLFrameBarrierEpochDrain() || !newcmd || slot < 0 ||
		slot >= (int)ml_barrier.configured_clients)
		return;
	memset(&neutral, 0, sizeof(neutral));
	neutral.msec = newcmd->msec;
	neutral.lightlevel = newcmd->lightlevel;
	neutral.angles[0] = ml_staged_commands[slot].angles[0];
	neutral.angles[1] = ml_staged_commands[slot].angles[1];
	neutral.angles[2] = ml_staged_commands[slot].angles[2];
	SV_ApplyClientCommand(client, &neutral);
	if (sv_ml_frame_barrier_test_mode->value)
		Com_Printf("ML_FRAME_BARRIER_EVENT event=epoch_drain_neutral slot=%d "
			"server_frame=%d msec=%u\n", slot, sv.framenum,
			(unsigned)neutral.msec);
}

void
SV_MLFrameBarrierValidateClient(client_t *client)
{
	int slot;
	if (!SV_MLFrameBarrierModeEnabled() || !client)
		return;
	slot = ML_ClientSlot(client);
	if (slot < 0 || !ML_IdentityValid(client->userinfo, slot))
	{
		Com_Printf("ML_FRAME_BARRIER_EVENT event=identity_fault slot=%d\n",
			slot);
		ML_FrameBarrierProtocolFault(&ml_barrier);
	}
}

void
SV_MLFrameBarrierValidateLiveness(void)
{
	uint32_t slot;
	uint64_t now;
	if (!SV_MLFrameBarrierModeEnabled() ||
		ml_barrier.phase == ML_BARRIER_WAIT_ROSTER ||
		ml_barrier.phase == ML_BARRIER_FAULT)
		return;
	now = ML_Now();
	for (slot = 0; slot < ml_barrier.configured_clients; slot++)
	{
		client_t *client = &svs.clients[slot];
		if (!ml_barrier.slots[slot].connected ||
			!ml_barrier.slots[slot].registered)
			continue;
		if (client->state < cs_connected ||
			(now >= (uint64_t)(unsigned int)client->lastmessage &&
			 now - (uint64_t)(unsigned int)client->lastmessage >=
				ml_barrier.timeout_ms))
		{
			Com_Printf("ML_FRAME_BARRIER_EVENT event=disconnect slot=%u "
				"frame=%d reason=liveness\n", slot, sv.framenum);
			ML_FrameBarrierDisconnect(&ml_barrier, slot);
			return;
		}
	}
}

void
SV_MLFrameBarrierStageMove(client_t *client, const usercmd_t *oldest,
	const usercmd_t *oldcmd, const usercmd_t *newcmd, int net_drop)
{
	int slot;
	ml_barrier_input_result_t result;
	if (!SV_MLFrameBarrierEnabled())
	{
		return;
	}
	slot = ML_ClientSlot(client);
	if (slot < 0 || client->state != cs_spawned)
	{
		ML_FrameBarrierProtocolFault(&ml_barrier);
		return;
	}
	if (!ml_barrier.slots[slot].registered)
	{
		return;
	}
	if (ML_TestScenario("sustained-drop", ml_barrier.expected_action_tick))
	{
		if (ml_sustained_command_logged[slot] !=
			ml_barrier.expected_action_tick)
		{
			ml_sustained_command_logged[slot] =
				ml_barrier.expected_action_tick;
			Com_Printf("ML_FRAME_BARRIER_EVENT event=sustained_drop "
				"slot=%d tick=%u\n", slot,
				ml_barrier.expected_action_tick);
		}
		return;
	}
	if (ML_TestScenario("brief-drop", ml_barrier.expected_action_tick))
	{
		result = ML_StageCandidate(slot, oldcmd);
		if (result != ML_BARRIER_INPUT_ACCEPTED &&
			result != ML_BARRIER_INPUT_IDEMPOTENT)
			result = ML_StageCandidate(slot, oldest);
		Com_Printf("ML_FRAME_BARRIER_EVENT event=brief_drop_recovery slot=%d "
			"tick=%u result=%s source=command_triple\n", slot,
			ml_barrier.expected_action_tick, ML_InputResultName(result));
		/* Deliberately discard the newest command. A later ordinary packet can
		 * recover it only through the protocol-34 command triple. */
		return;
	}
	if (ML_TestScenario("stale", ml_barrier.expected_action_tick))
	{
		usercmd_t sanitized = *newcmd;
		ml_barrier_input_result_t stale;
		sanitized.msec = 100;
		sanitized.lightlevel = 0;
		stale = ML_FrameBarrierStageAction(&ml_barrier, (uint32_t)slot,
			(ml_barrier.expected_action_tick +
				ML_FRAME_BARRIER_ACTION_GENERATIONS - 1) %
				ML_FRAME_BARRIER_ACTION_GENERATIONS,
			ML_CommandFingerprint(&sanitized));
		Com_Printf("ML_FRAME_BARRIER_EVENT event=stale_command slot=%d "
			"tick=%u result=%s\n", slot, ml_barrier.expected_action_tick,
			ML_InputResultName(stale));
	}
	result = ML_StageCandidate(slot, newcmd);
	if (result == ML_BARRIER_INPUT_ACCEPTED &&
		ML_TestScenario("duplicate", ml_barrier.expected_action_tick))
	{
		ml_barrier_input_result_t duplicate = ML_StageCandidate(slot, newcmd);
		Com_Printf("ML_FRAME_BARRIER_EVENT event=duplicate_command slot=%d "
			"tick=%u result=%s\n", slot, ml_barrier.expected_action_tick,
			ML_InputResultName(duplicate));
	}
	if (result == ML_BARRIER_INPUT_ACCEPTED &&
		ML_TestScenario("conflict", ml_barrier.expected_action_tick))
	{
		ml_barrier_input_result_t conflict = ML_FrameBarrierStageAction(
			&ml_barrier, (uint32_t)slot,
			ml_barrier.expected_action_tick %
				ML_FRAME_BARRIER_ACTION_GENERATIONS,
			ml_barrier.slots[slot].command_fingerprint ^ UINT64_C(1));
		Com_Printf("ML_FRAME_BARRIER_EVENT event=conflicting_command slot=%d "
			"tick=%u result=%s\n", slot, ml_barrier.expected_action_tick,
			ML_InputResultName(conflict));
	}
	if (result == ML_BARRIER_INPUT_ACCEPTED ||
		result == ML_BARRIER_INPUT_IDEMPOTENT || net_drop <= 0)
	{
		return;
	}
	result = ML_StageCandidate(slot, oldcmd);
	if (result == ML_BARRIER_INPUT_ACCEPTED ||
		result == ML_BARRIER_INPUT_IDEMPOTENT || net_drop <= 1)
	{
		return;
	}
	ML_StageCandidate(slot, oldest);
}

qboolean
SV_MLFrameBarrierStringCommand(client_t *client, const char *text)
{
	unsigned int version, tick, epoch, telemetry_frame;
	char trailing;
	int slot;
	const char *configured_id;
	if (!SV_MLFrameBarrierModeEnabled())
	{
		return false;
	}
	if (SV_MLFrameBarrierEpochDrain())
	{
		if (!strncmp(text, "disconnect", 10))
			return false;
		if (client->state != cs_spawned)
			return false;
		if (sv_ml_frame_barrier_test_mode->value)
			Com_Printf("ML_FRAME_BARRIER_EVENT event=epoch_drain_string_swallowed "
				"slot=%d\n", ML_ClientSlot(client));
		return true;
	}
	if (!strncmp(text, "ml_barrier_ready", 16))
	{
		slot = ML_ClientSlot(client);
		if (sscanf(text, "ml_barrier_ready %u %u %c", &version, &tick,
			&trailing) != 2 || version != ML_FRAME_BARRIER_VERSION || slot < 0)
		{
			ML_FrameBarrierProtocolFault(&ml_barrier);
		}
		else
		{
			ml_barrier_input_result_t ready;
			if (ML_TestScenario("sustained-drop", tick))
			{
				if (ml_sustained_ready_logged[slot] != tick)
				{
					ml_sustained_ready_logged[slot] = tick;
					Com_Printf("ML_FRAME_BARRIER_EVENT "
						"event=sustained_drop_ready slot=%d tick=%u\n",
						slot, tick);
				}
				return true;
			}
			if (ML_TestScenario("future", tick))
			{
				ready = ML_FrameBarrierReady(&ml_barrier, (uint32_t)slot,
					tick + 1);
				Com_Printf("ML_FRAME_BARRIER_EVENT event=future_ready slot=%d "
					"tick=%u injected=%u result=%s\n", slot, tick, tick + 1,
					ML_InputResultName(ready));
				return true;
			}
			if (ML_TestScenario("stale", tick))
			{
				ready = ML_FrameBarrierReady(&ml_barrier, (uint32_t)slot,
					tick ? tick - 1 : 0);
				Com_Printf("ML_FRAME_BARRIER_EVENT event=stale_ready slot=%d "
					"tick=%u injected=%u result=%s\n", slot, tick,
					tick ? tick - 1 : 0, ML_InputResultName(ready));
			}
			ready = ML_FrameBarrierReady(&ml_barrier, (uint32_t)slot, tick);
			if (ML_TestScenario("duplicate", tick))
			{
				ml_barrier_input_result_t duplicate = ML_FrameBarrierReady(
					&ml_barrier, (uint32_t)slot, tick);
				Com_Printf("ML_FRAME_BARRIER_EVENT event=duplicate_ready slot=%d "
					"tick=%u result=%s\n", slot, tick,
					ML_InputResultName(duplicate));
			}
		}
		return true;
	}
	if (!strncmp(text, "ml_barrier_bootstrap_ready", 26))
	{
		char client_id[40];
		slot = ML_ClientSlot(client);
		configured_id = Info_ValueForKey(client->userinfo, "ml_client_id");
		if (sscanf(text, "ml_barrier_bootstrap_ready %u %39s %c", &version,
			client_id, &trailing) != 2 ||
			version != ML_FRAME_BARRIER_VERSION || slot < 0 ||
			strcmp(client_id, configured_id))
		{
			ML_FrameBarrierProtocolFault(&ml_barrier);
		}
		else
		{
			ML_FrameBarrierRegister(&ml_barrier, (uint32_t)slot);
			Com_Printf("ML_FRAME_BARRIER_EVENT event=bootstrap_ready slot=%d "
				"client_id=%s\n", slot, client_id);
		}
		return true;
	}
	if (!strncmp(text, "ml_barrier_old_telemetry_discarded", 34))
	{
		slot = ML_ClientSlot(client);
		if (sscanf(text, "ml_barrier_old_telemetry_discarded %u %u %u %c",
			&version, &epoch, &telemetry_frame, &trailing) != 3 ||
			version != ML_FRAME_BARRIER_VERSION || slot < 0 ||
			epoch != ml_map_epoch || telemetry_frame >= (uint32_t)sv.framenum)
		{
			ML_FrameBarrierProtocolFault(&ml_barrier);
		}
		else
		{
			Com_Printf("ML_FRAME_BARRIER_EVENT event=old_telemetry "
				"slot=%d replay_frame=%u accepted_frame=%d "
				"result=discarded clock_regressed=0\n", slot,
				telemetry_frame, sv.framenum);
		}
		return true;
	}
	/* These side effects are represented in the staged usercmd identity and
	 * are intentionally never executed at packet-arrival time. */
	if (!strncmp(text, "hook", 4) || !strncmp(text, "unhook", 6) ||
		!strncmp(text, "use ", 4))
	{
		ML_FrameBarrierProtocolFault(&ml_barrier);
		return true;
	}
	if (client->state == cs_spawned && strncmp(text, "disconnect", 10))
	{
		ML_FrameBarrierProtocolFault(&ml_barrier);
		return true;
	}
	return false;
}

ml_barrier_gate_result_t
SV_MLFrameBarrierPoll(void)
{
	ml_barrier_phase_t phase = ml_barrier.phase;
	ml_barrier_gate_result_t result;
	if (SV_MLFrameBarrierEnabled() && Cvar_VariableValue("ml_async") != 0)
	{
		ML_FrameBarrierProtocolFault(&ml_barrier);
	}
	result = ML_FrameBarrierPoll(&ml_barrier, ML_Now());
	if (!ml_load_delay_reported && ML_TestFaultIs("load-delay") &&
		phase == ML_BARRIER_WAIT_ROSTER &&
		ml_barrier.phase == ML_BARRIER_WAIT_BOOTSTRAP)
	{
		uint64_t load_ms = ML_Now() - ml_map_reset_time;
		ml_load_delay_reported = true;
		if (load_ms > ml_barrier.timeout_ms)
		{
			Com_Printf("ML_FRAME_BARRIER_EVENT event=bootstrap_load_delay "
				"result=recovered timeout_started=0 load_ms=%u\n",
				(unsigned int)load_ms);
		}
		else
		{
			Com_Printf("ML_FRAME_BARRIER_EVENT event=bootstrap_load_delay "
				"result=not_exercised timeout_started=0 load_ms=%u\n",
				(unsigned int)load_ms);
			ML_FrameBarrierProtocolFault(&ml_barrier);
			return ML_BARRIER_GATE_FAULT;
		}
	}
	return result;
}

void
SV_MLFrameBarrierApplyCommands(void)
{
	uint32_t slot;
	for (slot = 0; slot < ml_barrier.configured_clients; slot++)
	{
		Com_Printf("ML_FRAME_BARRIER_EVENT event=apply slot=%u action_tick=%u "
			"server_frame=%d msec=%u\n", slot,
			ml_barrier.phase == ML_BARRIER_WAIT_BOOTSTRAP ? 0 :
				ml_barrier.expected_action_tick,
			sv.framenum, (unsigned)ml_staged_commands[slot].msec);
		SV_ApplyClientCommand(&svs.clients[slot], &ml_staged_commands[slot]);
	}
}

void
SV_MLFrameBarrierCommitted(void)
{
	qboolean bootstrap = ml_barrier.phase == ML_BARRIER_WAIT_BOOTSTRAP;
	uint32_t action_tick = bootstrap ? 0 : ml_barrier.expected_action_tick;
	ML_FrameBarrierCommitted(&ml_barrier, (uint32_t)sv.framenum, ML_Now());
	Com_Printf("ML_FRAME_BARRIER_EVENT event=%s action_tick=%u "
		"server_frame=%d map_epoch=%u\n",
		bootstrap ? "bootstrap_commit" : "action_commit", action_tick,
		sv.framenum, ml_map_epoch);
}

const char *
SV_MLFrameBarrierFault(void)
{
	return ML_FrameBarrierFaultName(ml_barrier.fault);
}
