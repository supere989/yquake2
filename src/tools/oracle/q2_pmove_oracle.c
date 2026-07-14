#include "q2_oracle.h"
#include "oracle_identity.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *map_path;
static char map_sha256[65];
static unsigned map_checksum;

static trace_t oracle_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end)
{
	trace_t trace = CM_BoxTrace(start, end, mins, maxs, 0, MASK_PLAYERSOLID);
	if (trace.fraction < 1.0f || trace.startsolid)
		trace.ent = (struct edict_s *)1;
	return trace;
}

static int oracle_point_contents(const vec3_t point)
{
	return CM_PointContents(point, 0);
}

static int world_to_fixed(float value, short *out)
{
	long scaled = lroundf(value * 8.0f);
	if (scaled < -32768 || scaled > 32767)
		return 0;
	*out = (short)scaled;
	return 1;
}

static void fixed_vec(const short input[3], vec3_t output)
{
	for (int i = 0; i < 3; ++i) output[i] = (float)input[i] * 0.125f;
}

static int parse_state(const char *line, pmove_t *move)
{
	vec3_t origin, velocity = {0, 0, 0};
	short delta[3] = {0, 0, 0};
	int value, snap = 1;
	memset(move, 0, sizeof(*move));
	if (!Q2_JsonVec3(line, "origin", origin))
		return 0;
	Q2_JsonVec3(line, "velocity", velocity);
	for (int i = 0; i < 3; ++i) {
		if (!world_to_fixed(origin[i], &move->s.origin[i]) ||
			!world_to_fixed(velocity[i], &move->s.velocity[i]))
			return 0;
	}
	move->s.pm_type = PM_NORMAL;
	move->s.gravity = 800;
	if (Q2_JsonInt(line, "pm_type", &value)) move->s.pm_type = (pmtype_t)value;
	if (Q2_JsonInt(line, "pm_flags", &value)) move->s.pm_flags = (byte)value;
	if (Q2_JsonInt(line, "pm_time", &value)) move->s.pm_time = (byte)value;
	if (Q2_JsonInt(line, "gravity", &value)) move->s.gravity = (short)value;
	if (Q2_JsonShortVec3(line, "delta_angles_short", delta))
		memcpy(move->s.delta_angles, delta, sizeof(delta));
	Q2_JsonBool(line, "snapinitial", &snap);
	move->snapinitial = snap ? true : false;
	move->trace = oracle_trace;
	move->pointcontents = oracle_point_contents;
	return move->s.pm_type >= PM_NORMAL && move->s.pm_type <= PM_FREEZE &&
		move->s.gravity >= 0;
}

static int parse_command(const char *json, usercmd_t *command)
{
	vec3_t angles;
	short angles_short[3];
	int value;
	memset(command, 0, sizeof(*command));
	if (!Q2_JsonInt(json, "msec", &value) || value < 1 || value > 255)
		return 0;
	command->msec = (byte)value;
	if (Q2_JsonShortVec3(json, "angles_short", angles_short)) {
		memcpy(command->angles, angles_short, sizeof(angles_short));
	} else if (Q2_JsonVec3(json, "angles", angles)) {
		for (int i = 0; i < 3; ++i) command->angles[i] = ANGLE2SHORT(angles[i]);
	}
	if (Q2_JsonInt(json, "forwardmove", &value)) {
		if (value < -32768 || value > 32767) return 0;
		command->forwardmove = (short)value;
	}
	if (Q2_JsonInt(json, "sidemove", &value)) {
		if (value < -32768 || value > 32767) return 0;
		command->sidemove = (short)value;
	}
	if (Q2_JsonInt(json, "upmove", &value)) {
		if (value < -32768 || value > 32767) return 0;
		command->upmove = (short)value;
	}
	if (Q2_JsonInt(json, "buttons", &value)) command->buttons = (byte)value;
	if (Q2_JsonInt(json, "impulse", &value)) command->impulse = (byte)value;
	if (Q2_JsonInt(json, "lightlevel", &value)) command->lightlevel = (byte)value;
	return 1;
}

static void print_state(const pmove_t *move, int command_index)
{
	vec3_t origin, velocity;
	fixed_vec(move->s.origin, origin);
	fixed_vec(move->s.velocity, velocity);
	fprintf(stdout, "{\"command_index\":%d,\"origin\":", command_index);
	Q2_OraclePrintVec3(stdout, origin);
	fputs(",\"velocity\":", stdout); Q2_OraclePrintVec3(stdout, velocity);
	fprintf(stdout, ",\"origin_fixed\":[%d,%d,%d],\"velocity_fixed\":[%d,%d,%d],"
		"\"pm_type\":%d,\"pm_flags\":%u,\"pm_time\":%u,\"gravity\":%d,"
		"\"viewangles\":",
		move->s.origin[0], move->s.origin[1], move->s.origin[2],
		move->s.velocity[0], move->s.velocity[1], move->s.velocity[2],
		move->s.pm_type, move->s.pm_flags, move->s.pm_time, move->s.gravity);
	Q2_OraclePrintVec3(stdout, move->viewangles);
	fprintf(stdout, ",\"viewheight\":%.9g,\"mins\":", move->viewheight);
	Q2_OraclePrintVec3(stdout, move->mins);
	fputs(",\"maxs\":", stdout); Q2_OraclePrintVec3(stdout, move->maxs);
	fprintf(stdout, ",\"grounded\":%s,\"waterlevel\":%d,\"watertype\":%d,\"touch_count\":%d}",
		move->groundentity ? "true" : "false", move->waterlevel,
		move->watertype, move->numtouch);
}

