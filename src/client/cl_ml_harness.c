/* Network-native ML client integration.
 *
 * This remains a normal protocol-34 client.  Policy actions become ordinary
 * usercmd_t messages.  Privileged server observations arrive on a separate,
 * authenticated conduit and are forwarded only to this client's local
 * harness endpoint.
 */
#include "header/ml_harness.h"
#include "header/ml_harness_role_admission.h"
#include "header/ml_harness_view_rebase.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ML_CLIENT_WIRE_VERSION 8u
#define ML_CLIENT_FRAME_BARRIER_VERSION 1u
#define ML_CLIENT_FRAME_BARRIER_CAPABILITY 0x00000001u
#define ML_CLIENT_REGISTER_MAGIC 0x52434d51u
#define ML_CLIENT_ACK_MAGIC      0x41434d51u
#define ML_CLIENT_TELEM_MAGIC    0x54434d51u
#define ML_OBSERVATION_MAGIC     0x514d324fu
#define ML_ACTION_MAGIC          0x514d3241u
#define ML_OBSERVATION_SIZE      1056u
#define ML_ACTION_SIZE           28u
#define ML_CAUSAL_MAGIC          0x514d3343u
#define ML_CAUSAL_VERSION        2u
#define ML_CAUSAL_SIZE           80u
#define ML_CAUSAL_FLAGS_MASK     ((1u << 22) - 1u)
#define ML_CAUSAL_HOOK_FLAGS_MASK (((0x1fu) << 9) | (1u << 19))
#define ML_CAUSAL_ECHO_VALID     (1u << 14)
#define ML_CAUSAL_FACTS_COMPLETE (1u << 15)
#define ML_CAUSAL_TRANSITION_TRAINABLE (1u << 16)
#define ML_CAUSAL_ROLE_PLAYING (1u << 20)
#define ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL (1u << 21)
#define ML_ACTION_GENERATION_COUNT 192u
#define ML_CLIENT_TELEMETRY_SIZE 1248u
#define ML_VERTICAL_DOWN_OR_CROUCH 0u
#define ML_VERTICAL_NEUTRAL        1u
#define ML_VERTICAL_UP_OR_JUMP     2u
#define ML_VERTICAL_COUNT          3u
#define ML_CLIENT_ID_SIZE 40
#define ML_CLIENT_TOKEN_SIZE 64
#define ML_CLIENT_TOKEN_MIN_LENGTH 32
#define ML_CLIENT_TOKEN_ENV "Q2_ML_CLIENT_TELEMETRY_TOKEN"
#define ML_HARNESS_IMPULSE_BASE 16u
#define ML_HARNESS_ACTION_COUNT 40u
#define ML_HARNESS_HIGH_GENERATION_COUNT 6u
#define ML_HARNESS_IMPULSE_COUNT \
    (ML_HARNESS_ACTION_COUNT * ML_HARNESS_HIGH_GENERATION_COUNT)
#define ML_HARNESS_BUTTON_GENERATION_SHIFT 2u
#define ML_HARNESS_BUTTON_GENERATION_MASK 0x7Cu
#define ML_HARNESS_LOW_GENERATION_COUNT 32u

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t barrier_capabilities;
    uint32_t obs_magic;
    uint32_t action_magic;
    uint32_t obs_size;
    uint32_t action_size;
    uint32_t causal_magic;
    uint32_t causal_version;
    uint32_t causal_size;
    char client_id[ML_CLIENT_ID_SIZE];
    char token[ML_CLIENT_TOKEN_SIZE];
} ml_client_register_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t accepted;
    uint32_t client_slot;
    uint32_t server_frame;
    uint32_t barrier_version;
    uint32_t barrier_capabilities;
    uint32_t obs_magic;
    uint32_t action_magic;
    uint32_t obs_size;
    uint32_t action_size;
    uint32_t causal_magic;
    uint32_t causal_version;
    uint32_t causal_size;
    char client_id[ML_CLIENT_ID_SIZE];
} ml_client_ack_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t sequence;
    uint32_t client_slot;
    uint32_t server_frame;
    uint32_t barrier_version;
    uint32_t barrier_capabilities;
    uint32_t map_epoch;
    uint32_t applied_action_tick;
    char client_id[ML_CLIENT_ID_SIZE];
    char map_name[32];
} ml_client_telemetry_header_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t flags;
    uint32_t tick;
    uint32_t client_life_epoch;
    uint32_t target_id;
    uint32_t target_epoch;
    uint32_t environmental_source_id;
    uint32_t environmental_source_epoch;
    uint32_t environmental_mod;
    uint32_t environmental_damage;
    uint32_t crouch_edge_id;
    uint32_t crouch_edge_epoch;
    uint32_t echo_tick;
    uint32_t action_generation;
    uint32_t hook_zone_id;
    uint32_t hook_attempt_tick;
    uint32_t hook_action_generation;
    uint32_t reserved;
} ml_causal_telemetry_t;

typedef struct
{
    uint32_t magic;
    uint32_t tick;
    float move_forward;
    float move_right;
    float look_yaw;
    float look_pitch;
    uint8_t vertical_intent;
    uint8_t fire;
    uint8_t hook;
    uint8_t weapon;
} ml_action_t;

