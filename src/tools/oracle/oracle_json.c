#include "q2_oracle.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_space(const char *p)
{
	while (p && *p && isspace((unsigned char)*p))
		++p;
	return p;
}

const char *Q2_JsonValue(const char *json, const char *key)
{
	char token[128];
	const char *p;
	if (!json || !key || strlen(key) + 3 > sizeof(token))
		return NULL;
	snprintf(token, sizeof(token), "\"%s\"", key);
	p = json;
	while ((p = strstr(p, token)) != NULL) {
		const char *after = skip_space(p + strlen(token));
		if (*after == ':')
			return skip_space(after + 1);
		p += strlen(token);
	}
	return NULL;
}

int Q2_JsonString(const char *json, const char *key, char *out, size_t out_size)
{
	const char *p = Q2_JsonValue(json, key);
	size_t n = 0;
	if (!p || *p != '"' || !out || out_size == 0)
		return 0;
	++p;
	while (*p && *p != '"') {
		unsigned char ch = (unsigned char)*p++;
		if (ch == '\\') {
			ch = (unsigned char)*p++;
			switch (ch) {
			case '"': case '\\': case '/': break;
			case 'b': ch = '\b'; break;
			case 'f': ch = '\f'; break;
			case 'n': ch = '\n'; break;
			case 'r': ch = '\r'; break;
			case 't': ch = '\t'; break;
			default: return 0;
			}
		}
		if (ch < 0x20 || n + 1 >= out_size)
			return 0;
		out[n++] = (char)ch;
	}
	if (*p != '"')
		return 0;
	out[n] = '\0';
	return 1;
}

int Q2_JsonNumber(const char *json, const char *key, double *out)
{
	const char *p = Q2_JsonValue(json, key);
	char *end;
	double value;
	if (!p || !out)
		return 0;
	errno = 0;
	value = strtod(p, &end);
	end = (char *)skip_space(end);
	if (end == p || errno == ERANGE || !isfinite(value) ||
		(*end && *end != ',' && *end != '}' && *end != ']'))
		return 0;
	*out = value;
	return 1;
}

int Q2_JsonInt(const char *json, const char *key, int *out)
{
	double value;
	if (!out || !Q2_JsonNumber(json, key, &value) ||
		value < INT_MIN || value > INT_MAX || value != (double)(int)value)
		return 0;
	*out = (int)value;
	return 1;
}

int Q2_JsonBool(const char *json, const char *key, int *out)
{
	const char *p = Q2_JsonValue(json, key);
	if (!p || !out)
		return 0;
	if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
	if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
	return 0;
}

static int parse_vec3(const char *p, double out[3])
{
	char *end;
	if (!p || *skip_space(p) != '[')
		return 0;
	p = skip_space(p) + 1;
	for (int i = 0; i < 3; ++i) {
		errno = 0;
		out[i] = strtod(p, &end);
		if (end == p || errno == ERANGE || !isfinite(out[i]))
			return 0;
		p = skip_space(end);
		if (i < 2) {
			if (*p != ',') return 0;
			p = skip_space(p + 1);
		}
	}
	return *p == ']';
}

int Q2_JsonVec3(const char *json, const char *key, vec3_t out)
{
	double values[3];
	if (!out || !parse_vec3(Q2_JsonValue(json, key), values))
		return 0;
	for (int i = 0; i < 3; ++i) {
		if (values[i] < -FLT_MAX || values[i] > FLT_MAX)
			return 0;
		out[i] = (float)values[i];
		if (!isfinite(out[i]))
			return 0;
	}
	return 1;
}

int Q2_JsonShortVec3(const char *json, const char *key, short out[3])
{
	double values[3];
	if (!out || !parse_vec3(Q2_JsonValue(json, key), values))
		return 0;
	for (int i = 0; i < 3; ++i) {
		if (values[i] < SHRT_MIN || values[i] > SHRT_MAX ||
			values[i] != (double)(short)values[i])
			return 0;
		out[i] = (short)values[i];
	}
	return 1;
}

int Q2_JsonArray(const char *json, const char *key,
	const char **begin, const char **end)
{
	const char *p = Q2_JsonValue(json, key);
	int depth = 0, quoted = 0, escaped = 0;
	if (!p || *p != '[' || !begin || !end)
		return 0;
	*begin = p + 1;
	for (; *p; ++p) {
		if (quoted) {
			if (escaped) escaped = 0;
			else if (*p == '\\') escaped = 1;
			else if (*p == '"') quoted = 0;
			continue;
		}
		if (*p == '"') quoted = 1;
		else if (*p == '[') ++depth;
		else if (*p == ']' && --depth == 0) { *end = p; return 1; }
	}
	return 0;
}

int Q2_JsonNextObject(const char **cursor, const char *end,
	char *out, size_t out_size)
{
	const char *p, *start;
	int depth = 0, quoted = 0, escaped = 0;
	size_t len;
	if (!cursor || !*cursor || !end || !out || out_size == 0)
		return -1;
	p = *cursor;
	while (p < end && (isspace((unsigned char)*p) || *p == ',')) ++p;
	if (p >= end) { *cursor = p; return 0; }
	if (*p != '{') return -1;
	start = p;
	for (; p < end; ++p) {
		if (quoted) {
			if (escaped) escaped = 0;
			else if (*p == '\\') escaped = 1;
			else if (*p == '"') quoted = 0;
			continue;
		}
		if (*p == '"') quoted = 1;
		else if (*p == '{') ++depth;
		else if (*p == '}' && --depth == 0) {
			len = (size_t)(p + 1 - start);
			if (len >= out_size) return -1;
			memcpy(out, start, len);
			out[len] = '\0';
			*cursor = p + 1;
			return 1;
		}
	}
	return -1;
}
