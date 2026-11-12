# CMake configuration hook for mod-twow-bot-xp-lock

if(TORTOISE_MODULE_CMAKE_PHASE STREQUAL "POST_TARGETS")

  if(TORTOISE_CURRENT_MODULE_LINKAGE STREQUAL "dynamic")
    GetModuleProjectName("${TORTOISE_CURRENT_MODULE}" BOT_XP_LOCK_TARGET)
  else()
    set(BOT_XP_LOCK_TARGET modules)
  endif()

  # ---------------------------------------------------------------------------
  # Discover TortoiseBots (Native Modular Companion Engine for tortoise-wow)
  # ---------------------------------------------------------------------------
  set(TORTOISEBOTS_FOUND FALSE)
  if(NOT DEFINED MODULE_TORTOISEBOTS OR NOT "${MODULE_TORTOISEBOTS}" STREQUAL "disabled")
    set(TORTOISEBOTS_SEARCH_PATHS
      "${CMAKE_SOURCE_DIR}/modules/TortoiseBots"
      "${CMAKE_SOURCE_DIR}/../TortoiseBots"
      "${CMAKE_CURRENT_LIST_DIR}/../TortoiseBots"
    )
    foreach(SEARCH_PATH ${TORTOISEBOTS_SEARCH_PATHS})
      if(EXISTS "${SEARCH_PATH}/runtime/BotManager.h")
        set(TORTOISEBOTS_ROOT "${SEARCH_PATH}")
        set(TORTOISEBOTS_FOUND TRUE)
        break()
      endif()
    endforeach()
  endif()

  if(TORTOISEBOTS_FOUND)
    message(STATUS "mod-twow-bot-xp-lock: Discovered TortoiseBots at ${TORTOISEBOTS_ROOT}")
    target_compile_definitions(${BOT_XP_LOCK_TARGET} PRIVATE
      HAVE_TORTOISEBOTS=1
      MANGOSBOT_ZERO=1
      CMANGOS=1
    )
    target_include_directories(${BOT_XP_LOCK_TARGET}
      PUBLIC
        "${TORTOISEBOTS_ROOT}"
        "${TORTOISEBOTS_ROOT}/ai"
        "${TORTOISEBOTS_ROOT}/ai/playerbot"
        "${TORTOISEBOTS_ROOT}/runtime"
    )
  else()
    message(STATUS "mod-twow-bot-xp-lock: Running in Standalone Core mode (Profile A)")
  endif()

endif()
