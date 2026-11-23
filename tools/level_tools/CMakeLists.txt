add_executable(level_dump level_dump/main.cpp)
target_link_libraries(level_dump fmt common decomp)

# merc_dump (AI-assisted): offline merc-model census for a fr3 (host only; tools/ is
# not built for NINTENDO_SWITCH). See merc_dump/main.cpp.
add_executable(merc_dump merc_dump/main.cpp)
target_link_libraries(merc_dump fmt common)
