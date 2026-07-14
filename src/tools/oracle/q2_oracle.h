#ifndef Q2_ORACLE_H
#define Q2_ORACLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../../common/header/common.h"

#define Q2_ORACLE_SCHEMA "q2-physics-oracle-v1"
#define Q2_ORACLE_MAX_LINE (1024 * 1024)
#define Q2_ORACLE_MAX_COMMANDS 4096

typedef struct
{
	char id[128];
	char op[48];
} q2_oracle_request_t;

void Q2_OracleSetProgram(const char *program);
void Q2_OracleInitEndian(void);
int Q2_OracleFileSHA256(const char *path, char out_hex[65]);
void Q2_OracleSHA256Text(const char *text, char out_hex[65]);
void Q2_OraclePrintString(FILE *stream, const char *text);
void Q2_OraclePrintVec3(FILE *stream, const vec3_t value);
void Q2_OraclePrintError(const char *id, const char *code, const char *detail);
void Q2_OraclePrintToolProvenance(FILE *stream);

const char *Q2_JsonValue(const char *json, const char *key);
int Q2_JsonString(const char *json, const char *key, char *out, size_t out_size);
int Q2_JsonNumber(const char *json, const char *key, double *out);
int Q2_JsonInt(const char *json, const char *key, int *out);
int Q2_JsonBool(const char *json, const char *key, int *out);
int Q2_JsonVec3(const char *json, const char *key, vec3_t out);
int Q2_JsonShortVec3(const char *json, const char *key, short out[3]);
int Q2_JsonArray(const char *json, const char *key,
	const char **begin, const char **end);
int Q2_JsonNextObject(const char **cursor, const char *end,
	char *out, size_t out_size);

void Q2_CMIdentity(const char *map_sha256, char out_hex[65]);
void Q2_PmoveIdentity(const char *map_sha256, int gravity,
	float airaccelerate, char out_hex[65]);

#endif
