cmake_minimum_required(VERSION 3.16)
project(ComputerMonitor VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CMON_BUNDLED_SQLITE "Compiler SQLite depuis l'amalgamation (téléchargée si absente)" OFF)
set(CMON_SQLITE_DIR "" CACHE PATH "Dossier contenant sqlite3.c / sqlite3.h (amalgamation)")

# ---------- SQLite ----------
if(NOT CMON_BUNDLED_SQLITE AND NOT WIN32)
  find_package(SQLite3 QUIET)
endif()
if(SQLite3_FOUND)
  set(CMON_SQLITE SQLite::SQLite3)
else()
  # Amalgamation : CMON_SQLITE_DIR, third_party/sqlite, ou téléchargée depuis sqlite.org
  if(CMON_SQLITE_DIR)
    set(SQLITE_DIR "${CMON_SQLITE_DIR}")
  else()
    set(SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
  endif()
  if(NOT EXISTS "${SQLITE_DIR}/sqlite3.c")
    include(FetchContent)
    FetchContent_Declare(sqlite_amalgamation
      URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip)
    FetchContent_MakeAvailable(sqlite_amalgamation)
    set(SQLITE_DIR "${sqlite_amalgamation_SOURCE_DIR}")
  endif()
  add_library(cmon_sqlite STATIC "${SQLITE_DIR}/sqlite3.c")
  target_include_directories(cmon_sqlite PUBLIC "${SQLITE_DIR}")
  target_compile_definitions(cmon_sqlite PRIVATE
    SQLITE_THREADSAFE=0 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
  if(NOT MSVC)
    target_compile_options(cmon_sqlite PRIVATE -w)
  endif()
  set(CMON_SQLITE cmon_sqlite)
endif()

# ---------- avertissements ----------
if(MSVC)
  set(CMON_WARN /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
else()
  set(CMON_WARN -Wall -Wextra -Wno-unused-parameter)
endif()
if(MINGW)
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

# ---------- code commun ----------
add_library(cmon_common STATIC src/common/cm_util.c src/common/cm_db.c)
target_link_libraries(cmon_common PUBLIC ${CMON_SQLITE})
target_compile_options(cmon_common PRIVATE ${CMON_WARN})

# ---------- tracker (arrière-plan) ----------
if(WIN32)
  add_executable(cmon-tracker WIN32 src/tracker/tracker_main.c src/tracker/platform_win.c)
  target_link_libraries(cmon-tracker PRIVATE cmon_common wtsapi32 version)
  if(MSVC)
    target_link_options(cmon-tracker PRIVATE /ENTRY:mainCRTStartup)
  endif()
else()
  find_package(X11 REQUIRED)
  add_executable(cmon-tracker src/tracker/tracker_main.c src/tracker/platform_x11.c)
  target_include_directories(cmon-tracker PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(cmon-tracker PRIVATE cmon_common ${X11_LIBRARIES})
  if(X11_Xscreensaver_FOUND)
    target_compile_definitions(cmon-tracker PRIVATE CM_HAVE_XSS)
    target_link_libraries(cmon-tracker PRIVATE ${X11_Xscreensaver_LIB})
  else()
    message(WARNING "libXss introuvable : la détection d'inactivité sera désactivée (installez libxss-dev)")
  endif()
endif()
target_compile_options(cmon-tracker PRIVATE ${CMON_WARN})

# ---------- cmon (statistiques / rapports) ----------
set(REPORT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/report_template.h")
add_custom_command(
  OUTPUT "${REPORT_HEADER}"
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/cli/report_template.html
          -DOUT=${REPORT_HEADER} -DNAME=CMON_REPORT_TEMPLATE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS src/cli/report_template.html cmake/embed.cmake
  COMMENT "Intégration du modèle de rapport HTML")
add_executable(cmon src/cli/cli_main.c src/cli/cli_common.c src/cli/cli_stats.c src/cli/cli_report.c "${REPORT_HEADER}")
target_include_directories(cmon PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(cmon PRIVATE cmon_common)
if(WIN32)
  target_link_libraries(cmon PRIVATE shell32 advapi32)
endif()
target_compile_options(cmon PRIVATE ${CMON_WARN})

install(TARGETS cmon cmon-tracker RUNTIME DESTINATION bin)
