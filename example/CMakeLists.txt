include_directories(
   ${WLC_INCLUDE_DIRS}
   ${CHCK_INCLUDE_DIRS}
   ${XKBCOMMON_INCLUDE_DIRS}
)
add_executable(example example.c)
target_link_libraries(example PRIVATE ${WLC_LIBRARIES})
add_executable(notile notile.c)
target_link_libraries(notile PRIVATE ${WLC_LIBRARIES})
