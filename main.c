#include "flint/flint.h"
#include <stdio.h>

// flint entry point
int main(void) {
    fl_app_t app;
    fl_result_t res = fl_app_create(&app);
    if (res != FL_OK) {
        printf("fl_app_create failed: %d\n", res);
        return 1;
    }
    printf("fl_app_create succeeded\n");
    fl_app_destroy(app);
    printf("fl_app_destroy succeeded\n");
    return 0;
}
