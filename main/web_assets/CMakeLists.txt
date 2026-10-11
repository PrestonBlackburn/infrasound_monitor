if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(WEB_TEMPLATES  ${PROJECT_DIR}/templates)
    set(WEB_STATIC     ${PROJECT_DIR}/static)
    set(WEB_TOOLS      ${PROJECT_DIR}/tools/web)
    set(WEB_OUT        ${CMAKE_CURRENT_BINARY_DIR}/web_out)

    # files that should trigger a re-render
    file(GLOB_RECURSE WEB_INPUTS CONFIGURE_DEPENDS
        ${WEB_TEMPLATES}/* ${WEB_STATIC}/* ${WEB_TOOLS}/*.py
        ${PROJECT_DIR}/main/state/state_struct.h)

    # Outputs
    set(WEB_GZ ${WEB_OUT}/live_demo.html.gz)
    file(GLOB_RECURSE STATIC_FILES CONFIGURE_DEPENDS ${WEB_STATIC}/*)
    foreach(f ${STATIC_FILES})
        get_filename_component(name ${f} NAME)
        list(APPEND WEB_GZ ${WEB_OUT}/${name}.gz)
    endforeach()


    set(WEB_PYTHON ${PROJECT_DIR}/.venv/bin/python)
    if(NOT EXISTS ${WEB_PYTHON})
        set(WEB_PYTHON ${PYTHON})   # fall back to IDF's Python
    endif()

    add_custom_command(
        OUTPUT ${WEB_GZ}
        COMMAND ${WEB_PYTHON} ${WEB_TOOLS}/build_web.py ${WEB_TEMPLATES} ${WEB_STATIC} ${WEB_OUT}
        DEPENDS ${WEB_INPUTS}
        COMMENT "Rendering web assets"
        VERBATIM
    )
endif()

idf_component_register(
    SRCS "web_assets.c"
    INCLUDE_DIRS "."
    EMBED_FILES ${WEB_GZ}
)