// X16 AgentBridge: bounded, flat JSON requests (no external dependencies).
#ifndef AGENT_JSON_H
#define AGENT_JSON_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define AGENT_JSON_FIELDS 32
typedef enum { AJ_STRING, AJ_INTEGER, AJ_BOOL, AJ_NULL } aj_type;
typedef struct {
    char *key;
    aj_type type;
    char *string;
    int64_t integer;
} aj_field;
typedef struct {
    aj_field fields[AGENT_JSON_FIELDS];
    size_t count;
} aj_object;

// Decodes strings in place. Integers must be exact JSON-safe integers.
bool aj_parse(char *line, aj_object *object);
const aj_field *aj_get(const aj_object *object, const char *key);
bool aj_allowed(const aj_object *object, const char *keys);
#endif
