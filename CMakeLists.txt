cmake_minimum_required ( VERSION 2.6 )
project ( lxc-wold )

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin/)


LINK_DIRECTORIES(/usr/lib/lxc)

ADD_EXECUTABLE(lxc-wold lxc-wold.c lxc/arguments.c)

TARGET_LINK_LIBRARIES(lxc-wold lxc)


# Does break linkage, therefore disabled...
#INSTALL(TARGETS lxc-wold DESTINATION bin)