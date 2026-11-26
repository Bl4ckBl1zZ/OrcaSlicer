# Stages the macOS Linux runtime package (see CMakeLists.txt). Run with cmake -P and
#   STAGE_DIR, MODULE, SUPPORT_FILES ("|"-separated), HOST_RUNTIME_DIR, ROOTFS_TAR.
string(REPLACE "|" ";" SUPPORT_FILES "${SUPPORT_FILES}")

set(_missing "")
foreach(_name slicer_linux_runtime_host slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0)
    if (NOT EXISTS "${HOST_RUNTIME_DIR}/${_name}")
        list(APPEND _missing "${HOST_RUNTIME_DIR}/${_name}")
    endif ()
endforeach()
if (NOT EXISTS "${ROOTFS_TAR}")
    list(APPEND _missing "${ROOTFS_TAR}")
endif ()

file(REMOVE_RECURSE "${STAGE_DIR}")
if (_missing)
    message(WARNING "Bambu Linux runtime: not packaged, build the Linux side first. Missing: ${_missing}")
    return()
endif ()

file(MAKE_DIRECTORY "${STAGE_DIR}")
file(COPY "${MODULE}" ${SUPPORT_FILES} "${ROOTFS_TAR}" DESTINATION "${STAGE_DIR}")
file(GLOB _files LIST_DIRECTORIES false "${HOST_RUNTIME_DIR}/*")
foreach(_file ${_files})
    get_filename_component(_name "${_file}" NAME)
    # Never ship Bambu's proprietary libraries, even if a developer left them there.
    if (NOT _name MATCHES "^(libbambu_networking|libBambuSource|liblive555|libagora_rtc_sdk|libagora-fdkaac)[.]so$|^(linux_component_manifest|network_plugins)[.]json$")
        file(COPY "${_file}" DESTINATION "${STAGE_DIR}")
    endif ()
endforeach()
