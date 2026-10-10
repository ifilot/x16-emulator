#include "agent_json.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void ws(char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') ++*p;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool codepoint(char **p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; ++i) {
        int h = hex(**p);
        if (h < 0) return false;
        *v = (*v << 4) | h;
        ++*p;
    }
    return true;
}
static bool string(char **p, char **result) {
    if (*(*p)++ != '"') return false;
    char *out = *p;
    *result = out;
    while (**p && **p != '"') {
        unsigned char c = *(*p)++;
        if (c < 32) return false;
        if (c == '\\') {
            c = *(*p)++;
            if (!c) return false;
            switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned u, low;
                    if (!codepoint(p, &u) || u == 0) return false;
                    if (u >= 0xd800 && u <= 0xdbff) {
                        if (**p != '\\' || (*p)[1] != 'u') return false;
                        *p += 2;
                        if (!codepoint(p, &low) || low < 0xdc00 || low > 0xdfff) return false;
                        u = 0x10000 + ((u - 0xd800) << 10) + low - 0xdc00;
                    } else if (u >= 0xdc00 && u <= 0xdfff) return false;
                    if (u < 0x80) *out++ = u;
                    else if (u < 0x800) {
                        *out++ = 0xc0 | (u >> 6); *out++ = 0x80 | (u & 63);
                    } else if (u < 0x10000) {
                        *out++ = 0xe0 | (u >> 12); *out++ = 0x80 | ((u >> 6) & 63); *out++ = 0x80 | (u & 63);
                    } else {
                        *out++ = 0xf0 | (u >> 18); *out++ = 0x80 | ((u >> 12) & 63);
                        *out++ = 0x80 | ((u >> 6) & 63); *out++ = 0x80 | (u & 63);
                    }
                    continue;
                }
                default: return false;
            }
        } else if (c >= 0x80) {
            // Validate raw UTF-8 as well as escaped Unicode.
            unsigned u = c, min;
            int n;
            if (c >= 0xc2 && c <= 0xdf) { n = 1; u &= 31; min = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { n = 2; u &= 15; min = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { n = 3; u &= 7; min = 0x10000; }
            else return false;
            *out++ = c;
            for (int i = 0; i < n; ++i) {
                c = **p;
                if ((c & 0xc0) != 0x80) return false;
                ++*p; *out++ = c; u = (u << 6) | (c & 63);
            }
            if (u < min || u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff)) return false;
            continue;
        }
        *out++ = c;
    }
    if (**p != '"') return false;
    ++*p;
    *out = 0;
    return true;
}
const aj_field *aj_get(const aj_object *o, const char *key) {
    for (size_t i = 0; i < o->count; ++i)
        if (!strcmp(o->fields[i].key, key)) return &o->fields[i];
    return NULL;
}
bool aj_parse(char *p, aj_object *o) {
    o->count = 0;
    ws(&p);
    if (*p++ != '{') return false;
    ws(&p);
    if (*p == '}') { ++p; ws(&p); return !*p; }
    for (;;) {
        if (o->count == AGENT_JSON_FIELDS || *p != '"') return false;
        aj_field f = {0};
        if (!string(&p, &f.key) || aj_get(o, f.key)) return false;
        ws(&p);
        if (*p++ != ':') return false;
        ws(&p);
        if (*p == '"') {
            f.type = AJ_STRING;
            if (!string(&p, &f.string)) return false;
        } else if (*p == '-' || isdigit((unsigned char)*p)) {
            char *start = p;
            if (*p == '-') ++p;
            if (!isdigit((unsigned char)*p)) return false;
            if (*p == '0') ++p;
            else while (isdigit((unsigned char)*p)) ++p;
            if (*p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return false;
            errno = 0;
            char *end;
            f.integer = strtoll(start, &end, 10);
            if (errno || end != p || f.integer > INT64_C(9007199254740991) || f.integer < -INT64_C(9007199254740991)) return false;
            f.type = AJ_INTEGER;
        } else if (!strncmp(p, "true", 4)) { f.type = AJ_BOOL; f.integer = 1; p += 4; }
        else if (!strncmp(p, "false", 5)) { f.type = AJ_BOOL; p += 5; }
        else if (!strncmp(p, "null", 4)) { f.type = AJ_NULL; p += 4; }
        else return false;
        o->fields[o->count++] = f;
        ws(&p);
        if (*p == '}') { ++p; ws(&p); return !*p; }
        if (*p++ != ',') return false;
        ws(&p);
    }
}
bool aj_allowed(const aj_object *o, const char *keys) {
    for (size_t i = 0; i < o->count; ++i) {
        const char *p = keys;
        bool found = false;
        while (*p) {
            while (*p == ' ') ++p;
            const char *start = p;
            while (*p && *p != ' ') ++p;
            if ((size_t)(p - start) == strlen(o->fields[i].key) && !strncmp(start, o->fields[i].key, p - start)) found = true;
        }
        if (!found) return false;
    }
    return true;
}
