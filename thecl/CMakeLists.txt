find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
bison_target(EcsParse ecsparse.y ${CMAKE_CURRENT_BINARY_DIR}/ecsparse.c COMPILE_FLAGS ${BISON_FLAGS})
flex_target(EcsScan ecsscan.l ${CMAKE_CURRENT_BINARY_DIR}/ecsscan.c)
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
add_executable(gooc
  ${BISON_EcsParse_OUTPUT_SOURCE} ${FLEX_EcsScan_OUTPUTS}
  expr.c thecl.c path.c thecl10.c field.c gool_ins.c gmips.c
  expr.h thecl.h path.h gool_ins.h c1_gool.h c2_gool.h field.h gmips.h)
if(WIN32)
    set(ADDITIONAL_LIBRARIES "")
else()
    set(ADDITIONAL_LIBRARIES m)
endif()
target_link_libraries(gooc util ${ADDITIONAL_LIBRARIES})
link_setargv(gooc)
install(TARGETS gooc DESTINATION bin)
install(FILES thecl.1 DESTINATION share/man/man1)
