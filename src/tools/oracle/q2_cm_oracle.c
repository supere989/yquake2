#include "q2_oracle.h"
#include "oracle_identity.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *map_path;
static char map_sha256[65];
static char physics_identity[65];
static unsigned map_checksum;
static cmodel_t *world_model;

static void print_prefix(const char *id, const char *op)
{
	fputs("{\"ok\":true,\"id\":", stdout); Q2_OraclePrintString(stdout, id);
	fputs(",\"op\":", stdout); Q2_OraclePrintString(stdout, op);
	fputs(",\"schema\":\"q2-cm-oracle-v1\",\"tool_identity\":", stdout);
	Q2_OraclePrintString(stdout, Q2_TOOL_IDENTITY_SHA256);
	fputs(",\"physics_identity\":", stdout); Q2_OraclePrintString(stdout, physics_identity);
	fputs(",\"map_sha256\":", stdout); Q2_OraclePrintString(stdout, map_sha256);
	fprintf(stdout, ",\"map_checksum\":%u", map_checksum);
}

static int request_header(const char *line, q2_oracle_request_t *request)
{
	memset(request, 0, sizeof(*request));
	if (!Q2_JsonString(line, "op", request->op, sizeof(request->op)))
		return 0;
	if (!Q2_JsonString(line, "id", request->id, sizeof(request->id)))
		Q_strlcpy(request->id, "", sizeof(request->id));
	return 1;
}

