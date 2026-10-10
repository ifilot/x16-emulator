#include "agent_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static bool parse(const char *s) {
    char buf[4096]; aj_object o;
    strcpy(buf, s); return aj_parse(buf, &o);
}
int main(void) {
    CHECK(parse("{}"));
    CHECK(parse(" {\"cmd\":\"hello\",\"id\":-23,\"enabled\":true,\"x\":null} \n"));
    const char *bad[] = {"", "[]", "{", "{\"x\"}", "{\"x\":}", "{\"x\":1,}",
        "{\"x\":1} trailing", "{\"x\":1,\"x\":2}", "{\"x\":{}}", "{\"x\":[]}",
        "{\"x\":01}", "{\"x\":-01}", "{\"x\":1.2}", "{\"x\":1e2}",
        "{\"x\":9223372036854775808}", "{\"x\":9007199254740992}",
        "{\"x\":falsehood}", "{\"x\":\"\\q\"}", "{\"x\":\"\\u0000\"}",
        "{\"x\":\"\\ud800\"}", "{\"x\":\"\\udc00\"}", "{\"x\":\"\\u12\"}",
        "{\"x\":\"\n\"}", "{\"x\":\"\xc0\xaf\"}", "{\"x\":\"\xed\xa0\x80\"}",
        "{\"x\":\"\xf4\x90\x80\x80\"}", "{\"x\":\"\xe2\x82\"}",
        "{\"x\":\"\\", "{\"x\":\"unterminated"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) CHECK(!parse(bad[i]));
    char good[] = "{\"cmd\":\"a\\n\\t\\\\\\\"\\u00e9\\ud83d\\ude00\",\"id\":9007199254740991}";
    aj_object o;
    CHECK(aj_parse(good, &o));
    CHECK(!strcmp(aj_get(&o, "cmd")->string, "a\n\t\\\"\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(aj_get(&o, "id")->integer == 9007199254740991LL);
    CHECK(aj_allowed(&o, "id cmd")); CHECK(!aj_allowed(&o, "cmd"));
    CHECK(!aj_get(&o, "missing"));
    char fields[4096] = "{";
    for (int i = 0; i < 33; ++i) {
        char field[40]; snprintf(field, sizeof(field), "%s\"k%d\":0", i ? "," : "", i); strcat(fields, field);
    }
    strcat(fields, "}"); CHECK(!aj_parse(fields, &o));
    // Bounded malformed-input fuzzing, also run under ASan/UBSan in development.
    unsigned seed = 17;
    for (int i = 0; i < 10000; ++i) {
        char noise[80];
        for (size_t j = 0; j < sizeof(noise)-1; ++j) { seed = seed * 1664525u + 1013904223u; noise[j] = (seed >> 24) | 1; }
        noise[79] = 0; aj_parse(noise, &o);
    }
    puts("agent JSON tests passed");
    return 0;
}
