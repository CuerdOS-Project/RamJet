#include "../src/class.h"
#include "../src/parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_classes(void) {
    IoClass c;
    assert(io_class_from_string("idle", &c) == 0 && io_class_to_num(c) == 3);
    assert(io_class_from_string("best-effort", &c) == 0 && io_class_to_num(c) == 2);
    assert(io_class_from_string("realtime", &c) == 0 && io_class_to_num(c) == 1);
}

static void test_json(void) {
    const char *line = "{ \"name\": \"example\", \"nice\": -20, \"ioclass\": \"best-effort\", \"ionice\": 7 }";
    char name[32];
    long value;
    assert(json_get_string(line, "name", name, sizeof(name)) == 1);
    assert(strcmp(name, "example") == 0);
    assert(json_get_int(line, "nice", &value) == 1 && value == -20);
    assert(json_get_int(line, "ionice", &value) == 1 && value == 7);
    assert(json_get_int(line, "missing", &value) == 0);

    assert(json_get_string("{\"name\":\"a\\b\"}", "name", name, sizeof(name)) == 1);
    assert(strcmp(name, "a\b") == 0);
}

int main(void) {
    test_classes();
    test_json();
    puts("RamJet core tests: OK");
    return 0;
}