_Static_assert(sizeof(ml_client_register_t) == 148, "registration wire size changed");
_Static_assert(sizeof(ml_client_ack_t) == 100, "ack wire size changed");
_Static_assert(sizeof(ml_client_telemetry_header_t) == 112,
    "telemetry header wire size changed");
_Static_assert(sizeof(ml_action_t) == 28, "action wire size changed");
_Static_assert(sizeof(ml_causal_telemetry_t) == ML_CAUSAL_SIZE,
    "causal telemetry wire size changed");

static cvar_t *ml_harness;
static cvar_t *ml_headless;
static cvar_t *ml_harness_debug;
static cvar_t *ml_client_id;
static cvar_t *ml_frame_barrier;
static cvar_t *ml_telemetry_server;
static cvar_t *ml_harness_addr;
static char ml_telemetry_token[ML_CLIENT_TOKEN_SIZE];
static qboolean ml_telemetry_token_valid;
static netadr_t telemetry_address;
static netadr_t harness_address;
static qboolean addresses_valid;
static qboolean registered;
static int last_register_time;
static ml_action_t latest_action;
static uint32_t latest_action_tick;
static uint32_t applied_look_tick;
static uint32_t applied_command_tick;
static uint32_t ready_action_tick;
static uint32_t latest_telemetry_tick;
static uint32_t latest_telemetry_sequence;
static uint32_t latest_map_epoch;
static qboolean latest_telemetry_valid;
static qboolean awaiting_map_epoch;
static qboolean barrier_bootstrap_ready_sent;
static byte pending_telemetry[ML_CLIENT_TELEMETRY_SIZE];
static byte latest_telemetry[ML_CLIENT_TELEMETRY_SIZE];
static qboolean pending_telemetry_valid;
static char latest_map_name[32];
static ml_harness_view_rebase_t view_rebase;
static ml_harness_role_admission_t role_admission;
static usercmd_t *preparing_action_cmd;
static usercmd_t *prepared_action_cmd;
static uint32_t preparing_action_tick;
static uint32_t prepared_action_tick;
static uint32_t finalized_action_tick;

static qboolean ML_ActionValid(const ml_action_t *action)
{
    return action && action->magic == ML_ACTION_MAGIC && action->tick > 0 &&
        action->vertical_intent < ML_VERTICAL_COUNT && action->fire <= 1 &&
        action->hook <= 3 && action->weapon <= 9 &&
        isfinite(action->move_forward) && isfinite(action->move_right) &&
        isfinite(action->look_yaw) && isfinite(action->look_pitch) &&
        fabsf(action->move_forward) <= 1.0001f &&
        fabsf(action->move_right) <= 1.0001f &&
        fabsf(action->look_yaw) <= 45.0001f &&
        fabsf(action->look_pitch) <= 30.0001f;
}

static qboolean ML_CausalEnvelopeValid(const ml_causal_telemetry_t *causal,
    uint32_t server_frame)
{
    uint32_t hook_flags;
    if (!causal || causal->magic != ML_CAUSAL_MAGIC ||
        causal->version != ML_CAUSAL_VERSION ||
        causal->packet_size != ML_CAUSAL_SIZE || causal->tick != server_frame ||
        causal->client_life_epoch == 0 ||
        causal->flags & ~ML_CAUSAL_FLAGS_MASK || causal->reserved ||
        ((causal->flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL) &&
        !(causal->flags & ML_CAUSAL_ROLE_PLAYING)) ||
        causal->action_generation > ML_ACTION_GENERATION_COUNT ||
        causal->hook_action_generation > ML_ACTION_GENERATION_COUNT)
        return false;
    hook_flags = causal->flags & ML_CAUSAL_HOOK_FLAGS_MASK;
    if (hook_flags)
        return causal->hook_attempt_tick > 0 &&
            causal->hook_attempt_tick <= causal->tick &&
            causal->hook_action_generation > 0 &&
            (causal->hook_attempt_tick != causal->tick ||
             causal->hook_action_generation == causal->action_generation);
    return causal->hook_attempt_tick == 0 &&
        causal->hook_action_generation == 0;
}

static size_t ML_BoundedLength(const char *text, size_t limit)
{
    size_t length = 0;
    if (!text)
        return 0;
    while (length < limit && text[length])
        length++;
    return length;
}

static qboolean ML_LoadTelemetryToken(void)
{
    const char *source = getenv(ML_CLIENT_TOKEN_ENV);
    size_t index, length = ML_BoundedLength(source, ML_CLIENT_TOKEN_SIZE);

    memset(ml_telemetry_token, 0, sizeof(ml_telemetry_token));
    if (length < ML_CLIENT_TOKEN_MIN_LENGTH ||
        length >= ML_CLIENT_TOKEN_SIZE)
        return false;
    for (index = 0; index < length; index++)
    {
        const char value = source[index];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') ||
              value == '.' || value == '_' || value == '~' ||
              value == '+' || value == '/' || value == '=' || value == '-'))
            return false;
    }
    memcpy(ml_telemetry_token, source, length);
    return true;
}

