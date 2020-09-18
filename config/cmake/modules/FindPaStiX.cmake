# Pastix lib requires linking to a blas library.
# It is up to the user of this module to find a BLAS and link to it.
# Pastix requires SCOTCH or METIS (partitioning and reordering tools) as well

if (PASTIX_INCLUDE_DIRS AND PASTIX_LIBRARIES)
  set(PASTIX_FIND_QUIETLY TRUE)
endif (PASTIX_INCLUDE_DIRS AND PASTIX_LIBRARIES)

find_path(PASTIX_INCLUDE_DIRS
  NAMES
  pastix.h
  PATHS
  ${PASTIX_DIR}/include
  ${INCLUDE_INSTALL_DIR}
)

find_library(PASTIX_LIBRARIES pastix PATHS ${PASTIX_DIR}/lib ${LIB_INSTALL_DIR})



include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PASTIX DEFAULT_MSG
                                  PASTIX_INCLUDE_DIRS PASTIX_LIBRARIES)

mark_as_advanced(PASTIX_INCLUDES PASTIX_LIBRARIES)