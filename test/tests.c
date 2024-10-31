#include <string.h>
#include "tests.h"
#include "tests-common.h"

int verbose = 0;

int
main(int argc, char **argv)
{
    run_test(list_test);
    run_test(string_test);

    return 0;
}
