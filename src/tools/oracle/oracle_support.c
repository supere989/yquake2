#include "q2_oracle.h"
#include "sha256.h"
#include "oracle_identity.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *oracle_program = "q2-oracle";
static cvar_t map_noareas_cvar = {"map_noareas", "0", NULL, 0, false, 0.0f, NULL, "0"};
static cvar_t flushmap_cvar = {"flushmap", "0", NULL, 0, false, 0.0f, NULL, "0"};
cvar_t *sv_entfile;

void Q2_OracleSetProgram(const char *program)
{
	if (program && *program)
		oracle_program = program;
}

void Q2_OracleInitEndian(void)
{
	Swap_Init();
	sv_entfile = &map_noareas_cvar;
}

cvar_t *Cvar_Get(const char *name, const char *value, int flags)
{
	(void)value; (void)flags;
	if (name && strcmp(name, "flushmap") == 0) return &flushmap_cvar;
	return &map_noareas_cvar;
}

float Cvar_VariableValue(const char *name)
{
	return (name && strcmp(name, "flushmap") == 0) ? flushmap_cvar.value : 0.0f;
}

int FS_LoadFile(const char *path, void **buffer)
{
	FILE *stream;
	long length;
	void *data;
	if (buffer) *buffer = NULL;
	stream = fopen(path, "rb");
	if (!stream) return -1;
	if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
		fseek(stream, 0, SEEK_SET) != 0) {
		fclose(stream); return -1;
	}
	data = malloc((size_t)length + 1);
	if (!data) { fclose(stream); return -1; }
	if (length && fread(data, 1, (size_t)length, stream) != (size_t)length) {
		free(data); fclose(stream); return -1;
	}
	((unsigned char *)data)[length] = 0;
	fclose(stream);
	if (buffer) *buffer = data; else free(data);
	return (int)length;
}

void FS_FreeFile(void *buffer) { free(buffer); }
int FS_Read(void *buffer, int len, fileHandle_t handle)
{
	(void)buffer; (void)len; (void)handle;
	return 0;
}

void Com_Printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (getenv("Q2_ORACLE_VERBOSE")) vfprintf(stderr, format, args);
	va_end(args);
}

void Com_DPrintf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (getenv("Q2_ORACLE_VERBOSE")) vfprintf(stderr, format, args);
	va_end(args);
}

void Com_Error(int code, const char *format, ...)
{
	char message[1024];
	va_list args;
	(void)code;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	fputs("{\"ok\":false,\"error\":\"engine_error\",\"program\":", stdout);
	Q2_OraclePrintString(stdout, oracle_program);
	fputs(",\"detail\":", stdout);
	Q2_OraclePrintString(stdout, message);
	fputs("}\n", stdout);
	fflush(stdout);
	exit(2);
}

void Sys_Error(const char *format, ...)
{
	char message[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	Com_Error(ERR_FATAL, "%s", message);
}

void Q2_OraclePrintString(FILE *stream, const char *text)
{
	fputc('"', stream);
	for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
		switch (*p) {
		case '"': fputs("\\\"", stream); break;
		case '\\': fputs("\\\\", stream); break;
		case '\b': fputs("\\b", stream); break;
		case '\f': fputs("\\f", stream); break;
		case '\n': fputs("\\n", stream); break;
		case '\r': fputs("\\r", stream); break;
		case '\t': fputs("\\t", stream); break;
		default:
			if (*p < 0x20) fprintf(stream, "\\u%04x", *p);
			else fputc(*p, stream);
		}
	}
	fputc('"', stream);
}

void Q2_OraclePrintVec3(FILE *stream, const vec3_t value)
{
	fprintf(stream, "[%.9g,%.9g,%.9g]", value[0], value[1], value[2]);
}

void Q2_OraclePrintError(const char *id, const char *code, const char *detail)
{
	fputs("{\"ok\":false,\"id\":", stdout); Q2_OraclePrintString(stdout, id ? id : "");
	fputs(",\"error\":", stdout); Q2_OraclePrintString(stdout, code ? code : "invalid_request");
	fputs(",\"detail\":", stdout); Q2_OraclePrintString(stdout, detail ? detail : "");
	fputs("}\n", stdout); fflush(stdout);
}

void Q2_OracleSHA256Text(const char *text, char out_hex[65])
{
	q2_sha256_ctx_t context;
	uint8_t hash[32];
	q2_sha256_init(&context);
	q2_sha256_update(&context, text, strlen(text));
	q2_sha256_final(&context, hash);
	q2_sha256_hex(hash, out_hex);
}

int Q2_OracleFileSHA256(const char *path, char out_hex[65])
{
	q2_sha256_ctx_t context;
	uint8_t hash[32], buffer[32768];
	FILE *stream = fopen(path, "rb");
	size_t count;
	if (!stream) return 0;
	q2_sha256_init(&context);
	while ((count = fread(buffer, 1, sizeof(buffer), stream)) > 0)
		q2_sha256_update(&context, buffer, count);
	if (ferror(stream)) { fclose(stream); return 0; }
	fclose(stream);
	q2_sha256_final(&context, hash);
	q2_sha256_hex(hash, out_hex);
	return 1;
}

void Q2_CMIdentity(const char *map_sha256, char out_hex[65])
{
	char canonical[1024];
	snprintf(canonical, sizeof(canonical),
		"schema=%s;kind=cm;collision=%s;shared_header=%s;shared_source=%s;build=%s;map=%s",
		Q2_ORACLE_SCHEMA, Q2_COLLISION_SOURCE_SHA256,
		Q2_SHARED_HEADER_SHA256, Q2_SHARED_SOURCE_SHA256,
		Q2_ORACLE_BUILD_CONTRACT,
		map_sha256 ? map_sha256 : "");
	Q2_OracleSHA256Text(canonical, out_hex);
}

void Q2_PmoveIdentity(const char *map_sha256, int gravity,
	float airaccelerate, char out_hex[65])
{
	char canonical[1536];
	snprintf(canonical, sizeof(canonical),
		"schema=%s;kind=pmove;collision=%s;pmove=%s;shared_header=%s;shared_source=%s;build=%s;"
		"map=%s;gravity=%d;airaccelerate=%.9g;constants=%s",
		Q2_ORACLE_SCHEMA, Q2_COLLISION_SOURCE_SHA256,
		Q2_PMOVE_SOURCE_SHA256, Q2_SHARED_HEADER_SHA256, Q2_SHARED_SOURCE_SHA256,
		Q2_ORACLE_BUILD_CONTRACT, map_sha256 ? map_sha256 : "",
		gravity, airaccelerate, Q2_PMOVE_CONSTANTS);
	Q2_OracleSHA256Text(canonical, out_hex);
}
