/* Network-native ML client integration.
 *
 * This remains a normal protocol-34 client.  Policy actions become ordinary
 * usercmd_t messages.  Privileged server observations arrive on a separate,
 * authenticated conduit and are forwarded only to this client's local
 * harness endpoint.
 */
#include "header/ml_harness.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ML_CLIENT_WIRE_VERSION 1u
#define ML_CLIENT_REGISTER_MAGIC 0x52434d51u
#define ML_CLIENT_ACK_MAGIC      0x41434d51u
#define ML_CLIENT_TELEM_MAGIC    0x54434d51u
#define ML_ACTION_MAGIC          0x514d4c41u
#define ML_CLIENT_ID_SIZE 40
#define ML_CLIENT_TOKEN_SIZE 64

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t reserved;
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
    char client_id[ML_CLIENT_ID_SIZE];
    char map_name[32];
} ml_client_telemetry_header_t;

typedef struct
{
    uint32_t magic;
    uint32_t tick;
    float move_forward;
    float move_right;
    float look_yaw;
    float look_pitch;
    uint8_t jump;
    uint8_t fire;
    uint8_t hook;
    uint8_t weapon;
} ml_action_t;

_Static_assert(sizeof(ml_client_register_t) == 120, "registration wire size changed");
_Static_assert(sizeof(ml_client_ack_t) == 64, "ack wire size changed");
_Static_assert(sizeof(ml_client_telemetry_header_t) == 96,
    "telemetry header wire size changed");
_Static_assert(sizeof(ml_action_t) == 28, "action wire size changed");

static cvar_t *ml_harness;
static cvar_t *ml_headless;
static cvar_t *ml_harness_debug;
static cvar_t *ml_client_id;
static cvar_t *ml_telemetry_server;
static cvar_t *ml_telemetry_token;
static cvar_t *ml_harness_addr;
static netadr_t telemetry_address;
static netadr_t harness_address;
static qboolean addresses_valid;
static qboolean registered;
static int last_register_time;
static ml_action_t latest_action;
static uint32_t latest_action_tick;
static uint32_t applied_look_tick;
static uint32_t applied_command_tick;
static char latest_map_name[32];

static size_t ML_BoundedLength(const char *text, size_t limit)
{
    size_t length = 0;
    if (!text)
        return 0;
    while (length < limit && text[length])
        length++;
    return length;
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
    memset(&latest_action, 0, sizeof(latest_action));
    latest_action_tick = 0;
    applied_look_tick = 0;
    applied_command_tick = 0;
    if (ml_harness_debug && ml_harness_debug->value)
        Com_Printf("ML harness: telemetry map changed to %s; action epoch reset\n",
            latest_map_name);
    return true;
}

static void ML_ResolveAddresses(void)
{
    addresses_valid = false;
    if (!ml_harness || !ml_harness->value || !ml_telemetry_server->string[0] ||
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
    ml_telemetry_server = Cvar_Get("ml_telemetry_server", "", 0);
    ml_telemetry_token = Cvar_Get("ml_telemetry_token", "", 0);
    ml_harness_addr = Cvar_Get("ml_harness_addr", "127.0.0.1:39000", 0);
    memset(&latest_action, 0, sizeof(latest_action));
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
    Q_strlcpy(registration.client_id, ml_client_id->string,
        sizeof(registration.client_id));
    Q_strlcpy(registration.token, ml_telemetry_token->string,
        sizeof(registration.token));
    NET_SendPacket(NS_CLIENT, sizeof(registration), &registration,
        telemetry_address);
    last_register_time = cls.realtime;
}

qboolean ML_HarnessPacket(netadr_t from, const byte *data, int length)
{
    uint32_t magic;
    const ml_client_ack_t *ack;
    const ml_client_telemetry_header_t *telemetry;
    const ml_action_t *action;

    if (!ml_harness || !ml_harness->value || length < (int)sizeof(magic))
        return false;
    memcpy(&magic, data, sizeof(magic));

    if (addresses_valid && NET_CompareAdr(from, harness_address) &&
        magic == ML_ACTION_MAGIC && length == (int)sizeof(*action))
    {
        action = (const ml_action_t *)data;
        if (action->tick >= latest_action_tick)
        {
            latest_action = *action;
            latest_action_tick = action->tick;
            if (ml_harness_debug->value)
                Com_Printf("ML harness: action tick=%u forward=%.2f fire=%u\n",
                    action->tick, action->move_forward, action->fire);
        }
        return true;
    }

    if (!addresses_valid || !NET_CompareAdr(from, telemetry_address))
        return false;

    if (magic == ML_CLIENT_ACK_MAGIC && length == (int)sizeof(*ack))
    {
        ack = (const ml_client_ack_t *)data;
        if (ack->version == ML_CLIENT_WIRE_VERSION &&
            ack->packet_size == sizeof(*ack) && ML_IdMatches(ack->client_id))
        {
            registered = ack->accepted ? true : false;
            if (registered)
                Com_DPrintf("ML harness: conduit registered on slot %u\n",
                    ack->client_slot);
        }
        return true;
    }

    if (magic == ML_CLIENT_TELEM_MAGIC &&
        length >= (int)sizeof(*telemetry))
    {
        telemetry = (const ml_client_telemetry_header_t *)data;
        if (telemetry->version == ML_CLIENT_WIRE_VERSION &&
            telemetry->packet_size == (uint32_t)length &&
            ML_IdMatches(telemetry->client_id) &&
            ML_UpdateMap(telemetry->map_name))
        {
            registered = true;
            NET_SendPacket(NS_CLIENT, length, (void *)data, harness_address);
        }
        return true;
    }

    return true; /* authenticated conduit address: never feed it to netchan */
}

void ML_HarnessApplyAction(usercmd_t *cmd)
{
    float forward, right;
    const char *weapon_name;
    if (!ml_harness || !ml_harness->value || !cmd || !latest_action_tick)
        return;

    forward = latest_action.move_forward;
    right = latest_action.move_right;
    if (forward < -1.0f) forward = -1.0f;
    if (forward > 1.0f) forward = 1.0f;
    if (right < -1.0f) right = -1.0f;
    if (right > 1.0f) right = 1.0f;
    cmd->forwardmove = (short)(forward * 320.0f);
    cmd->sidemove = (short)(right * 320.0f);
    cmd->upmove = latest_action.jump ? 320 : 0;
    if (latest_action.fire)
        cmd->buttons |= BUTTON_ATTACK;
    else
        cmd->buttons &= ~BUTTON_ATTACK;

    /* Look deltas are decisions, not held controls: apply each telemetry
       tick once even if the renderer emits several usercmds per server tick. */
    if (latest_action.tick != applied_look_tick)
    {
        cl.viewangles[YAW] += latest_action.look_yaw;
        cl.viewangles[PITCH] += latest_action.look_pitch;
        applied_look_tick = latest_action.tick;
    }

    /* Grapple and weapon selection are reliable Quake client commands, not
       usercmd bits. Emit them once for each new policy decision. */
    if (latest_action.tick != applied_command_tick)
    {
        if (ml_harness_debug->value)
            Com_Printf("ML harness: applying tick=%u forward=%d side=%d\n",
                latest_action.tick, cmd->forwardmove, cmd->sidemove);
        if (latest_action.hook == 1)
            Cbuf_AddText("cmd hook\n");
        else if (latest_action.hook == 3)
            Cbuf_AddText("cmd unhook\n");
        weapon_name = ML_WeaponName(latest_action.weapon);
        if (weapon_name)
            Cbuf_AddText(va("cmd use \"%s\"\n", weapon_name));
        applied_command_tick = latest_action.tick;
    }
}
