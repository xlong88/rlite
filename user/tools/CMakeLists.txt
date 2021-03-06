option(MAC2IFNAME, "Include mac2ifname binary tool" OFF)

protobuf_generate_cpp(IPORINA_GPB_SRC IPORINA_GPB_HDR iporina.proto)
message(STATUS "Adding include dir ${CMAKE_CURRENT_BINARY_DIR} to iporinad target")

# Libraries
add_library(fdfwd STATIC fdfwd.cpp)
target_link_libraries(fdfwd ${CMAKE_THREAD_LIBS_INIT})

# Executables
add_executable(rinaperf rinaperf.c)
add_executable(rina-echo-async rina-echo-async.c)
add_executable(rlite-ctl rlite-ctl.c)
add_executable(rina-gw rina-gw.cpp)
add_executable(iporinad iporinad.cpp ${IPORINA_GPB_SRC} ${IPORINA_GPB_HDR})
if (MAC2IFNAME)
add_executable(mac2ifname mac2ifname.c)
endif()
add_executable(test-wifi test-wifi.c)

target_include_directories(iporinad PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(rinaperf rina-api ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(rina-echo-async rina-api)
target_link_libraries(rlite-ctl rina-api rlite-conf)
target_link_libraries(rina-gw rina-api fdfwd)
target_link_libraries(iporinad rina-api cdap fdfwd ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(test-wifi rina-api rlite-wifi)

 # Installation directives
install(TARGETS rinaperf rlite-ctl rina-gw rina-echo-async iporinad DESTINATION usr/bin)
if (MAC2IFNAME)
install(TARGETS mac2ifname DESTINATION usr/bin)
endif()
install(FILES rina-gw.conf iporinad.conf initscript DESTINATION etc/rina)
install(PROGRAMS inet-toy rina-toy rlite-node-config DESTINATION usr/bin)

add_subdirectory(rinacat)
