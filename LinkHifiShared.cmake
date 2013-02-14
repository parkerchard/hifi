MACRO(LINK_HIFI_SHARED_LIBRARY TARGET)
    if (NOT TARGET HifiShared)
        add_subdirectory(../shared ../shared)
    endif (NOT TARGET HifiShared)
    
    include_directories(../shared/src)
    get_directory_property(HIFI_SHARED_LIBRARY DIRECTORY ../shared DEFINITION HIFI_SHARED_LIBRARY)
    target_link_libraries(${TARGET} ${HIFI_SHARED_LIBRARY})
ENDMACRO(LINK_HIFI_SHARED_LIBRARY _target)