static const char *ML_WeaponName(uint8_t weapon)
{
    static const char *names[] = {
        NULL, "Blaster", "Shotgun", "Super Shotgun", "Machinegun",
        "Chaingun", "Grenade Launcher", "Rocket Launcher", "HyperBlaster",
        "Railgun"
    };
    return weapon < (sizeof(names) / sizeof(names[0])) ? names[weapon] : NULL;
}

static qboolean ML_IdMatches(const char *packet_id)
{
    size_t packet_len, configured_len;
    packet_len = ML_BoundedLength(packet_id, ML_CLIENT_ID_SIZE);
    configured_len = ML_BoundedLength(ml_client_id->string, ML_CLIENT_ID_SIZE);
    return packet_len > 0 && packet_len < ML_CLIENT_ID_SIZE &&
        packet_len == configured_len &&
        memcmp(packet_id, ml_client_id->string, packet_len) == 0;
}

static void ML_ClearPreparedAction(void)
{
    preparing_action_cmd = NULL;
    prepared_action_cmd = NULL;
    preparing_action_tick = 0;
    prepared_action_tick = 0;
    finalized_action_tick = 0;
}

static void ML_ClearActionEpoch(void)
{
    memset(&latest_action, 0, sizeof(latest_action));
    latest_action.vertical_intent = ML_VERTICAL_NEUTRAL;
    latest_action_tick = 0;
    applied_look_tick = 0;
    applied_command_tick = 0;
    ready_action_tick = 0;
    ML_ClearPreparedAction();
}

static void ML_MaybeSendReady(void)
{
    if (!ml_frame_barrier || !ml_frame_barrier->value || !registered ||
        awaiting_map_epoch || view_rebase.pending || role_admission.pending ||
        !latest_action_tick ||
        finalized_action_tick != latest_action_tick ||
        ready_action_tick == latest_action_tick)
        return;
    Cbuf_AddText(va("cmd ml_barrier_ready %u %u\n",
        ML_CLIENT_FRAME_BARRIER_VERSION, latest_action_tick));
    ready_action_tick = latest_action_tick;
}

static qboolean ML_MapEqual(const char *packet_map)
{
    size_t map_len = ML_BoundedLength(packet_map, sizeof(latest_map_name));
    size_t current_len = ML_BoundedLength(
        latest_map_name, sizeof(latest_map_name));
    return map_len > 0 && map_len < sizeof(latest_map_name) &&
        map_len == current_len &&
        memcmp(packet_map, latest_map_name, map_len) == 0;
}

static qboolean ML_UpdateMap(const char *packet_map)
{
    size_t map_len = ML_BoundedLength(packet_map, sizeof(latest_map_name));
    size_t current_len = ML_BoundedLength(
        latest_map_name, sizeof(latest_map_name));
    qboolean changed;

    if (!map_len || map_len >= sizeof(latest_map_name))
        return false;
    changed = map_len != current_len ||
        memcmp(packet_map, latest_map_name, map_len) != 0;
    if (!changed)
        return true;

    memset(latest_map_name, 0, sizeof(latest_map_name));
    memcpy(latest_map_name, packet_map, map_len);
    ML_ClearActionEpoch();
    memset(&role_admission, 0, sizeof(role_admission));
    if (ml_harness_debug && ml_harness_debug->value)
        Com_Printf("ML harness: telemetry map changed to %s; action epoch reset\n",
            latest_map_name);
    return true;
}

static void ML_ReportOldTelemetry(const ml_client_telemetry_header_t *telemetry)
{
    if (!ml_frame_barrier || !ml_frame_barrier->value || !telemetry)
        return;
    Cbuf_AddText(va(
        "cmd ml_barrier_old_telemetry_discarded %u %u %u\n",
        ML_CLIENT_FRAME_BARRIER_VERSION, telemetry->map_epoch,
        telemetry->server_frame));
}

static void ML_ResolveAddresses(void)
{
    addresses_valid = false;
    if (!ml_harness || !ml_harness->value)
        return;
    if (!ml_telemetry_token_valid)
        Com_Error(ERR_FATAL,
            "ML harness: " ML_CLIENT_TOKEN_ENV " is missing or malformed\n");
    if (!ml_telemetry_server->string[0] ||
        !ml_harness_addr->string[0] || !ml_client_id->string[0])
        return;
    if (!NET_StringToAdr(ml_telemetry_server->string, &telemetry_address) ||
        !NET_StringToAdr(ml_harness_addr->string, &harness_address))
    {
        Com_Printf("ML harness: invalid telemetry or harness address\n");
        return;
    }
    addresses_valid = true;
}

