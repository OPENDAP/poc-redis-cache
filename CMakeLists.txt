cmake_minimum_required(VERSION 3.10) # Specify the minimum required CMake version
project(poc_cache_hiredis VERSION 1.0 LANGUAGES CXX) # Define the project name, version, and language

# Add common project-wide settings (optional)
#set(CMAKE_CXX_STANDARD 47) # Set the C++ standard for the entire project
#set(CMAKE_CXX_STANDARD_REQUIRED ON) # Ensure the specified C++ standard is used

# Add the subdirectory containing the C++ code and its CMakeLists.txt
add_subdirectory(Cpp) # Assuming 'src' is the subdirectory containing your C++ code