static void print_identity(const char *id, int gravity, float airaccelerate,
	const char *physics_identity)
{
	fputs("{\"ok\":true,\"id\":", stdout); Q2_OraclePrintString(stdout, id);
	fputs(",\"op\":\"identity\",\"schema\":\"q2-pmove-oracle-v1\",\"physics_identity\":", stdout);
	Q2_OraclePrintString(stdout, physics_identity);
	fputs(",\"map_sha256\":", stdout); Q2_OraclePrintString(stdout, map_sha256);
	fprintf(stdout, ",\"map_checksum\":%u,\"parameters\":{\"gravity\":%d,"
		"\"airaccelerate\":%.9g,\"constants\":", map_checksum, gravity, airaccelerate);
	Q2_OraclePrintString(stdout, Q2_PMOVE_CONSTANTS);
	fputs("},\"source\":{\"collision_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_COLLISION_SOURCE_SHA256);
	fputs(",\"pmove_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_PMOVE_SOURCE_SHA256);
	fputs(",\"shared_header_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_SHARED_HEADER_SHA256);
	fputs(",\"shared_source_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_SHARED_SOURCE_SHA256);
	fputs(",\"build_contract\":", stdout); Q2_OraclePrintString(stdout, Q2_ORACLE_BUILD_CONTRACT);
	fputs("}}\n", stdout);
}

static void handle_line(const char *line)
{
	char id[128] = "", op[48] = "";
	char command_json[4096];
	const char *cursor, *end;
	pmove_t move;
	usercmd_t *commands = NULL;
	double air = 0.0;
	char identity[65];
	int command_count = 0, next;
	Q2_JsonString(line, "id", id, sizeof(id));
	if (!Q2_JsonString(line, "op", op, sizeof(op))) {
		Q2_OraclePrintError(id, "invalid_request", "op string is required");
		return;
	}
	if (strcmp(op, "identity") == 0) {
		int gravity = 800;
		Q2_JsonInt(line, "gravity", &gravity);
		Q2_JsonNumber(line, "airaccelerate", &air);
		Q2_PmoveIdentity(map_sha256, gravity, (float)air, identity);
		print_identity(id, gravity, (float)air, identity);
		return;
	}
	if (strcmp(op, "simulate") != 0) {
		Q2_OraclePrintError(id, "unknown_operation", op);
		return;
	}
	if (!parse_state(line, &move)) {
		Q2_OraclePrintError(id, "invalid_state", "origin/fixed state is invalid or outside pmove range");
		return;
	}
	if (Q2_JsonNumber(line, "airaccelerate", &air)) pm_airaccelerate = (float)air;
	else pm_airaccelerate = 0.0f;
	if (!Q2_JsonArray(line, "commands", &cursor, &end)) {
		Q2_OraclePrintError(id, "invalid_commands", "commands array is required");
		return;
	}
	commands = calloc(Q2_ORACLE_MAX_COMMANDS, sizeof(*commands));
	if (!commands) {
		Q2_OraclePrintError(id, "out_of_memory", "cannot allocate command batch");
		return;
	}
	while ((next = Q2_JsonNextObject(&cursor, end, command_json, sizeof(command_json))) > 0) {
		if (command_count >= Q2_ORACLE_MAX_COMMANDS) {
			free(commands);
			Q2_OraclePrintError(id, "too_many_commands", "at most 4096 commands are accepted");
			return;
		}
		if (!parse_command(command_json, &commands[command_count])) {
			free(commands);
			Q2_OraclePrintError(id, "invalid_command", command_json);
			return;
		}
		++command_count;
	}
	if (next < 0) {
		free(commands);
		Q2_OraclePrintError(id, "invalid_commands", "commands contains a malformed object");
		return;
	}
	Q2_PmoveIdentity(map_sha256, move.s.gravity, pm_airaccelerate, identity);
	fputs("{\"ok\":true,\"id\":", stdout); Q2_OraclePrintString(stdout, id);
	fputs(",\"op\":\"simulate\",\"schema\":\"q2-pmove-oracle-v1\",\"physics_identity\":", stdout);
	Q2_OraclePrintString(stdout, identity);
	fputs(",\"frames\":[", stdout);
	for (int i = 0; i < command_count; ++i) {
		move.cmd = commands[i];
		Pmove(&move);
		move.snapinitial = false;
		if (i) fputc(',', stdout);
		print_state(&move, i);
	}
	fputs("],\"final\":", stdout); print_state(&move, command_count ? command_count - 1 : -1);
	fprintf(stdout, ",\"command_count\":%d}\n", command_count);
	free(commands);
}

int main(int argc, char **argv)
{
	char *line;
	if (argc != 3 || strcmp(argv[1], "--map") != 0) {
		fprintf(stderr, "usage: %s --map PATH.bsp\n", argv[0]);
		return 64;
	}
	map_path = argv[2];
	Q2_OracleSetProgram("q2-pmove-oracle");
	Q2_OracleInitEndian();
	if (!Q2_OracleFileSHA256(map_path, map_sha256)) {
		fprintf(stderr, "q2-pmove-oracle: cannot read %s: %s\n", map_path, strerror(errno));
		return 66;
	}
	CM_LoadMap(map_path, false, &map_checksum);
	line = malloc(Q2_ORACLE_MAX_LINE);
	if (!line) return 70;
	while (fgets(line, Q2_ORACLE_MAX_LINE, stdin)) {
		if (!strchr(line, '\n') && !feof(stdin)) {
			Q2_OraclePrintError("", "request_too_large", "NDJSON line exceeds one MiB");
			break;
		}
		handle_line(line);
		fflush(stdout);
	}
	free(line);
	return ferror(stdin) ? 74 : 0;
}