void ML_HarnessInit(void)
{
    ml_harness = Cvar_Get("ml_harness", "0", 0);
    ml_headless = Cvar_Get("ml_headless", "0", 0);
    ml_harness_debug = Cvar_Get("ml_harness_debug", "0", 0);
    ml_client_id = Cvar_Get("ml_client_id", "", CVAR_USERINFO);
    ml_frame_barrier = Cvar_Get("ml_frame_barrier", "0", CVAR_USERINFO);
    Cvar_Get("ml_frame_barrier_version", "1", CVAR_USERINFO);
    Cvar_Get("ml_frame_barrier_capability", "1", CVAR_USERINFO);
    ml_telemetry_server = Cvar_Get("ml_telemetry_server", "", 0);
    ml_harness_addr = Cvar_Get("ml_harness_addr", "127.0.0.1:39000", 0);
    ml_telemetry_token_valid = ML_LoadTelemetryToken();
    memset(&latest_action, 0, sizeof(latest_action));
    latest_action.vertical_intent = ML_VERTICAL_NEUTRAL;
    ML_ResolveAddresses();
    if (ml_harness->value)
        Com_Printf("ML harness: client_id=%s conduit=%s local=%s\n",
            ml_client_id->string, ml_telemetry_server->string,
            ml_harness_addr->string);
}

qboolean ML_HarnessHeadless(void)
{
    return ml_harness && ml_harness->value && ml_headless &&
        ml_headless->value ? true : false;
}

void ML_HarnessShutdown(void)
{
    registered = false;
    addresses_valid = false;
    ml_telemetry_token_valid = false;
    memset(ml_telemetry_token, 0, sizeof(ml_telemetry_token));
    latest_telemetry_valid = false;
    awaiting_map_epoch = false;
    latest_telemetry_tick = 0;
    latest_telemetry_sequence = 0;
    latest_map_epoch = 0;
    memset(latest_telemetry, 0, sizeof(latest_telemetry));
    memset(latest_map_name, 0, sizeof(latest_map_name));
    memset(&view_rebase, 0, sizeof(view_rebase));
    memset(&role_admission, 0, sizeof(role_admission));
}

void ML_HarnessPump(void)
{
    ml_client_register_t registration;
    if (!ml_harness || !ml_harness->value)
        return;
    if (!addresses_valid)
        ML_ResolveAddresses();
    if (!addresses_valid || cls.state < ca_connected)
    {
        registered = false;
        return;
    }
    if (registered && cls.realtime - last_register_time < 5000)
        return;
    if (!registered && cls.realtime - last_register_time < 500)
        return;

    memset(&registration, 0, sizeof(registration));
    registration.magic = ML_CLIENT_REGISTER_MAGIC;
    registration.version = ML_CLIENT_WIRE_VERSION;
    registration.packet_size = sizeof(registration);
    registration.barrier_capabilities = ML_CLIENT_FRAME_BARRIER_CAPABILITY;
    registration.obs_magic = ML_OBSERVATION_MAGIC;
    registration.action_magic = ML_ACTION_MAGIC;
    registration.obs_size = ML_OBSERVATION_SIZE;
    registration.action_size = ML_ACTION_SIZE;
    registration.causal_magic = ML_CAUSAL_MAGIC;
    registration.causal_version = ML_CAUSAL_VERSION;
    registration.causal_size = ML_CAUSAL_SIZE;
    Q_strlcpy(registration.client_id, ml_client_id->string,
        sizeof(registration.client_id));
    memcpy(registration.token, ml_telemetry_token,
        sizeof(registration.token));
    NET_SendPacket(NS_CLIENT, sizeof(registration), &registration,
        telemetry_address);
    if (ml_frame_barrier->value && cls.state == ca_active &&
        !barrier_bootstrap_ready_sent)
    {
        if (!ML_HarnessBootstrapRoleReady(cl.frame.valid,
            cl.frame.playerstate.pmove.pm_type == PM_NORMAL))
            Com_Error(ERR_DROP,
                "ML barrier: bootstrap requires public PM_NORMAL, got "
                "valid=%d pm_type=%d frame=%d\n",
                cl.frame.valid ? 1 : 0,
                cl.frame.playerstate.pmove.pm_type,
                cl.frame.serverframe);
        Cbuf_AddText(va("cmd ml_barrier_bootstrap_ready %u %s\n",
            ML_CLIENT_FRAME_BARRIER_VERSION, ml_client_id->string));
        barrier_bootstrap_ready_sent = true;
    }
    last_register_time = cls.realtime;
}

