# CMake generated Testfile for 
# Source directory: C:/src/DeskBeam/tests
# Build directory: C:/src/DeskBeam/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[capture_test]=] "C:/src/DeskBeam/build/bin/Debug/capture_test.exe")
  set_tests_properties([=[capture_test]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/src/DeskBeam/tests/CMakeLists.txt;8;add_test;C:/src/DeskBeam/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[capture_test]=] "C:/src/DeskBeam/build/bin/Release/capture_test.exe")
  set_tests_properties([=[capture_test]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/src/DeskBeam/tests/CMakeLists.txt;8;add_test;C:/src/DeskBeam/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[capture_test]=] "C:/src/DeskBeam/build/bin/MinSizeRel/capture_test.exe")
  set_tests_properties([=[capture_test]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/src/DeskBeam/tests/CMakeLists.txt;8;add_test;C:/src/DeskBeam/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[capture_test]=] "C:/src/DeskBeam/build/bin/RelWithDebInfo/capture_test.exe")
  set_tests_properties([=[capture_test]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/src/DeskBeam/tests/CMakeLists.txt;8;add_test;C:/src/DeskBeam/tests/CMakeLists.txt;0;")
else()
  add_test([=[capture_test]=] NOT_AVAILABLE)
endif()
