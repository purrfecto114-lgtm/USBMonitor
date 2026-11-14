#include "usbmon.h"
#include <string.h>

static int count, failures;
#define CHECK(expr) do { count++; if (!(expr)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

int main(void)
{
    static const char plug[] = "add@/devices/example\0ACTION=add\0SUBSYSTEM=block";
    static const char remove[] = "remove@/devices/example\0SUBSYSTEM=block\0ACTION=remove";
    static const char change[] = "change@/devices/example\0ACTION=change\0SUBSYSTEM=block";
    static const char usb[] = "add@/devices/example\0ACTION=add\0SUBSYSTEM=usb";
    static const char bind[] = "bind@/devices/example\0ACTION=bind\0SUBSYSTEM=block";
    char escaped[40];
    jval *j;

    CHECK(um_hot_block_event(plug, sizeof plug));
    CHECK(um_hot_block_event(remove, sizeof remove));
    CHECK(um_hot_block_event(change, sizeof change));
    CHECK(!um_hot_block_event(plug, sizeof plug - 1));
    CHECK(!um_hot_block_event(usb, sizeof usb));
    CHECK(!um_hot_block_event(bind, sizeof bind));
    CHECK(!um_hot_block_event("", 0));

    j = json_parse("\"中\\ud801\\udc37\"");
    CHECK(j && !strcmp(j->str, "中𐐷"));
    json_free(j);
    j = json_parse("-2.75e3");
    CHECK(j && j->num == -2750);
    json_free(j);
    CHECK(json_parse("01") == NULL);
    CHECK(json_parse("-0x1") == NULL);
    CHECK(json_parse("1.") == NULL);
    CHECK(json_parse("1e+") == NULL);
    CHECK(json_parse("1e999") == NULL);
    CHECK(json_parse("\"\\u0000\"") == NULL);
    CHECK(json_parse("\"\xff\"") == NULL);
    um_json_escape("汉\xff\x01", escaped, sizeof escaped);
    CHECK(!strcmp(escaped, "汉\\ufffd\\u0001"));
    um_json_escape("汉", escaped, 3);
    CHECK(!escaped[0]);
    printf("%d checks, %d failures\n", count, failures);
    return failures != 0;
}
