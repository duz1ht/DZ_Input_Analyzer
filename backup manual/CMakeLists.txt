cmake_minimum_required(VERSION 3.28...3.30)

add_library(dz-input-analyzer MODULE
  dz-input-analyzer.cpp
)

target_link_libraries(dz-input-analyzer PRIVATE OBS::libobs)

set_target_properties(dz-input-analyzer PROPERTIES
  FOLDER "plugins"
)

# Install into OBS rundir when building OBS on Windows
if(OS_WINDOWS)
  install(
    TARGETS dz-input-analyzer
    RUNTIME DESTINATION obs-plugins/64bit
    LIBRARY DESTINATION obs-plugins/64bit
    ARCHIVE DESTINATION obs-plugins/64bit
  )
endif()
