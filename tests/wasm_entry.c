/* SPDX-License-Identifier: MIT */
/* Exercise both C entrypoint ABIs. A loader that never runs main must fail. */
#include <stdio.h>
#include <string.h>
#ifdef ENTRY_ARGS
int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "wasm-argv"))
        return 9;
    puts(argv[1]);
    return 7;
}
#else
int main(void)
{
    puts("wasm-main");
    return 0;
}
#endif
