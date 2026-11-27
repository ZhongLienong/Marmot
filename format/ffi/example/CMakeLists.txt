cmake_minimum_required(VERSION 3.24)
project(marmot_ffi_example LANGUAGES C)

add_library(marmot_ffi_example SHARED example.c)
set_target_properties(marmot_ffi_example PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