static void ML_ProcessTelemetry(const byte *data)
{
    const ml_client_telemetry_header_t *telemetry =
        (const ml_client_telemetry_header_t *)data;
    const ml_causal_telemetry_t *causal = (const ml_causal_telemetry_t *)(data +
        sizeof(*telemetry) + ML_OBSERVATION_SIZE);
    qboolean same_map_epoch;
    int life_result;
    int role_result;
    int settle_result;
    if (telemetry->version != ML_CLIENT_WIRE_VERSION ||
        telemetry->packet_size != ML_CLIENT_TELEMETRY_SIZE ||
        telemetry->barrier_version != ML_CLIENT_FRAME_BARRIER_VERSION ||
        (telemetry->barrier_capabilities &
            ML_CLIENT_FRAME_BARRIER_CAPABILITY) == 0 ||
        telemetry->map_epoch == 0 ||
        !ML_CausalEnvelopeValid(causal, telemetry->server_frame) ||
        !ML_IdMatches(telemetry->client_id) ||
        ML_BoundedLength(telemetry->map_name, sizeof(telemetry->map_name)) == 0 ||
        ML_BoundedLength(telemetry->map_name,
            sizeof(telemetry->map_name)) >= sizeof(telemetry->map_name))
        return;
    if (!registered)
    {
        memcpy(pending_telemetry, data, sizeof(pending_telemetry));
        pending_telemetry_valid = true;
        return;
    }

    /* The authenticated conduit is monotonic across map loads.  Old UDP
       datagrams are harmlessly discarded, while an equivocation for an
       already accepted frame or an unannounced epoch is fatal.  Only the
       protocol serverdata boundary may arm the next epoch. */
    if (latest_telemetry_valid)
    {
        if (telemetry->map_epoch < latest_map_epoch)
            return;
        if (telemetry->map_epoch == latest_map_epoch)
        {
            if (awaiting_map_epoch)
                return;
            if (!ML_MapEqual(telemetry->map_name))
                Com_Error(ERR_DROP,
                    "ML barrier: conflicting map name in epoch %u\n",
                    telemetry->map_epoch);
            if (telemetry->server_frame < latest_telemetry_tick)
            {
                ML_ReportOldTelemetry(telemetry);
                return;
            }
            if (telemetry->server_frame == latest_telemetry_tick)
            {
                if (!memcmp(data, latest_telemetry,
                    sizeof(latest_telemetry)))
                    return;
                Com_Error(ERR_DROP,
                    "ML barrier: conflicting telemetry for frame %u\n",
                    telemetry->server_frame);
            }
            if (telemetry->sequence <= latest_telemetry_sequence)
                Com_Error(ERR_DROP,
                    "ML barrier: telemetry sequence rollback %u <= %u\n",
                    telemetry->sequence, latest_telemetry_sequence);
        }
        else if (!awaiting_map_epoch ||
            telemetry->map_epoch != latest_map_epoch + 1)
        {
            Com_Error(ERR_DROP,
                "ML barrier: unannounced map epoch %u after %u\n",
                telemetry->map_epoch, latest_map_epoch);
        }
    }
    /* Map loading runs two mandatory game-DLL settle frames.  The absolute
       first barrier frame is therefore not assumed to be one; the invariant
       is the causal ladder itself: exactly one applied tick precedes every
       accepted telemetry frame, including bootstrap and a new map epoch. */
    if (ml_frame_barrier->value && (telemetry->server_frame == 0 ||
        telemetry->applied_action_tick + 1 != telemetry->server_frame))
        Com_Error(ERR_DROP,
            "ML barrier: applied action %u not bound to frame %u\n",
            telemetry->applied_action_tick, telemetry->server_frame);
    same_map_epoch = latest_telemetry_valid &&
        telemetry->map_epoch == latest_map_epoch;
    if (!ML_UpdateMap(telemetry->map_name))
        return;
    role_result = ML_HarnessRoleArm(&role_admission,
        telemetry->server_frame, causal->flags);
    if (role_result < 0)
        Com_Error(ERR_DROP,
            "ML barrier: invalid routed role flags=0x%08x frame=%u\n",
            causal->flags, telemetry->server_frame);
    life_result = ML_HarnessViewAcceptLife(&view_rebase,
        causal->client_life_epoch, telemetry->server_frame,
        telemetry->applied_action_tick,
        same_map_epoch ? 1 : 0,
        (causal->flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL) != 0);
    if (life_result < 0)
        Com_Error(ERR_DROP,
            "ML barrier: invalid life epoch %u after %u in frame %u\n",
            causal->client_life_epoch, view_rebase.accepted_life_epoch,
            telemetry->server_frame);
    /* Stock teleport settling remains public PM_NORMAL.  A role-playing
       non-normal packet is death/GIB/intermission lifecycle, so it must not
       be inferred as settling merely from the generic E/F/T0 triad. */
    if (causal->flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL)
        settle_result = ML_HarnessViewObserveSettleCausal(&view_rebase,
            causal->client_life_epoch, telemetry->server_frame,
            telemetry->applied_action_tick,
            (causal->flags & ML_CAUSAL_ECHO_VALID) != 0,
            (causal->flags & ML_CAUSAL_FACTS_COMPLETE) != 0,
            (causal->flags & ML_CAUSAL_TRANSITION_TRAINABLE) != 0);
    else
        settle_result = ML_HarnessViewLeavePublicNormal(&view_rebase);
    if (settle_result < 0)
        Com_Error(ERR_DROP,
            "ML barrier: invalid respawn-settling causal triad in frame %u\n",
            telemetry->server_frame);
    latest_map_epoch = telemetry->map_epoch;
    latest_telemetry_tick = telemetry->server_frame;
    latest_telemetry_sequence = telemetry->sequence;
    latest_telemetry_valid = true;
    awaiting_map_epoch = false;
    memcpy(latest_telemetry, data, sizeof(latest_telemetry));
    pending_telemetry_valid = false;
    if (role_result > 0 || life_result > 0 || settle_result > 0)
    {
        ML_ClearPreparedAction();
        if ((life_result > 0 || settle_result > 0) &&
            ml_harness_debug && ml_harness_debug->value)
            Com_Printf("ML_FRAME_BARRIER_EVENT event=life_view_rebase_armed "
                "client_id=%s prior_life_epoch=%u life_epoch=%u "
                "telemetry_frame=%u source=%s\n",
                ml_client_id->string, view_rebase.prior_life_epoch,
                view_rebase.pending_life_epoch,
                view_rebase.pending_server_frame,
                view_rebase.pending_kind ==
                    ML_HARNESS_VIEW_REBASE_SETTLE_FRAME
                    ? "teleport_settle_frame" : "causal_life_epoch");
        if (role_result > 0 && ml_harness_debug && ml_harness_debug->value)
            Com_Printf("ML_FRAME_BARRIER_EVENT event=role_fence_armed "
                "client_id=%s telemetry_frame=%u role_playing=1 "
                "role_public_pm_normal=%d trainable=%d\n",
                ml_client_id->string, telemetry->server_frame,
                (causal->flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL) != 0,
                (causal->flags & ML_CAUSAL_TRANSITION_TRAINABLE) != 0);
        ML_HarnessServerFrame();
    }
    NET_SendPacket(NS_CLIENT, ML_CLIENT_TELEMETRY_SIZE, (void *)data,
        harness_address);
}