static void print_identity_fields(void)
{
	fputs(",\"provenance\":", stdout); Q2_OraclePrintToolProvenance(stdout);
	fputs(",\"source\":{", stdout);
	fputs("\"collision_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_COLLISION_SOURCE_SHA256);
	fputs(",\"shared_header_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_SHARED_HEADER_SHA256);
	fputs(",\"shared_source_sha256\":", stdout); Q2_OraclePrintString(stdout, Q2_SHARED_SOURCE_SHA256);
	fputc('}', stdout);
}

static void handle_map_info(const q2_oracle_request_t *request)
{
	print_prefix(request->id, request->op);
	print_identity_fields();
	fputs(",\"map\":", stdout); Q2_OraclePrintString(stdout, map_path);
	fputs(",\"model0\":{\"mins\":", stdout); Q2_OraclePrintVec3(stdout, world_model->mins);
	fputs(",\"maxs\":", stdout); Q2_OraclePrintVec3(stdout, world_model->maxs);
	fprintf(stdout, ",\"headnode\":%d},\"clusters\":%d,\"inline_models\":%d}\n",
		world_model->headnode, CM_NumClusters(), CM_NumInlineModels());
}

static void handle_point_contents(const char *line, const q2_oracle_request_t *request)
{
	vec3_t point;
	int headnode = 0;
	if (!Q2_JsonVec3(line, "point", point)) {
		Q2_OraclePrintError(request->id, "invalid_point", "point must be three finite numbers");
		return;
	}
	Q2_JsonInt(line, "headnode", &headnode);
	print_prefix(request->id, request->op);
	fputs(",\"point\":", stdout); Q2_OraclePrintVec3(stdout, point);
	fprintf(stdout, ",\"headnode\":%d,\"contents\":%d}\n",
		headnode, CM_PointContents(point, headnode));
}

static void handle_point_cluster(const char *line, const q2_oracle_request_t *request)
{
	vec3_t point;
	int leaf;
	if (!Q2_JsonVec3(line, "point", point)) {
		Q2_OraclePrintError(request->id, "invalid_point", "point must be three finite numbers");
		return;
	}
	leaf = CM_PointLeafnum(point);
	print_prefix(request->id, request->op);
	fputs(",\"point\":", stdout); Q2_OraclePrintVec3(stdout, point);
	fprintf(stdout, ",\"leaf\":%d,\"cluster\":%d,\"area\":%d,\"contents\":%d}\n",
		leaf, CM_LeafCluster(leaf), CM_LeafArea(leaf), CM_LeafContents(leaf));
}

static void handle_box_trace(const char *line, const q2_oracle_request_t *request)
{
	vec3_t start, end, mins, maxs;
	trace_t trace;
	int headnode = 0, mask = MASK_PLAYERSOLID;
	if (!Q2_JsonVec3(line, "start", start) || !Q2_JsonVec3(line, "end", end) ||
		!Q2_JsonVec3(line, "mins", mins) || !Q2_JsonVec3(line, "maxs", maxs)) {
		Q2_OraclePrintError(request->id, "invalid_trace", "start/end/mins/maxs are required vec3 values");
		return;
	}
	Q2_JsonInt(line, "headnode", &headnode);
	Q2_JsonInt(line, "mask", &mask);
	trace = CM_BoxTrace(start, end, mins, maxs, headnode, mask);
	print_prefix(request->id, request->op);
	fprintf(stdout, ",\"headnode\":%d,\"mask\":%d,\"fraction\":%.9g,"
		"\"allsolid\":%s,\"startsolid\":%s,\"endpos\":",
		headnode, mask, trace.fraction, trace.allsolid ? "true" : "false",
		trace.startsolid ? "true" : "false");
	Q2_OraclePrintVec3(stdout, trace.endpos);
	fputs(",\"plane\":{\"normal\":", stdout); Q2_OraclePrintVec3(stdout, trace.plane.normal);
	fprintf(stdout, ",\"dist\":%.9g,\"type\":%u,\"signbits\":%u},\"contents\":%d",
		trace.plane.dist, trace.plane.type, trace.plane.signbits, trace.contents);
	if (trace.surface) {
		fputs(",\"surface\":{\"name\":", stdout); Q2_OraclePrintString(stdout, trace.surface->name);
		fprintf(stdout, ",\"flags\":%d,\"value\":%d}", trace.surface->flags, trace.surface->value);
	} else {
		fputs(",\"surface\":null", stdout);
	}
	fputs("}\n", stdout);
}

static void handle_pvs(const char *line, const q2_oracle_request_t *request)
{
	int from_cluster, to_cluster;
	vec3_t from, to;
	byte *bits;
	if (!Q2_JsonInt(line, "from_cluster", &from_cluster)) {
		if (!Q2_JsonVec3(line, "from", from)) {
			Q2_OraclePrintError(request->id, "invalid_pvs", "from_cluster or from point is required");
			return;
		}
		from_cluster = CM_LeafCluster(CM_PointLeafnum(from));
	}
	if (!Q2_JsonInt(line, "to_cluster", &to_cluster)) {
		if (!Q2_JsonVec3(line, "to", to)) {
			Q2_OraclePrintError(request->id, "invalid_pvs", "to_cluster or to point is required");
			return;
		}
		to_cluster = CM_LeafCluster(CM_PointLeafnum(to));
	}
	if (from_cluster < -1 || from_cluster >= CM_NumClusters() ||
		to_cluster < -1 || to_cluster >= CM_NumClusters()) {
		Q2_OraclePrintError(request->id, "cluster_range", "cluster is outside the loaded map");
		return;
	}
	bits = CM_ClusterPVS(from_cluster);
	print_prefix(request->id, request->op);
	fprintf(stdout, ",\"from_cluster\":%d,\"to_cluster\":%d,\"potentially_visible\":%s}\n",
		from_cluster, to_cluster,
		(to_cluster >= 0 && (bits[to_cluster >> 3] & (1 << (to_cluster & 7)))) ? "true" : "false");
}

static void handle_portal(const char *line, const q2_oracle_request_t *request)
{
	int portal, open;
	if (!Q2_JsonInt(line, "portal", &portal) || !Q2_JsonBool(line, "open", &open)) {
		Q2_OraclePrintError(request->id, "invalid_portal", "portal integer and open boolean are required");
		return;
	}
	CM_SetAreaPortalState(portal, open ? true : false);
	print_prefix(request->id, request->op);
	fprintf(stdout, ",\"portal\":%d,\"open\":%s}\n", portal, open ? "true" : "false");
}

static void handle_areas(const char *line, const q2_oracle_request_t *request)
{
	int area1, area2;
	if (!Q2_JsonInt(line, "area1", &area1) || !Q2_JsonInt(line, "area2", &area2)) {
		Q2_OraclePrintError(request->id, "invalid_areas", "area1 and area2 are required integers");
		return;
	}
	print_prefix(request->id, request->op);
	fprintf(stdout, ",\"area1\":%d,\"area2\":%d,\"connected\":%s}\n",
		area1, area2, CM_AreasConnected(area1, area2) ? "true" : "false");
}

static void handle_line(const char *line)
{
	q2_oracle_request_t request;
	if (!request_header(line, &request)) {
		Q2_OraclePrintError("", "invalid_request", "op string is required");
		return;
	}
	if (strcmp(request.op, "identity") == 0 || strcmp(request.op, "map_info") == 0)
		handle_map_info(&request);
	else if (strcmp(request.op, "point_contents") == 0)
		handle_point_contents(line, &request);
	else if (strcmp(request.op, "point_cluster") == 0)
		handle_point_cluster(line, &request);
	else if (strcmp(request.op, "box_trace") == 0)
		handle_box_trace(line, &request);
	else if (strcmp(request.op, "pvs") == 0)
		handle_pvs(line, &request);
	else if (strcmp(request.op, "set_areaportal") == 0)
		handle_portal(line, &request);
	else if (strcmp(request.op, "areas_connected") == 0)
		handle_areas(line, &request);
	else
		Q2_OraclePrintError(request.id, "unknown_operation", request.op);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	char *line;
	if (argc != 3 || strcmp(argv[1], "--map") != 0) {
		fprintf(stderr, "usage: %s --map PATH.bsp\n", argv[0]);
		return 64;
	}
	map_path = argv[2];
	Q2_OracleSetProgram("q2-cm-oracle");
	Q2_OracleInitEndian();
	if (!Q2_OracleFileSHA256(map_path, map_sha256)) {
		fprintf(stderr, "q2-cm-oracle: cannot read %s: %s\n", map_path, strerror(errno));
		return 66;
	}
	world_model = CM_LoadMap(map_path, false, &map_checksum);
	Q2_CMIdentity(map_sha256, physics_identity);
	line = malloc(Q2_ORACLE_MAX_LINE);
	if (!line) return 70;
	while (fgets(line, Q2_ORACLE_MAX_LINE, stdin)) {
		if (!strchr(line, '\n') && !feof(stdin)) {
			Q2_OraclePrintError("", "request_too_large", "NDJSON line exceeds one MiB");
			break;
		}
		handle_line(line);
	}
	free(line);
	return ferror(stdin) ? 74 : 0;
}
