cmake_minimum_required(VERSION 3.18) # Specify the minimum required CMake version
project(poc_cache_hiredis VERSION 1.0 LANGUAGES CXX) # Define the project name, version, and language

# Put this here so ctest can be run from the build dir. jhrg 106/25
enable_testing()

# Add the subdirectory containing the C++ code and its CMakeLists.txt
add_subdirectory(Cpp) # Assuming 'src' is the subdirectory containing your C++ code