qboolean ML_HarnessPacket(netadr_t from, const byte *data, int length)
{
    uint32_t magic;
    const ml_client_ack_t *ack;
    const ml_action_t *action;

    if (!ml_harness || !ml_harness->value || length < (int)sizeof(magic))
        return false;
    memcpy(&magic, data, sizeof(magic));

    if (addresses_valid && NET_CompareAdr(from, harness_address))
    {
        if (magic == ML_ACTION_MAGIC && length == (int)sizeof(*action))
        {
            action = (const ml_action_t *)data;
            if (ML_ActionValid(action))
            {
                if (ml_frame_barrier->value)
                {
                    if (!registered || !latest_telemetry_tick ||
                        awaiting_map_epoch)
                        Com_Error(ERR_DROP,
                            "ML barrier: action arrived before accepted telemetry\n");
                    if (action->tick < latest_telemetry_tick)
                        return true;
                    if (action->tick > latest_telemetry_tick)
                        Com_Error(ERR_DROP,
                            "ML barrier: future action %u for telemetry %u\n",
                            action->tick, latest_telemetry_tick);
                    if (action->tick == latest_action_tick)
                    {
                        if (memcmp(action, &latest_action, sizeof(*action)))
                            Com_Error(ERR_DROP,
                                "ML barrier: conflicting duplicate action %u\n",
                                action->tick);
                        return true;
                    }
                }
                else if (action->tick < latest_action_tick)
                    return true;
                latest_action = *action;
                latest_action_tick = action->tick;
                ML_ClearPreparedAction();
                if (ml_harness_debug->value)
                    Com_Printf("ML harness: action tick=%u forward=%.2f fire=%u\n",
                        action->tick, action->move_forward, action->fire);
            }
        }
        /* The configured harness endpoint is a private action channel. Consume
           every malformed/legacy datagram from it instead of offering those
           bytes to the ordinary Quake netchan. */
        return true;
    }

    if (!addresses_valid || !NET_CompareAdr(from, telemetry_address))
        return false;

    if (magic == ML_CLIENT_ACK_MAGIC && length == (int)sizeof(*ack))
    {
        ack = (const ml_client_ack_t *)data;
        if (ack->version == ML_CLIENT_WIRE_VERSION &&
            ack->packet_size == sizeof(*ack) &&
            ack->barrier_version == ML_CLIENT_FRAME_BARRIER_VERSION &&
            (ack->barrier_capabilities &
                ML_CLIENT_FRAME_BARRIER_CAPABILITY) != 0 &&
            ack->obs_magic == ML_OBSERVATION_MAGIC &&
            ack->action_magic == ML_ACTION_MAGIC &&
            ack->obs_size == ML_OBSERVATION_SIZE &&
            ack->action_size == ML_ACTION_SIZE &&
            ack->causal_magic == ML_CAUSAL_MAGIC &&
            ack->causal_version == ML_CAUSAL_VERSION &&
            ack->causal_size == ML_CAUSAL_SIZE && ML_IdMatches(ack->client_id))
        {
            registered = ack->accepted ? true : false;
            if (registered)
            {
                Com_DPrintf("ML harness: conduit registered on slot %u\n",
                    ack->client_slot);
                if (pending_telemetry_valid)
                {
                    const ml_client_telemetry_header_t *pending =
                        (const ml_client_telemetry_header_t *)pending_telemetry;
                    if (pending->server_frame == ack->server_frame)
                        ML_ProcessTelemetry(pending_telemetry);
                    else
                        pending_telemetry_valid = false;
                }
            }
        }
        return true;
    }

    if (magic == ML_CLIENT_TELEM_MAGIC &&
        length == (int)ML_CLIENT_TELEMETRY_SIZE)
    {
        ML_ProcessTelemetry(data);
        return true;
    }

    return true; /* authenticated conduit address: never feed it to netchan */
}

