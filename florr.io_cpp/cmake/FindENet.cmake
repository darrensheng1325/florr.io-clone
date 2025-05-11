# FindENet.cmake
# Finds the ENet library
#
# This will define the following variables:
#
#   ENet_FOUND        - True if the system has ENet
#   ENet_INCLUDE_DIRS - ENet include directory
#   ENet_LIBRARIES    - ENet libraries
#   ENet_VERSION      - ENet version

include(FindPackageHandleStandardArgs)

# Try to find ENet using pkg-config
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_ENet QUIET enet)
endif()

# Find include directory
find_path(ENet_INCLUDE_DIR
    NAMES enet/enet.h
    PATHS
        ${PC_ENet_INCLUDE_DIRS}
        /usr/include
        /usr/local/include
        /opt/homebrew/include
    PATH_SUFFIXES enet
)

# Find library
find_library(ENet_LIBRARY
    NAMES enet
    PATHS
        ${PC_ENet_LIBRARY_DIRS}
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
)

# Set version
if(PC_ENet_VERSION)
    set(ENet_VERSION ${PC_ENet_VERSION})
endif()

# Handle the QUIETLY and REQUIRED arguments and set ENet_FOUND
find_package_handle_standard_args(ENet
    REQUIRED_VARS ENet_LIBRARY ENet_INCLUDE_DIR
    VERSION_VAR ENet_VERSION
)

if(ENet_FOUND)
    set(ENet_LIBRARIES ${ENet_LIBRARY})
    set(ENet_INCLUDE_DIRS ${ENet_INCLUDE_DIR})
endif()

mark_as_advanced(ENet_INCLUDE_DIR ENet_LIBRARY) 