## Quatra Arkansas — ESP-IDF component (v1.1, hardware-agnostic library)
##
## Scope per PRD v1.1 amendments §2:
##   - irrigation algorithm (pure math)
##   - webapp JSON (de)serialization (string ops only, no UART driver)
##
## Drop this directory into the EXTRA_COMPONENT_DIRS of your project (or
## simply place it under `components/` in your IDF project root) and add
## `quatra` to REQUIRES of the component that calls it.

idf_component_register(
    SRCS
        "src/quatra_et0.c"
        "src/quatra_zone.c"
        "src/quatra_uart.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        json    # cJSON — the only external dependency
)