void ML_HarnessApplyAction(usercmd_t *cmd)
{
    float forward, right;
    const char *weapon_name;
    if (!ml_harness || !ml_harness->value || !cmd || !latest_action_tick ||
        awaiting_map_epoch || view_rebase.pending || role_admission.pending)
        return;

    forward = latest_action.move_forward;
    right = latest_action.move_right;
    if (forward < -1.0f) forward = -1.0f;
    if (forward > 1.0f) forward = 1.0f;
    if (right < -1.0f) right = -1.0f;
    if (right > 1.0f) right = 1.0f;
    cmd->forwardmove = (short)(forward * 320.0f);
    cmd->sidemove = (short)(right * 320.0f);
    cmd->upmove = latest_action.vertical_intent == ML_VERTICAL_UP_OR_JUMP
        ? 320 : latest_action.vertical_intent == ML_VERTICAL_DOWN_OR_CROUCH
            ? -320 : 0;
    if (latest_action.fire)
        cmd->buttons |= BUTTON_ATTACK;
    else
        cmd->buttons &= ~BUTTON_ATTACK;

    /* Look deltas are decisions, not held controls: apply each telemetry
       tick once even if the renderer emits several usercmds per server tick. */
    if (latest_action.tick != applied_look_tick)
    {
        int teleport_settling =
            (cl.frame.playerstate.pmove.pm_flags & PMF_TIME_TELEPORT) != 0;
        cl.viewangles[YAW] += latest_action.look_yaw;
        cl.viewangles[PITCH] = ML_HarnessViewApplyPolicyPitch(
            cl.viewangles[PITCH], latest_action.look_pitch,
            teleport_settling);
        applied_look_tick = latest_action.tick;
    }

    /* Grapple and weapon selection are reliable Quake client commands, not
       usercmd bits. Emit them once for each new policy decision. */
    if (latest_action.tick != applied_command_tick)
    {
        if (ml_harness_debug->value)
            Com_Printf("ML harness: applying tick=%u forward=%d side=%d\n",
                latest_action.tick, cmd->forwardmove, cmd->sidemove);
        if (!ml_frame_barrier->value)
        {
            if (latest_action.hook == 1)
                Cbuf_AddText("cmd hook\n");
            else if (latest_action.hook == 3)
                Cbuf_AddText("cmd unhook\n");
            weapon_name = ML_WeaponName(latest_action.weapon);
            if (weapon_name)
                Cbuf_AddText(va("cmd use \"%s\"\n", weapon_name));
        }
        applied_command_tick = latest_action.tick;
    }
    preparing_action_cmd = cmd;
    preparing_action_tick = latest_action_tick;
}

void ML_HarnessActionAnglesPrepared(usercmd_t *cmd)
{
    if (!ml_harness || !ml_harness->value || !cmd ||
        awaiting_map_epoch || view_rebase.pending || role_admission.pending ||
        preparing_action_cmd != cmd ||
        preparing_action_tick != latest_action_tick ||
        !latest_action_tick)
    {
        prepared_action_cmd = NULL;
        prepared_action_tick = 0;
        return;
    }
    prepared_action_cmd = cmd;
    prepared_action_tick = latest_action_tick;
}

void ML_HarnessResetEpoch(void)
{
    registered = false;
    last_register_time = 0;
    ML_ClearActionEpoch();
    awaiting_map_epoch = latest_telemetry_valid;
    barrier_bootstrap_ready_sent = false;
    pending_telemetry_valid = false;
    memset(&view_rebase, 0, sizeof(view_rebase));
    memset(&role_admission, 0, sizeof(role_admission));
}

