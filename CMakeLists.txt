cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(TARGET pdf2jpeg)

project(${TARGET})

find_library(LIB_POPPLER poppler)
find_library(LIB_POPPLER_CPP poppler-cpp)
find_library(LIB_BOOST_OPT boost_program_options)
find_library(LIB_PTHREAD pthread)

add_executable(${TARGET} ${TARGET}.cxx)

target_compile_features(${TARGET} PUBLIC cxx_std_17)

target_link_libraries(${TARGET} PRIVATE stdc++fs ${LIB_PTHREAD} ${LIB_POPPLER} ${LIB_POPPLER_CPP} ${LIB_BOOST_OPT})

# Add "tags" target and make my_project depending on this target.
set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags
    COMMAND ctags --c++-kinds=+p --fields=+iaS --extra=+q .
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_dependencies(${TARGET} tags)

# vi:set tabstop=8 softtabstop=4 shiftwidth=4 noexpandtab nowrap:
