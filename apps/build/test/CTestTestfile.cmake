# CMake generated Testfile for 
# Source directory: /work/apps/test
# Build directory: /work/apps/build/test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
include("/work/apps/build/test/lwm2m_test[1]_include.cmake")
add_test(lwm2m_gtests "lwm2m_test")
set_tests_properties(lwm2m_gtests PROPERTIES  _BACKTRACE_TRIPLES "/work/apps/test/CMakeLists.txt;55;add_test;/work/apps/test/CMakeLists.txt;0;")
