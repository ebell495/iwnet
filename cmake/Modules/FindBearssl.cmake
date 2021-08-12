mark_as_advanced(BEARSSL_INCLUDE_DIRS BEARSSL_STATIC_LIB)

find_path(BEARSSL_INCLUDE_DIRS NAMES bearssl.h)
find_library(BEARSSL_STATIC_LIB NAMES libbearssl.a)

if(BEARSSL_STATIC_LIB AND BEARSSL_INCLUDE_DIRS)
  set(BEARSSL_FOUND ON)
  set(BEARSSL_LIBRARIES ${BEARSSL_STATIC_LIB})
  message("Bearssl Found ${BEARSSL_LIBRARIES} ${BEARSSL_INCLUDE_DIRS}")
else()
  message(FATAL_ERROR "Bearssl Not Found" )
  set(BEARSSL-NOTFOUND ON)
endif()