void ML_HarnessServerFrame(void)
{
    int i;
    int role_result;
    int snapshot_result;
    uint32_t snapshot_frame;

    if (!ml_harness || !ml_harness->value ||
        (!view_rebase.pending && !role_admission.pending) ||
        !cl.frame.valid || cl.frame.serverframe < 0)
        return;
    snapshot_frame = (uint32_t)cl.frame.serverframe;
    if (role_admission.pending)
    {
        uint32_t role_frame = role_admission.pending_server_frame;
        uint32_t role_flags = role_admission.pending_flags;
        int pm_type = cl.frame.playerstate.pmove.pm_type;
        role_result = ML_HarnessRoleSnapshotReady(&role_admission,
            snapshot_frame, pm_type == PM_NORMAL,
            pm_type == PM_SPECTATOR,
            pm_type == PM_DEAD || pm_type == PM_GIB ||
                pm_type == PM_FREEZE);
        if (role_result < 0)
            Com_Error(ERR_DROP,
                "ML barrier: routed role mismatch flags=0x%08x "
                "pm_type=%d snapshot=%u telemetry=%u\n",
                role_flags, pm_type, snapshot_frame, role_frame);
        if (role_result == 0)
            return;
        if (ml_harness_debug && ml_harness_debug->value)
            Com_Printf("ML_FRAME_BARRIER_EVENT event=role_fence_accept "
                "client_id=%s telemetry_frame=%u snapshot_frame=%u "
                "role_playing=1 role_public_pm_normal=%d pm_type=%d\n",
                ml_client_id->string, role_frame, snapshot_frame,
                (role_flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL) != 0,
                pm_type);
    }
    if (!view_rebase.pending)
        return;
    if (snapshot_frame < view_rebase.pending_server_frame)
        return;
    if (snapshot_frame > view_rebase.pending_server_frame)
        Com_Error(ERR_DROP,
            "ML barrier: life view snapshot %u advanced past telemetry %u\n",
            snapshot_frame, view_rebase.pending_server_frame);
    if (cl.frame.playerstate.pmove.pm_type != PM_NORMAL &&
        !(view_rebase.pending_kind ==
            ML_HARNESS_VIEW_REBASE_SETTLE_FRAME &&
        (cl.frame.playerstate.pmove.pm_type == PM_DEAD ||
            cl.frame.playerstate.pmove.pm_type == PM_GIB)))
        Com_Error(ERR_DROP,
            "ML barrier: nonplayable view rebase kind=%d pm_type=%d frame=%u\n",
            view_rebase.pending_kind,
            cl.frame.playerstate.pmove.pm_type, snapshot_frame);
    snapshot_result = ML_HarnessViewSnapshotReady(
        &view_rebase, snapshot_frame);
    if (snapshot_result < 0)
        Com_Error(ERR_DROP,
            "ML barrier: life view snapshot %u advanced past telemetry %u\n",
            snapshot_frame, view_rebase.pending_server_frame);
    if (snapshot_result == 0)
        return;

    if (cl.frame.playerstate.pmove.pm_type == PM_DEAD ||
        cl.frame.playerstate.pmove.pm_type == PM_GIB)
    {
        /* A death during stock settling is still an exact nontrainable
           lifecycle boundary, but its public playerstate is a death camera
           and cannot define a playable command-angle basis.  Consume only
           the matching frame and fence its action; life+1 owns the next real
           rebase. */
        applied_look_tick = view_rebase.pending_applied_action_tick;
        if (ml_harness_debug && ml_harness_debug->value)
            Com_Printf("ML_FRAME_BARRIER_EVENT "
                "event=life_view_rebase_nonplayable client_id=%s "
                "life_epoch=%u telemetry_frame=%u snapshot_frame=%u "
                "applied_action_tick=%u pm_type=%d trainable=0\n",
                ml_client_id->string, view_rebase.pending_life_epoch,
                view_rebase.pending_server_frame, snapshot_frame,
                view_rebase.pending_applied_action_tick,
                cl.frame.playerstate.pmove.pm_type);
        return;
    }

    for (i = 0; i < 3; i++)
        cl.viewangles[i] = ML_HarnessViewCommandAngle(
            cl.frame.playerstate.viewangles[i],
            cl.frame.playerstate.pmove.delta_angles[i]);
    /* The normal playerstate already represents the boundary action.  Fence
       exactly that tick: an action for a newer telemetry frame that arrived
       while the snapshot was in flight remains pending and applies once. */
    applied_look_tick = view_rebase.pending_applied_action_tick;
    if (ml_harness_debug && ml_harness_debug->value)
        Com_Printf("ML_FRAME_BARRIER_EVENT event=life_view_rebase "
            "client_id=%s prior_life_epoch=%u life_epoch=%u "
            "telemetry_frame=%u snapshot_frame=%u source=protocol_playerstate "
            "applied_action_tick=%u command_yaw=%.6f command_pitch=%.6f\n",
            ml_client_id->string, view_rebase.prior_life_epoch,
            view_rebase.pending_life_epoch,
            view_rebase.pending_server_frame, snapshot_frame,
            view_rebase.pending_applied_action_tick,
            cl.viewangles[YAW], cl.viewangles[PITCH]);
}

void ML_HarnessFinalizeAction(usercmd_t *cmd)
{
    uint32_t encoded;
    if (!ml_harness || !ml_harness->value || !cmd)
        return;
    if (!latest_action_tick || awaiting_map_epoch || view_rebase.pending ||
        role_admission.pending ||
        prepared_action_cmd != cmd ||
        prepared_action_tick != latest_action_tick)
    {
        cmd->buttons &= ~ML_HARNESS_BUTTON_GENERATION_MASK;
        return;
    }

    /* usercmd.impulse is unused by the Quake II game and already travels in
       the ordinary protocol-34 command stream. Reserve 16..255 plus five
       otherwise-unused button bits as a modulo-192 decision identity and
       hook/weapon request; game.so validates this only for registered ML
       identities and strips the private button bits before gameplay. Reliable
       `cmd hook/use` remains the actual gameplay mechanism. */
    encoded = ((latest_action.tick / ML_HARNESS_LOW_GENERATION_COUNT) %
        ML_HARNESS_HIGH_GENERATION_COUNT) *
        ML_HARNESS_ACTION_COUNT +
        (uint32_t)latest_action.hook * 10u +
        (uint32_t)latest_action.weapon;
    if (encoded < ML_HARNESS_IMPULSE_COUNT)
        cmd->impulse = (byte)(ML_HARNESS_IMPULSE_BASE + encoded);
    cmd->buttons = (byte)((cmd->buttons & ~ML_HARNESS_BUTTON_GENERATION_MASK) |
        ((latest_action.tick % ML_HARNESS_LOW_GENERATION_COUNT) <<
            ML_HARNESS_BUTTON_GENERATION_SHIFT));
    finalized_action_tick = latest_action_tick;
    prepared_action_cmd = NULL;
    prepared_action_tick = 0;
    ML_MaybeSendReady();
}
