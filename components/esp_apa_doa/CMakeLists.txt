idf_component_register(INCLUDE_DIRS "include")

set(_esp_apa_doa_lib "${CMAKE_CURRENT_SOURCE_DIR}/lib/${IDF_TARGET}/libesp_apa_doa.a")
if(NOT EXISTS "${_esp_apa_doa_lib}")
    message(FATAL_ERROR "esp_apa_doa prebuilt library is missing: ${_esp_apa_doa_lib}")
endif()
