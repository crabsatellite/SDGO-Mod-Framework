cmake_minimum_required(VERSION 3.20)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SDGO_VERSION LIMIT_COUNT 1)
project(sdgo_mod_framework VERSION ${SDGO_VERSION} LANGUAGES C CXX RC)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(SDGO_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(SDGO_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(SDGO_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# GOnline.exe is a 32-bit process.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure for 32-bit: cmake -S . -B build -G \"Visual Studio 17 2022\" -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: d3d9.dll is a static import of the game, so it must not depend on a
# VC++ redistributable being installed on the player's machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Output is staged as two trees that mirror the install targets:
#   stage/<config>/client/  -> the game folder (next to GOnline.exe): d3d9.dll, sdgo/...
#   stage/<config>/server/  -> Battle/Agent/World host folders: winmm.dll, sdgo/...
# tools/install_client.ps1 / install_server.ps1 copy them; tools/package.ps1 zips a release.
set(SDGO_STAGE "${CMAKE_BINARY_DIR}/stage/$<CONFIG>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)

add_library(sdgo_api INTERFACE)
target_include_directories(sdgo_api INTERFACE include)

function(sdgo_stage target destination)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> "${SDGO_STAGE}/${destination}/"
    )
endfunction()

# Version resource (cmake/version.rc.in) naming the product, so installers can tell this
# project's DLLs from unrelated ones with the same file name.
function(sdgo_version_resource target filename description)
    set(RES_FILENAME "${filename}")
    set(RES_DESCRIPTION "${description}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()

function(sdgo_strict target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endfunction()

# ---- loader shims: d3d9.dll next to GOnline.exe, winmm.dll next to BattleServer.exe --
add_library(early_boot STATIC loader/common/early_boot.cpp)
target_link_libraries(early_boot PUBLIC sdgo_api)
sdgo_strict(early_boot)

add_library(d3d9_shim SHARED loader/d3d9/d3d9_shim.cpp loader/d3d9/d3d9.def)
set_target_properties(d3d9_shim PROPERTIES OUTPUT_NAME d3d9)
target_link_libraries(d3d9_shim PRIVATE early_boot)
sdgo_strict(d3d9_shim)
sdgo_version_resource(d3d9_shim d3d9.dll "SDGO mod framework loader (client)")
sdgo_stage(d3d9_shim "client")

add_library(winmm_shim SHARED loader/winmm/winmm_shim.cpp loader/winmm/winmm.def)
set_target_properties(winmm_shim PROPERTIES OUTPUT_NAME winmm)
target_link_libraries(winmm_shim PRIVATE early_boot)
sdgo_strict(winmm_shim)
sdgo_version_resource(winmm_shim winmm.dll "SDGO mod framework loader (server)")
sdgo_stage(winmm_shim "server")

# ---- core: sdgo/core.dll ----------------------------------------------------------
add_library(sdgo_core SHARED core/core.cpp core/hooks.cpp core/log.cpp core/symbols.cpp)
set_target_properties(sdgo_core PROPERTIES OUTPUT_NAME core)
target_link_libraries(sdgo_core PRIVATE sdgo_api minhook)
sdgo_strict(sdgo_core)
sdgo_version_resource(sdgo_core core.dll "SDGO mod framework core")
sdgo_stage(sdgo_core "client/sdgo")
sdgo_stage(sdgo_core "server/sdgo")

# Source-asset script registration is a linkable consumer, not another VM or a
# staged Mod. Owner/importer modules provide verified native symbols and content.
add_library(sdgo_script_registration STATIC core/script_registration.cpp core/native_script_registration.cpp)
target_link_libraries(sdgo_script_registration PUBLIC sdgo_api bcrypt)
sdgo_strict(sdgo_script_registration)
add_library(sdgo_resource_bindings STATIC core/resource_bindings.cpp core/native_resource_bindings.cpp)
target_link_libraries(sdgo_resource_bindings PUBLIC sdgo_api sdgo_script_registration)
sdgo_strict(sdgo_resource_bindings)
add_library(sdgo_skill_identity STATIC core/skill_identity.cpp core/native_skill_identity.cpp)
target_link_libraries(sdgo_skill_identity PUBLIC sdgo_api sdgo_script_registration)
sdgo_strict(sdgo_skill_identity)

# ---- mods: sdgo/mods/<id>/<id>.dll + mod.ini + extra files -------------------------
# Client mods go to stage/<config>/client/sdgo/mods/<id>, SERVER mods to the server tree.
add_custom_target(release_stable_payload DEPENDS sdgo_core d3d9_shim winmm_shim)
add_custom_target(release_beta_payload DEPENDS release_stable_payload)
function(sdgo_add_mod id)
    cmake_parse_arguments(MOD "SERVER" "DESCRIPTION" "SOURCES;FILES;LIBS" ${ARGN})
    if(MOD_SERVER)
        set(root "server/sdgo")
    else()
        set(root "client/sdgo")
    endif()
    add_library(${id} SHARED ${MOD_SOURCES})
    target_link_libraries(${id} PRIVATE sdgo_api ${MOD_LIBS})
    sdgo_strict(${id})
    sdgo_version_resource(${id} ${id}.dll "SDGO mod: ${MOD_DESCRIPTION}")
    sdgo_stage(${id} "${root}/mods/${id}")
    # Copy on every target build: this refreshes changed source data and also
    # repairs a stale/corrupted staged file whose timestamp is newer than source.
    set(mod_data_commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${SDGO_STAGE}/${root}/mods/${id}")
    foreach(file mod.ini manager.json ${MOD_FILES})
        list(APPEND mod_data_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/${id}/${file}" "${SDGO_STAGE}/${root}/mods/${id}/${file}")
    endforeach()
    add_custom_target(${id}_data ${mod_data_commands} VERBATIM)
    add_dependencies(${id} ${id}_data)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/mods/${id}/mod.ini" release_channel REGEX "^channel=")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/mods/${id}/mod.ini" release_category REGEX "^category=")
    if(NOT release_category STREQUAL "category=development")
        if(release_channel STREQUAL "channel=stable")
            add_dependencies(release_stable_payload ${id})
        elseif(release_channel STREQUAL "channel=beta")
            add_dependencies(release_beta_payload ${id})
        endif()
    endif()
endfunction()

sdgo_add_mod(mission_ai DESCRIPTION "co-op mission wingmen (client)"
             SOURCES mods/mission_ai/mission_ai.cpp FILES mission_ai.lua)
sdgo_add_mod(ime_fix DESCRIPTION "Chinese IME candidate and commit fixes" SOURCES mods/ime_fix/ime_fix.cpp FILES symbols.fragment.ini)
sdgo_add_mod(findwindow_fix DESCRIPTION "FindWindowW fix" SOURCES mods/findwindow_fix/findwindow_fix.cpp LIBS user32)
sdgo_add_mod(startup_crash_fix DESCRIPTION "random crash at game start fix" SOURCES mods/startup_crash_fix/startup_crash_fix.cpp)
sdgo_add_mod(waitroom_window_fix DESCRIPTION "native wait-room window initialization guard"
             SOURCES mods/waitroom_window_fix/waitroom_window_fix.cpp FILES symbols.fragment.ini)
if(BUILD_TESTING)
    add_executable(waitroom_window_fix_dll_test tests/waitroom_window_fix_dll_test.cpp)
    target_link_libraries(waitroom_window_fix_dll_test PRIVATE sdgo_api)
    sdgo_strict(waitroom_window_fix_dll_test)
    add_dependencies(waitroom_window_fix_dll_test waitroom_window_fix)
    add_test(NAME waitroom_window_fix_actual_dll COMMAND waitroom_window_fix_dll_test "$<TARGET_FILE:waitroom_window_fix>")
    set_tests_properties(waitroom_window_fix_actual_dll PROPERTIES TIMEOUT 30)
endif()
sdgo_add_mod(scene_lifecycle_fix DESCRIPTION "transactional native scene registration"
             SOURCES mods/scene_lifecycle_fix/scene_lifecycle_fix.cpp FILES symbols.fragment.ini)
if(BUILD_TESTING)
    add_executable(scene_lifecycle_fix_dll_test tests/scene_lifecycle_fix_dll_test.cpp)
    target_link_libraries(scene_lifecycle_fix_dll_test PRIVATE sdgo_api)
    sdgo_strict(scene_lifecycle_fix_dll_test)
    add_dependencies(scene_lifecycle_fix_dll_test scene_lifecycle_fix)
    add_test(NAME scene_lifecycle_fix_actual_dll
             COMMAND scene_lifecycle_fix_dll_test "$<TARGET_FILE:scene_lifecycle_fix>")
    set_tests_properties(scene_lifecycle_fix_actual_dll PROPERTIES TIMEOUT 30)
endif()
sdgo_add_mod(native_data_guard DESCRIPTION "verified native battle data guards"
             SOURCES mods/native_data_guard/native_data_guard.cpp FILES symbols.fragment.ini)
sdgo_add_mod(login_recovery SERVER DESCRIPTION "native authenticated stale login recovery"
             SOURCES mods/login_recovery/login_recovery.cpp FILES symbols.fragment.ini)
add_executable(login_recovery_policy_test tests/login_recovery_policy_test.cpp)
sdgo_strict(login_recovery_policy_test)
add_test(NAME login_recovery_policy COMMAND login_recovery_policy_test)
sdgo_add_mod(fps_limit DESCRIPTION "native config.xml FPS cap" SOURCES mods/fps_limit/fps_limit.cpp)
sdgo_add_mod(announcements DESCRIPTION "scheduled native announcements"
             SOURCES mods/announcements/announcements.cpp FILES symbols.fragment.ini rules.json runtime.ini news-settings.json news.ini LIBS ole32 oleaut32 uuid comctl32 winhttp)

add_executable(fps_limit_runtime_test tests/fps_limit_runtime_test.cpp)
target_link_libraries(fps_limit_runtime_test PRIVATE sdgo_api)
sdgo_strict(fps_limit_runtime_test)
add_dependencies(fps_limit_runtime_test fps_limit sdgo_core)
sdgo_add_mod(net_probe DESCRIPTION "packet logger" SOURCES mods/net_probe/net_probe.cpp)
sdgo_add_mod(unit_catalog DESCRIPTION "confirmed unit family content"
             SOURCES mods/unit_catalog/client.cpp mods/unit_catalog/preview_runtime.cpp
                     mods/unit_catalog/preview_shop.cpp mods/unit_catalog/preview_capsule.cpp
                     mods/unit_catalog/program_runtime.cpp
                     mods/unit_catalog/skill_atlas_runtime.cpp
                     support/native_resource_overlay/native_resource_overlay.cpp
                     support/native_resource_overlay/native_resource_overlay.def
             FILES symbols.fragment.ini LIBS sdgo_script_registration)
sdgo_add_mod(unit_catalog_server SERVER DESCRIPTION "owned imported-unit native skill bindings"
             SOURCES mods/unit_catalog_server/server.cpp FILES symbols.fragment.ini LIBS sdgo_skill_identity)

sdgo_add_mod(shop_labels DESCRIPTION "shop rental and Unicode labels" SOURCES mods/shop_labels/shop_labels.cpp FILES display.example.ini)
sdgo_add_mod(shop_localization DESCRIPTION "Chinese shop localization" SOURCES mods/shop_localization/shop_localization.cpp mods/shop_localization/shop_search.cpp FILES zh-CN.ini symbols.fragment.ini LIBS user32)
sdgo_add_mod(shop_art_localization DESCRIPTION "Chinese labels over verified shop artwork"
             SOURCES mods/shop_art_localization/shop_art_localization.cpp mods/shop_art_localization/native_ui.cpp
             FILES symbols.fragment.ini LIBS user32)
add_library(shop_catalog_engine STATIC mods/shop_catalog/catalog_runtime.cpp)
target_include_directories(shop_catalog_engine PUBLIC mods/shop_catalog)
target_link_libraries(shop_catalog_engine PUBLIC odbc32 bcrypt)
sdgo_strict(shop_catalog_engine)
sdgo_add_mod(shop_catalog SERVER DESCRIPTION "host-managed native shop catalog" SOURCES mods/shop_catalog/shop_catalog.cpp FILES catalog.example.ini LIBS shop_catalog_engine)
add_executable(shop_catalog_apply tools/shop_catalog_apply.cpp)
target_link_libraries(shop_catalog_apply PRIVATE shop_catalog_engine)
sdgo_strict(shop_catalog_apply)
sdgo_stage(shop_catalog_apply "server/sdgo/mods/shop_catalog")
add_dependencies(shop_catalog shop_catalog_apply)
sdgo_add_mod(coop_ai SERVER DESCRIPTION "co-op mission wingmen (battle server)" SOURCES mods/coop_ai/coop_ai.cpp)
sdgo_add_mod(match_ai DESCRIPTION "PvP AI players (client)" SOURCES mods/match_ai/match_ai.cpp)
sdgo_add_mod(pvp_ai SERVER DESCRIPTION "PvP AI players (battle server)" SOURCES mods/pvp_ai/pvp_ai.cpp)
sdgo_add_mod(battle_ai_server SERVER DESCRIPTION "player resource rules for PvP bots" SOURCES mods/battle_ai_server/battle_ai_server.cpp)
sdgo_add_mod(battle_ai DESCRIPTION "battle AI decisions and stuck recovery" SOURCES mods/battle_ai/battle_ai.cpp FILES battle_ai.lua universal_ai.lua config.example.ini)
sdgo_add_mod(hangar_manager DESCRIPTION "native hangar sorting and duplicate exchange"
             SOURCES mods/hangar_manager/hangar_manager.cpp mods/hangar_manager/native_ui.cpp FILES units.tsv LIBS user32 ole32)
sdgo_add_mod(storage_claim DESCRIPTION "native storage claim-all button"
             SOURCES mods/storage_claim/storage_claim.cpp mods/storage_claim/native_ui.cpp LIBS user32)
sdgo_add_mod(operator_catalog DESCRIPTION "operator names, portraits and item registration"
             SOURCES mods/operator_catalog/operator_catalog.cpp FILES display.example.ini)
sdgo_add_mod(battle_rewards SERVER DESCRIPTION "native contribution rewards and owner multipliers"
             SOURCES mods/battle_rewards/battle_rewards.cpp LIBS bcrypt FILES config.example.ini)
sdgo_add_mod(room_random_client DESCRIPTION "native random loadout client synchronization"
             SOURCES mods/room_random_client/room_random_client.cpp)
sdgo_add_mod(transform_input_fix DESCRIPTION "Apsalus III missing-animation transform input recovery"
             SOURCES mods/transform_input_fix/transform_input_fix.cpp)
sdgo_add_mod(room_random SERVER DESCRIPTION "per-round random owned human loadouts"
             SOURCES mods/room_random/room_random.cpp mods/room_random/inventory.cpp
             LIBS odbc32 bcrypt FILES config.example.ini)

sdgo_add_mod(net_route DESCRIPTION "configured network endpoint routes" SOURCES mods/net_route/net_route.cpp LIBS ws2_32 shell32 FILES config.example.ini)
sdgo_add_mod(agent_udp_pool SERVER DESCRIPTION "native Agent UDP port pool" SOURCES mods/agent_udp_pool/agent_udp_pool.cpp LIBS ws2_32 FILES config.example.ini)
sdgo_add_mod(agent_relay_log_fix SERVER DESCRIPTION "native Agent relay printf fix" SOURCES mods/agent_relay_log_fix/agent_relay_log_fix.cpp)
sdgo_add_mod(game_test_client DESCRIPTION "isolated native GameTest client" SOURCES mods/game_test_client/game_test_client.cpp mods/game_test_client/blueprint_actions.cpp LIBS user32 ws2_32)
sdgo_add_mod(game_test_server SERVER DESCRIPTION "isolated native GameTest battle server" SOURCES mods/game_test_server/game_test_server.cpp)

# Bounded developer recorder, outside the installed Mod/runtime trees.
add_executable(game_test_audio tools/game_test_audio.cpp)
target_link_libraries(game_test_audio PRIVATE sdgo_api mmdevapi ole32)
sdgo_strict(game_test_audio)
sdgo_version_resource(game_test_audio game_test_audio.exe "SDGO GameTest process audio capture")

sdgo_add_mod(daily_rewards SERVER DESCRIPTION "daily double battle rewards (Beta)"
             SOURCES mods/daily_rewards/daily_rewards.cpp FILES config.example.ini)
sdgo_add_mod(rank_progression_fix SERVER DESCRIPTION "native World battle rank progression fix"
             SOURCES mods/rank_progression_fix/rank_progression_fix.cpp LIBS ole32 oleaut32 bcrypt uuid)
sdgo_add_mod(friend_list_fix SERVER DESCRIPTION "native offline friend list and pilot names"
             SOURCES mods/friend_list_fix/friend_list_fix.cpp LIBS ole32 oleaut32 bcrypt uuid)
sdgo_add_mod(friend_list_client_fix DESCRIPTION "native friend list, messenger layout and offline conversation fix"
             SOURCES mods/friend_list_client_fix/friend_list_client_fix.cpp)
sdgo_add_mod(friend_unit_gift_client DESCRIPTION "optional native friend unit gifts and provenance"
             SOURCES mods/friend_unit_gift_client/friend_unit_gift_client.cpp
                     mods/friend_unit_gift_client/native_ui.cpp
                     mods/friend_unit_gift_client/transport.cpp
             LIBS user32 ole32)
sdgo_add_mod(friend_unit_gift_server SERVER DESCRIPTION "optional authenticated friend unit gift escrow"
             SOURCES mods/friend_unit_gift_server/friend_unit_gift_server.cpp
                     mods/friend_unit_gift_server/server_database.cpp
             LIBS ole32 oleaut32 bcrypt uuid)


# Private feature libraries; no room metadata or gameplay state enters the core.
add_library(room_metadata_common STATIC mods/room_metadata/common/room_metadata_common.cpp)
target_include_directories(room_metadata_common PUBLIC mods/room_metadata/common)
sdgo_strict(room_metadata_common)
add_library(room_titles_client_state STATIC mods/room_titles/client_state.cpp)
target_include_directories(room_titles_client_state PUBLIC mods/room_titles)
target_link_libraries(room_titles_client_state PUBLIC room_metadata_common)
sdgo_strict(room_titles_client_state)
add_library(room_titles_native_transport STATIC mods/room_titles/native_transport.cpp)
target_link_libraries(room_titles_native_transport PUBLIC sdgo_api room_metadata_common)
sdgo_strict(room_titles_native_transport)

sdgo_add_mod(room_titles_agent SERVER DESCRIPTION "room title transport (Agent)"
             SOURCES mods/room_titles_agent/room_titles_agent.cpp LIBS room_metadata_common)
sdgo_add_mod(room_titles DESCRIPTION "extended room titles (client)"
             SOURCES mods/room_titles/room_titles.cpp mods/room_titles/native_ui.cpp
             LIBS room_titles_native_transport room_titles_client_state user32)
sdgo_add_mod(room_titles_server SERVER DESCRIPTION "extended room titles (Battle)"
             SOURCES mods/room_titles_server/room_titles_server.cpp mods/room_titles_server/room_titles_server_logic.cpp
             LIBS room_metadata_common bcrypt)

sdgo_add_mod(new_account_profile SERVER DESCRIPTION "new-account starter currency and rank" SOURCES mods/new_account_profile/new_account_profile.cpp LIBS ole32 oleaut32 uuid FILES config.example.ini)
sdgo_add_mod(hangar_capacity SERVER DESCRIPTION "automatic native 1200-slot hangar"
             SOURCES mods/hangar_capacity/hangar_capacity.cpp LIBS ole32 oleaut32 uuid)

sdgo_add_mod(collection_capsule SERVER DESCRIPTION "never-acquired base-form capsule machine"
             SOURCES mods/collection_capsule/collection_capsule.cpp LIBS ole32 oleaut32 uuid FILES config.example.ini)

# Shared capsule resource provider, distributed inside the owner toolkit. The
# generated player bundle uses one capsule_catalog instance for all machines.
add_library(capsule_registry_support SHARED
    support/capsule_registry/capsule_catalog.cpp support/capsule_registry/capsule_runtime.cpp
    support/native_resource_overlay/native_resource_overlay.cpp
    support/native_resource_overlay/native_resource_overlay.def)
set_target_properties(capsule_registry_support PROPERTIES
    OUTPUT_NAME capsule_catalog
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/capsule_registry_support/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/capsule_registry_support/$<CONFIG>")
target_link_libraries(capsule_registry_support PRIVATE sdgo_api)
sdgo_strict(capsule_registry_support)
sdgo_version_resource(capsule_registry_support capsule_catalog.dll "SDGO shared capsule resource registry")
sdgo_stage(capsule_registry_support "support/capsule-registry")
add_dependencies(release_stable_payload capsule_registry_support)

# Optional catalog provider, same mod id in the two processes. Owner-generated
# catalog.bin is never staged or bundled by a framework build.
foreach(role client server)
    set(target ai_catalog_${role})
    add_library(${target} SHARED mods/ai_catalog/${role}.cpp)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ai_catalog
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/catalog_${role}/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/catalog_${role}/$<CONFIG>")
    target_link_libraries(${target} PRIVATE sdgo_api)
    sdgo_strict(${target})
    sdgo_version_resource(${target} ai_catalog.dll "SDGO mod: AI roster (${role})")
    sdgo_stage(${target} "${role}/sdgo/mods/ai_catalog")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/${role}.ini"
            "${SDGO_STAGE}/${role}/sdgo/mods/ai_catalog/mod.ini")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/manager.json"
            "${SDGO_STAGE}/${role}/sdgo/mods/ai_catalog/manager.json")
endforeach()
add_custom_target(ai_catalog_data ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/client/sdgo/mods/ai_catalog"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/server/sdgo/mods/ai_catalog"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/client.ini"
        "${SDGO_STAGE}/client/sdgo/mods/ai_catalog/mod.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/server.ini"
        "${SDGO_STAGE}/server/sdgo/mods/ai_catalog/mod.ini")

sdgo_add_mod(capsule_catalog DESCRIPTION "native capsule catalog overlay"
             SOURCES mods/capsule_catalog/capsule_catalog.cpp mods/capsule_catalog/capsule_runtime.cpp
                     support/native_resource_overlay/native_resource_overlay.cpp
                     support/native_resource_overlay/native_resource_overlay.def)
sdgo_add_mod(capsule_currency_fix DESCRIPTION "activity capsule currency display fix"
             SOURCES mods/capsule_currency_fix/capsule_currency_fix.cpp mods/capsule_currency_fix/capsule_currency_logic.cpp)
sdgo_add_mod(capsule_price_probe DESCRIPTION "read-only capsule price UI diagnostics"
             SOURCES mods/capsule_price_probe/capsule_price_probe.cpp)

# ---- data: symbol files and default configuration ---------------------------------
file(GLOB SDGO_SYMBOL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/symbols/*.ini")
add_custom_target(sdgo_data ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/client/sdgo/symbols"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDGO_SYMBOL_FILES} "${SDGO_STAGE}/client/sdgo/symbols/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/sdgo.ini" "${SDGO_STAGE}/client/sdgo/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/server/sdgo/symbols"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDGO_SYMBOL_FILES} "${SDGO_STAGE}/server/sdgo/symbols/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/sdgo.ini" "${SDGO_STAGE}/server/sdgo/"
    COMMENT "Staging symbol files and sdgo.ini"
)
# Direct builds of this lifecycle fix also need its admitted host signature.
add_dependencies(waitroom_window_fix sdgo_data)
add_dependencies(unit_catalog sdgo_data)
# A direct GameTest build needs its symbol map and the matching PvP observer
# implementation; retaining an older installed DLL can hide new status exports.
add_dependencies(game_test_client sdgo_data match_ai)

# The same manifests drive package selection and runtime category/feature gates.
add_custom_target(sdgo_catalog ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mod_catalog.py"
        --root "${CMAKE_CURRENT_SOURCE_DIR}"
        --output "${SDGO_STAGE}/client/sdgo/catalog.json"
        --output "${SDGO_STAGE}/server/sdgo/catalog.json"
    COMMENT "Validating and staging the categorized mod catalog"
)

# One self-contained user entry. Individual mods only contribute contracts/panels.
option(SDGO_BUILD_MANAGER "Build the unified Mod Manager EXE" ON)
if(SDGO_BUILD_MANAGER)
    find_program(SDGO_MANAGER_POWERSHELL NAMES powershell pwsh REQUIRED)
    file(GLOB_RECURSE SDGO_MANAGER_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.html" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.json" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.ps1" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.sql"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/shop/*.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/units/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/mods/unit_catalog/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/install_unit_catalog.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/collection_capsule/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/collection_capsule/sql/*.sql"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/*collection_capsule*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/support/capsule_registry/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/mods/collection_capsule/assets/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/mods/*/manager.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/rewards/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/owner/rewards/*.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/rewards/*.sql" "${CMAKE_CURRENT_SOURCE_DIR}/backend/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/backend/*.ps1")
    set(SDGO_MANAGER_EXE "${CMAKE_BINARY_DIR}/manager/SDGO Mod Manager.exe")
    add_custom_command(OUTPUT "${SDGO_MANAGER_EXE}"
        COMMAND "${SDGO_MANAGER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_mod_manager.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}" -Python "${Python3_EXECUTABLE}" -UnitBinary "$<TARGET_FILE:unit_catalog>"
            -CapsuleBinary "$<TARGET_FILE:capsule_catalog>"
            -RegistryBinary "$<TARGET_FILE:capsule_registry_support>"
        DEPENDS ${SDGO_MANAGER_INPUTS} unit_catalog "$<TARGET_FILE:unit_catalog>"
            capsule_catalog "$<TARGET_FILE:capsule_catalog>" capsule_registry_support "$<TARGET_FILE:capsule_registry_support>" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mod_manager.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_mod_manager.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/manager/SDGOModManager.spec"
        VERBATIM)
    add_custom_target(mod_manager ALL DEPENDS "${SDGO_MANAGER_EXE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/manager"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDGO_MANAGER_EXE}" "${SDGO_STAGE}/manager/")
endif()

# ---- tests: runtime check of the loader's forwarding (tests/loader_test.ps1) -------
add_executable(loader_test tests/loader_test.c)
sdgo_strict(loader_test)
add_executable(launcher_handshake_test tests/launcher_handshake_test.cpp)
sdgo_strict(launcher_handshake_test)

add_executable(capsule_client_runtime_harness tests/capsule_client_runtime_harness.cpp
               mods/capsule_catalog/capsule_runtime.cpp)
target_include_directories(capsule_client_runtime_harness PRIVATE mods/capsule_catalog)
target_link_libraries(capsule_client_runtime_harness PRIVATE sdgo_api)
sdgo_strict(capsule_client_runtime_harness)
add_executable(capsule_client_native_stream_test tests/capsule_client_native_stream_test.cpp
               mods/capsule_catalog/capsule_runtime.cpp)
target_include_directories(capsule_client_native_stream_test PRIVATE mods/capsule_catalog)
target_link_libraries(capsule_client_native_stream_test PRIVATE sdgo_api)
sdgo_strict(capsule_client_native_stream_test)
add_executable(capsule_currency_fix_test mods/capsule_currency_fix/capsule_currency_fix_test.cpp
               mods/capsule_currency_fix/capsule_currency_logic.cpp)
target_include_directories(capsule_currency_fix_test PRIVATE mods/capsule_currency_fix)
sdgo_strict(capsule_currency_fix_test)
add_executable(capsule_currency_dll_test tests/capsule_currency_dll_test.cpp)
target_link_libraries(capsule_currency_dll_test PRIVATE sdgo_api)
sdgo_strict(capsule_currency_dll_test)
add_dependencies(capsule_currency_dll_test capsule_currency_fix)
add_executable(capsule_price_probe_test mods/capsule_price_probe/capsule_price_probe_test.cpp)
sdgo_strict(capsule_price_probe_test)

# ---- tests: findwindow_fix against a simulated SDPlus hook (tests/findwindow_fix_test.ps1)
# None of these are staged into the client or server package.
add_executable(findwindow_fix_test tests/findwindow_fix_test.cpp)
target_link_libraries(findwindow_fix_test PRIVATE sdgo_api minhook user32)
sdgo_strict(findwindow_fix_test)
add_library(export_hook_test_mod SHARED tests/export_hook_test_mod.cpp)
target_link_libraries(export_hook_test_mod PRIVATE sdgo_api user32)
sdgo_strict(export_hook_test_mod)
foreach(fixture SDPlus generic_input_method)
    add_library(findwindow_fixture_${fixture} SHARED tests/findwindow_caller_fixture.cpp)
    set_target_properties(findwindow_fixture_${fixture} PROPERTIES OUTPUT_NAME ${fixture})
    target_link_libraries(findwindow_fixture_${fixture} PRIVATE user32)
    sdgo_strict(findwindow_fixture_${fixture})
endforeach()

# Real-core transport tests, never staged into release packages.
add_executable(net_route_test tests/net_route_test.cpp)
target_link_libraries(net_route_test PRIVATE sdgo_api ws2_32)
sdgo_strict(net_route_test)
add_executable(net_route_observer_test tests/net_route_observer_test.cpp)
target_link_libraries(net_route_observer_test PRIVATE sdgo_api ws2_32)
sdgo_strict(net_route_observer_test)
add_executable(net_login_test tests/net_login_test.cpp)
target_link_libraries(net_login_test PRIVATE sdgo_api ws2_32 shell32)
sdgo_strict(net_login_test)
add_library(net_route_conflict_mod SHARED tests/net_route_conflict_mod.cpp)
target_link_libraries(net_route_conflict_mod PRIVATE sdgo_api ws2_32)
sdgo_strict(net_route_conflict_mod)
add_executable(agent_udp_pool_test tests/agent_udp_pool_test.cpp)
target_link_libraries(agent_udp_pool_test PRIVATE sdgo_api ws2_32)
sdgo_strict(agent_udp_pool_test)

add_executable(daily_rewards_policy_test tests/daily_rewards_policy_test.cpp)
target_link_libraries(daily_rewards_policy_test PRIVATE sdgo_api)
sdgo_strict(daily_rewards_policy_test)
add_executable(daily_rewards_runtime_test tests/daily_rewards_runtime_test.cpp)
target_link_libraries(daily_rewards_runtime_test PRIVATE sdgo_api)
sdgo_strict(daily_rewards_runtime_test)

# Offline AI contracts. The fixtures call production code with synthetic rooms,
# packets, HUDs and native callbacks; no game executable is loaded.
include(CTest)
if(BUILD_TESTING)
    add_executable(native_resource_overlay_test tests/native_resource_overlay_test.cpp
        support/native_resource_overlay/native_resource_overlay.cpp)
    target_link_libraries(native_resource_overlay_test PRIVATE sdgo_api)
    sdgo_strict(native_resource_overlay_test)
    add_test(NAME native_resource_overlay_offline COMMAND native_resource_overlay_test)
    add_executable(unit_catalog_test tests/unit_catalog_test.cpp)
    target_link_libraries(unit_catalog_test PRIVATE sdgo_api)
    sdgo_strict(unit_catalog_test)
    add_library(unit_catalog_old_capsule_fixture SHARED tests/old_capsule_fixture.cpp)
    set_target_properties(unit_catalog_old_capsule_fixture PROPERTIES
        OUTPUT_NAME capsule_catalog
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/old_capsule_fixture/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/old_capsule_fixture/$<CONFIG>")
    sdgo_strict(unit_catalog_old_capsule_fixture)
    add_executable(unit_preview_test tests/unit_preview_test.cpp mods/unit_catalog/preview_runtime.cpp
        mods/unit_catalog/preview_shop.cpp mods/unit_catalog/preview_capsule.cpp)
    target_link_libraries(unit_preview_test PRIVATE sdgo_api)
    sdgo_strict(unit_preview_test)
    add_test(NAME unit_preview_offline COMMAND unit_preview_test)
    add_test(NAME unit_catalog_offline COMMAND unit_catalog_test)
    foreach(scenario success conflict missing_reference partial_failure index_reference_missing index_conflict
                     skill_reference_missing funnel_reference_missing linked_skill_reference_missing dependency_records_success
                     blueprint_output_missing blueprint_material_missing
                     blueprint_item_reference_missing index_blueprint_reference_missing index_blueprint_output_mismatch)
        add_test(NAME unit_catalog_${scenario}_offline COMMAND unit_catalog_test $<TARGET_FILE:unit_catalog> ${scenario})
        set_tests_properties(unit_catalog_${scenario}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME unit_catalog_old_capsule_offline COMMAND unit_catalog_test
        $<TARGET_FILE:unit_catalog> old_capsule $<TARGET_FILE:unit_catalog_old_capsule_fixture>)
    set_tests_properties(unit_catalog_old_capsule_offline PROPERTIES TIMEOUT 30)
    add_test(NAME unit_import_owner_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_import_*.py" -v)
    set_tests_properties(unit_import_owner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 90)
    add_test(NAME unit_resource_depot_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_resource_depot.py" -v)
    set_tests_properties(unit_resource_depot_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_executable(unit_skill_presentation_test tests/unit_skill_presentation_test.cpp)
    target_link_libraries(unit_skill_presentation_test PRIVATE sdgo_api)
    sdgo_strict(unit_skill_presentation_test)
    add_test(NAME unit_skill_presentation_offline COMMAND unit_skill_presentation_test)
    set_tests_properties(unit_skill_presentation_offline PROPERTIES TIMEOUT 30)
    add_test(NAME unit_presentation_owner_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_presentations.py" -v)
    set_tests_properties(unit_presentation_owner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)

endif()
if(BUILD_TESTING)
    add_executable(ime_fix_dll_test tests/ime_fix_dll_test.cpp)
    target_link_libraries(ime_fix_dll_test PRIVATE sdgo_api)
    sdgo_strict(ime_fix_dll_test)
    add_dependencies(ime_fix_dll_test ime_fix)
    add_executable(ime_result_native_replay tests/ime_result_native_replay.cpp)
    target_link_libraries(ime_result_native_replay PRIVATE sdgo_api)
    sdgo_strict(ime_result_native_replay)
    add_dependencies(ime_result_native_replay ime_fix)
    add_test(NAME ime_result_native_replay COMMAND ime_result_native_replay "$<TARGET_FILE:ime_fix>")
    set_tests_properties(ime_result_native_replay PROPERTIES TIMEOUT 30)
    foreach(case active missing_symbol invalid_api)
        add_test(NAME ime_fix_${case} COMMAND ime_fix_dll_test "$<TARGET_FILE:ime_fix>" "${case}")
        set_tests_properties(ime_fix_${case} PROPERTIES TIMEOUT 30)
    endforeach()
endif()
if(BUILD_TESTING)
    add_executable(core_ini_test tests/core_ini_test.cpp)
    add_executable(core_symbols_test tests/core_symbols_test.cpp core/symbols.cpp core/log.cpp)
    target_link_libraries(core_symbols_test PRIVATE sdgo_api)
    target_include_directories(core_symbols_test PRIVATE core)
    sdgo_strict(core_symbols_test)
    add_test(NAME core_symbols_offline COMMAND core_symbols_test)
    set_tests_properties(core_symbols_offline PROPERTIES TIMEOUT 60)
    add_executable(symbol_fragment_test tests/symbol_fragment_test.cpp core/symbols.cpp core/log.cpp)
    target_link_libraries(symbol_fragment_test PRIVATE sdgo_api)
    add_executable(game_test_sealed_room_title_test tests/game_test_sealed_room_title_test.cpp)
    sdgo_strict(game_test_sealed_room_title_test)
    add_test(NAME game_test_sealed_room_title COMMAND game_test_sealed_room_title_test)
    set_tests_properties(game_test_sealed_room_title PROPERTIES TIMEOUT 30)
    sdgo_strict(core_ini_test)
    sdgo_strict(symbol_fragment_test)
    add_test(NAME core_ini_offline COMMAND core_ini_test)
    add_test(NAME symbol_fragment_offline COMMAND symbol_fragment_test)
    add_executable(transform_input_fix_runtime_test tests/transform_input_fix_runtime_test.cpp)
    target_link_libraries(transform_input_fix_runtime_test PRIVATE sdgo_api)
    sdgo_strict(transform_input_fix_runtime_test)
    add_dependencies(transform_input_fix_runtime_test transform_input_fix)
    add_test(NAME transform_input_fix_actual_dll COMMAND transform_input_fix_runtime_test "$<TARGET_FILE:transform_input_fix>")
    set_tests_properties(transform_input_fix_actual_dll PROPERTIES TIMEOUT 30)
    add_executable(automatic_fixes_runtime_test tests/automatic_fixes_runtime_test.cpp)
    target_link_libraries(automatic_fixes_runtime_test PRIVATE sdgo_api)
    sdgo_strict(automatic_fixes_runtime_test)
    add_dependencies(automatic_fixes_runtime_test startup_crash_fix agent_relay_log_fix)
    add_test(NAME startup_crash_fix_actual_dll COMMAND automatic_fixes_runtime_test gate "$<TARGET_FILE:startup_crash_fix>")
    add_test(NAME agent_relay_log_fix_actual_dll COMMAND automatic_fixes_runtime_test relay "$<TARGET_FILE:agent_relay_log_fix>")
    set_tests_properties(startup_crash_fix_actual_dll agent_relay_log_fix_actual_dll PROPERTIES TIMEOUT 30)
    add_executable(game_test_battle_ai_activity_test tests/game_test_battle_ai_activity_test.cpp)
    sdgo_strict(game_test_battle_ai_activity_test)
    add_test(NAME game_test_battle_ai_activity_offline COMMAND game_test_battle_ai_activity_test)
    add_executable(game_test_quiet_wait_room_test tests/game_test_quiet_wait_room_test.cpp)
    target_link_libraries(game_test_quiet_wait_room_test PRIVATE sdgo_api)
    sdgo_strict(game_test_quiet_wait_room_test)
    add_test(NAME game_test_quiet_wait_room_offline COMMAND game_test_quiet_wait_room_test)
    add_executable(native_data_guard_runtime_test tests/native_data_guard_runtime_test.cpp)
    target_link_libraries(native_data_guard_runtime_test PRIVATE sdgo_api)
    sdgo_strict(native_data_guard_runtime_test)
    add_dependencies(native_data_guard_runtime_test native_data_guard)
    add_test(NAME native_data_guard_actual_dll COMMAND native_data_guard_runtime_test "$<TARGET_FILE:native_data_guard>")
    set_tests_properties(native_data_guard_actual_dll PROPERTIES TIMEOUT 30)
    add_executable(game_test_quiet_sockets_test tests/game_test_quiet_sockets_test.cpp)
    target_link_libraries(game_test_quiet_sockets_test PRIVATE sdgo_api ws2_32)
    sdgo_strict(game_test_quiet_sockets_test)
    add_test(NAME game_test_quiet_sockets_offline COMMAND game_test_quiet_sockets_test)
    add_executable(game_test_quiet_audio_test tests/game_test_quiet_audio_test.cpp)
    target_link_libraries(game_test_quiet_audio_test PRIVATE sdgo_api)
    sdgo_strict(game_test_quiet_audio_test)
    add_test(NAME game_test_quiet_audio_offline COMMAND game_test_quiet_audio_test)
    add_executable(game_test_quiet_graphics_test tests/game_test_quiet_graphics_test.cpp)
    target_link_libraries(game_test_quiet_graphics_test PRIVATE sdgo_api user32)
    sdgo_strict(game_test_quiet_graphics_test)
    add_test(NAME game_test_quiet_graphics_offline COMMAND game_test_quiet_graphics_test)
    add_test(NAME game_test_windows_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_game_test_windows.py)
    # The real child uses below-normal priority on a shared development host.
    set_tests_properties(game_test_windows_offline PROPERTIES TIMEOUT 90)
    add_test(NAME launcher_handshake_offline COMMAND launcher_handshake_test)
    add_test(NAME capsule_currency_actual_dll COMMAND capsule_currency_dll_test "$<TARGET_FILE:capsule_currency_fix>")
    set_tests_properties(capsule_currency_actual_dll PROPERTIES TIMEOUT 30)
endif()
if(BUILD_TESTING)
    add_executable(shop_localization_test tests/shop_localization_test.cpp)
    target_link_libraries(shop_localization_test PRIVATE sdgo_api)
    sdgo_strict(shop_localization_test)
    add_dependencies(shop_localization_test shop_localization)
    foreach(case RANGE 0 5)
        add_test(NAME shop_localization_${case} COMMAND shop_localization_test "$<TARGET_FILE:shop_localization>"
            "${CMAKE_BINARY_DIR}/shop-localization-case-${case}" "${case}")
        set_tests_properties(shop_localization_${case} PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME shop_localization_dictionary COMMAND shop_localization_test "$<TARGET_FILE:shop_localization>"
        "${CMAKE_BINARY_DIR}/shop-localization-case-bank" "6" "${CMAKE_CURRENT_SOURCE_DIR}/mods/shop_localization/zh-CN.ini")
    add_library(localization_service_fixture SHARED tests/localization_service_fixture.cpp)
    target_link_libraries(localization_service_fixture PRIVATE sdgo_api)
    set_target_properties(localization_service_fixture PROPERTIES OUTPUT_NAME shop_localization
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/localization-fixture/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/localization-fixture/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/localization-fixture/$<CONFIG>")
    sdgo_strict(localization_service_fixture)
    add_executable(localization_consumer_test tests/localization_consumer_test.cpp)
    target_link_libraries(localization_consumer_test PRIVATE sdgo_api)
    sdgo_strict(localization_consumer_test)
    add_dependencies(localization_consumer_test localization_service_fixture)
    add_test(NAME localization_optional_consumer COMMAND localization_consumer_test "$<TARGET_FILE:localization_service_fixture>")
    add_executable(shop_search_protocol_test tests/shop_search_protocol_test.cpp)
    target_link_libraries(shop_search_protocol_test PRIVATE sdgo_api)
    sdgo_strict(shop_search_protocol_test)
    add_test(NAME shop_search_protocol COMMAND shop_search_protocol_test)
    add_executable(shop_search_dll_test tests/shop_search_dll_test.cpp)
    target_link_libraries(shop_search_dll_test PRIVATE sdgo_api user32)
    sdgo_strict(shop_search_dll_test)
    add_test(NAME shop_search_native_bridge COMMAND shop_search_dll_test)
    add_executable(shop_search_index_parser_test tests/shop_search_index_parser_test.cpp)
    target_link_libraries(shop_search_index_parser_test PRIVATE shop_catalog_engine)
    sdgo_strict(shop_search_index_parser_test)
    add_test(NAME shop_search_index_parser COMMAND shop_search_index_parser_test)
    add_executable(shop_search_sql_builder tests/shop_search_sql_builder.cpp)
    target_include_directories(shop_search_sql_builder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(shop_search_sql_builder)
    add_test(NAME shop_search_sql_builder COMMAND shop_search_sql_builder)
    add_executable(shop_custom_display_test tests/shop_custom_display_test.cpp)
    target_link_libraries(shop_custom_display_test PRIVATE sdgo_api)
    sdgo_strict(shop_custom_display_test)
    foreach(case RANGE 0 2)
        add_test(NAME shop_custom_chinese_display_${case} COMMAND shop_custom_display_test
            "${CMAKE_BINARY_DIR}/shop-custom-display-${case}" "${case}")
    endforeach()

    foreach(test capsule_currency_fix capsule_price_probe)
        add_test(NAME ${test}_offline COMMAND ${test}_test)
        set_tests_properties(${test}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME capsule_configuration COMMAND "${CMAKE_COMMAND}" -E env
        "CAPSULE_RUNTIME_HARNESS=$<TARGET_FILE:capsule_client_runtime_harness>"
        "${Python3_EXECUTABLE}" -B -X utf8 -m unittest discover -s tests -p "test_capsule*.py" -q)
    set_tests_properties(capsule_configuration PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 180)
endif()

if(BUILD_TESTING)
    add_executable(game_test_framework_host tests/game_test_framework_host.cpp)
    target_link_libraries(game_test_framework_host PRIVATE sdgo_api)
    sdgo_strict(game_test_framework_host)
    add_test(NAME game_test_framework_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/game_test_framework_test.py"
        --host "$<TARGET_FILE:game_test_framework_host>")
    set_tests_properties(game_test_framework_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(game_test_onboarding_gate_test tests/game_test_onboarding_gate_test.cpp)
    add_executable(game_test_onboarding_actions_test tests/game_test_onboarding_actions_test.cpp)
    target_link_libraries(game_test_onboarding_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_onboarding_actions_test)
    add_test(NAME game_test_onboarding_actions_offline COMMAND game_test_onboarding_actions_test)
    set_tests_properties(game_test_onboarding_actions_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_native_credentials_test tests/game_test_native_credentials_test.cpp)
    foreach(friend_runtime_test IN ITEMS game_test_native_actions_transport game_test_friend_invite_sender game_test_friend_invite_receiver game_test_friend_room_actions)
        add_executable(${friend_runtime_test}_test tests/${friend_runtime_test}_test.cpp)
        target_link_libraries(${friend_runtime_test}_test PRIVATE sdgo_api)
        sdgo_strict(${friend_runtime_test}_test)
        add_test(NAME ${friend_runtime_test}_offline COMMAND ${friend_runtime_test}_test)
        set_tests_properties(${friend_runtime_test}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    target_link_libraries(game_test_native_credentials_test PRIVATE sdgo_api)
    sdgo_strict(game_test_native_credentials_test)
    add_test(NAME game_test_native_credentials_offline COMMAND game_test_native_credentials_test)
    set_tests_properties(game_test_native_credentials_offline PROPERTIES TIMEOUT 30)
    sdgo_strict(game_test_onboarding_gate_test)
    add_test(NAME game_test_onboarding_gate_offline COMMAND game_test_onboarding_gate_test)
    set_tests_properties(game_test_onboarding_gate_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_room_actions_test tests/game_test_room_actions_test.cpp)
    # Private monitor fixtures only; no stage/catalog rule or player payload.
    add_library(game_test_loopback_guard SHARED tests/game_test_loopback_guard.cpp)
    target_link_libraries(game_test_loopback_guard PRIVATE sdgo_api ws2_32)
    sdgo_strict(game_test_loopback_guard)
    add_library(game_test_dbghelp_loader SHARED tests/game_test_dbghelp_loader.cpp tests/game_test_dbghelp_loader.def)
    target_link_libraries(game_test_dbghelp_loader PRIVATE early_boot)
    sdgo_strict(game_test_dbghelp_loader)
    add_executable(game_test_dbghelp_loader_test tests/game_test_dbghelp_loader_test.cpp)
    sdgo_strict(game_test_dbghelp_loader_test)
    add_dependencies(game_test_dbghelp_loader_test game_test_dbghelp_loader)
    add_test(NAME game_test_dbghelp_loader_offline COMMAND game_test_dbghelp_loader_test "$<TARGET_FILE:game_test_dbghelp_loader>")
    set_tests_properties(game_test_dbghelp_loader_offline PROPERTIES TIMEOUT 30)
    target_link_libraries(game_test_room_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_room_actions_test)
    add_test(NAME game_test_room_actions_offline COMMAND game_test_room_actions_test)
    set_tests_properties(game_test_room_actions_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_channel_actions_test tests/game_test_channel_actions_test.cpp)
    add_executable(game_test_friend_extra_actions_test tests/game_test_friend_extra_actions_test.cpp)
    add_executable(game_test_friend_actions_test tests/game_test_friend_actions_test.cpp)
    add_executable(game_test_friend_message_history_test tests/game_test_friend_message_history_test.cpp)
    target_link_libraries(game_test_friend_message_history_test PRIVATE sdgo_api)
    sdgo_strict(game_test_friend_message_history_test)
    add_test(NAME game_test_friend_message_history_offline COMMAND game_test_friend_message_history_test)
    set_tests_properties(game_test_friend_message_history_offline PROPERTIES TIMEOUT 30)
    target_link_libraries(game_test_friend_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_friend_actions_test)
    add_test(NAME game_test_friend_actions_offline COMMAND game_test_friend_actions_test)
    set_tests_properties(game_test_friend_actions_offline PROPERTIES TIMEOUT 30)
    target_link_libraries(game_test_friend_extra_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_friend_extra_actions_test)
    add_test(NAME game_test_friend_extra_actions_offline COMMAND game_test_friend_extra_actions_test)
    set_tests_properties(game_test_friend_extra_actions_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_hangar_navigation_test tests/game_test_hangar_navigation_test.cpp)
    add_executable(game_test_page_test_actions_test tests/game_test_page_test_actions_test.cpp)
    target_link_libraries(game_test_page_test_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_page_test_actions_test)
    add_test(NAME game_test_page_test_actions_offline COMMAND game_test_page_test_actions_test)
    add_executable(game_test_storage_test_actions_test tests/game_test_storage_test_actions_test.cpp)
    target_link_libraries(game_test_storage_test_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_storage_test_actions_test)
    add_test(NAME game_test_storage_test_actions_offline COMMAND game_test_storage_test_actions_test)
    add_executable(game_test_battle_combat_actions_test tests/game_test_battle_combat_actions_test.cpp)
    target_link_libraries(game_test_battle_combat_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_battle_combat_actions_test)
    add_test(NAME game_test_battle_combat_actions_offline COMMAND game_test_battle_combat_actions_test)
    add_executable(game_test_capsule_draw_actions_test tests/game_test_capsule_draw_actions_test.cpp)
    target_link_libraries(game_test_capsule_draw_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_capsule_draw_actions_test)
    add_test(NAME game_test_capsule_draw_actions_offline COMMAND game_test_capsule_draw_actions_test --suite)
    add_executable(game_test_unit_select_actions_test tests/game_test_unit_select_actions_test.cpp)
    target_link_libraries(game_test_unit_select_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_unit_select_actions_test)
    add_test(NAME game_test_unit_select_actions_offline COMMAND game_test_unit_select_actions_test)
    add_executable(game_test_active_selection_observer_test tests/game_test_active_selection_observer_test.cpp)
    target_link_libraries(game_test_active_selection_observer_test PRIVATE sdgo_api)
    sdgo_strict(game_test_active_selection_observer_test)
    add_test(NAME game_test_active_selection_observer_offline COMMAND game_test_active_selection_observer_test)
    add_executable(game_test_room_join_actions_test tests/game_test_room_join_actions_test.cpp)
    target_link_libraries(game_test_room_join_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_room_join_actions_test)
    add_test(NAME game_test_room_join_actions_offline COMMAND game_test_room_join_actions_test)
    add_executable(game_test_guest_ready_actions_test tests/game_test_guest_ready_actions_test.cpp)
    target_link_libraries(game_test_guest_ready_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_guest_ready_actions_test)
    add_test(NAME game_test_guest_ready_actions_offline COMMAND game_test_guest_ready_actions_test)
    add_executable(game_test_pvp_session_actions_test tests/game_test_pvp_session_actions_test.cpp)
    target_link_libraries(game_test_pvp_session_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_pvp_session_actions_test)
    add_test(NAME game_test_pvp_session_actions_offline COMMAND game_test_pvp_session_actions_test)
    add_executable(game_test_mission_flow_actions_test tests/game_test_mission_flow_actions_test.cpp)
    target_link_libraries(game_test_mission_flow_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_mission_flow_actions_test)
    add_test(NAME game_test_mission_flow_actions_offline COMMAND game_test_mission_flow_actions_test)
    add_executable(game_test_shop_catalog_actions_test tests/game_test_shop_catalog_actions_test.cpp)
    target_link_libraries(game_test_shop_catalog_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_shop_catalog_actions_test)
    target_compile_options(game_test_shop_catalog_actions_test PRIVATE /utf-8)
    add_test(NAME game_test_shop_catalog_actions_offline COMMAND game_test_shop_catalog_actions_test)
    add_executable(game_test_operator_catalog_actions_test tests/game_test_operator_catalog_actions_test.cpp)
    target_link_libraries(game_test_operator_catalog_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_operator_catalog_actions_test)
    add_test(NAME game_test_operator_catalog_actions_offline COMMAND game_test_operator_catalog_actions_test)
    target_link_libraries(game_test_hangar_navigation_test PRIVATE sdgo_api)
    sdgo_strict(game_test_hangar_navigation_test)
    add_test(NAME game_test_hangar_navigation_offline COMMAND game_test_hangar_navigation_test)
    foreach(action battleship battleship_deck gift)
        add_executable(game_test_${action}_actions_test tests/game_test_${action}_actions_test.cpp)
        target_link_libraries(game_test_${action}_actions_test PRIVATE sdgo_api)
        sdgo_strict(game_test_${action}_actions_test)
        add_test(NAME game_test_${action}_actions_offline COMMAND game_test_${action}_actions_test)
        set_tests_properties(game_test_${action}_actions_offline PROPERTIES TIMEOUT 30)
    endforeach()
    target_link_libraries(game_test_channel_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_channel_actions_test)
    add_test(NAME game_test_channel_actions_offline COMMAND game_test_channel_actions_test)
    set_tests_properties(game_test_channel_actions_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_server_test tests/game_test_server_test.cpp)
    target_link_libraries(game_test_server_test PRIVATE sdgo_api)
    sdgo_strict(game_test_server_test)
    add_test(NAME game_test_server_offline COMMAND game_test_server_test)
    set_tests_properties(game_test_server_offline PROPERTIES TIMEOUT 30)
    add_test(
        NAME game_test_server_ready_json_offline
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/game_test_server_ready_json_test.py"
                "$<TARGET_FILE:game_test_server_test>")
    set_tests_properties(game_test_server_ready_json_offline
                         PROPERTIES TIMEOUT 30)
    add_executable(game_test_native_death_probe_test
                   tests/game_test_native_death_probe_test.cpp)
    target_link_libraries(game_test_native_death_probe_test PRIVATE sdgo_api)
    target_include_directories(game_test_native_death_probe_test
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(game_test_native_death_probe_test)
    add_test(NAME game_test_native_death_probe_offline
             COMMAND game_test_native_death_probe_test)
    set_tests_properties(game_test_native_death_probe_offline
                         PROPERTIES TIMEOUT 30)
    add_executable(game_test_native_player_combat_probe_test
                   tests/game_test_native_player_combat_probe_test.cpp)
    target_link_libraries(game_test_native_player_combat_probe_test PRIVATE sdgo_api)
    target_include_directories(game_test_native_player_combat_probe_test
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(game_test_native_player_combat_probe_test)
    add_test(NAME game_test_native_player_combat_probe_offline
             COMMAND game_test_native_player_combat_probe_test)
    set_tests_properties(game_test_native_player_combat_probe_offline
                         PROPERTIES TIMEOUT 30)

    if(SDGO_BUILD_MANAGER)
        add_test(NAME shop_owner_domain_service COMMAND "${CMAKE_BINARY_DIR}/manager-services/ShopUiTests.exe")
        add_test(NAME shop_chinese_required COMMAND "${CMAKE_BINARY_DIR}/manager-services/ShopChineseRequiredTests.exe")
        set_tests_properties(shop_owner_domain_service PROPERTIES TIMEOUT 60)
    endif()
    if(SDGO_BUILD_MANAGER)
        add_test(NAME mod_manager_frozen COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mod_manager_frozen_test.py" --exe "${SDGO_MANAGER_EXE}")
        set_tests_properties(mod_manager_frozen PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
    add_test(NAME mod_manager_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_mod_manager*.py" -v)
    set_tests_properties(mod_manager_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)
    add_test(NAME mod_manager_calendar_gifts_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_calendar*.py" -v)
    set_tests_properties(mod_manager_calendar_gifts_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME mod_manager_updates_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_manager_updates*.py" -v)
    set_tests_properties(mod_manager_updates_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)
    if(SDGO_BUILD_MANAGER)
        set_tests_properties(mod_manager_offline PROPERTIES
            ENVIRONMENT "SDGO_MANAGER_SERVICES=${CMAKE_BINARY_DIR}/manager-services/SdgoShopServices.dll")
    endif()
    add_test(NAME pr_ai_disclosure_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_pr_ai_disclosure.py" -v)
    set_tests_properties(pr_ai_disclosure_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_executable(hangar_model_test tests/hangar_model_test.cpp)
    target_link_libraries(hangar_model_test PRIVATE sdgo_api)
    sdgo_strict(hangar_model_test)
    target_compile_options(hangar_model_test PRIVATE /UNDEBUG)
    add_test(NAME hangar_model_offline COMMAND hangar_model_test)
    add_executable(hangar_native_test tests/hangar_native_test.cpp mods/hangar_manager/native_ui.cpp)
    target_link_libraries(hangar_native_test PRIVATE sdgo_api user32 ole32)
    sdgo_strict(hangar_native_test)
    target_compile_options(hangar_native_test PRIVATE /UNDEBUG)
    add_test(NAME hangar_native_offline COMMAND hangar_native_test)
    add_executable(hangar_page_ui_test tests/hangar_page_ui_test.cpp mods/hangar_manager/native_ui.cpp)
    target_link_libraries(hangar_page_ui_test PRIVATE sdgo_api user32)
    sdgo_strict(hangar_page_ui_test)
    target_compile_options(hangar_page_ui_test PRIVATE /UNDEBUG)
    add_test(NAME hangar_page_ui_offline COMMAND hangar_page_ui_test)
    add_executable(storage_claim_model_test tests/storage_claim_model_test.cpp)
    target_link_libraries(storage_claim_model_test PRIVATE sdgo_api)
    sdgo_strict(storage_claim_model_test)
    target_compile_options(storage_claim_model_test PRIVATE /UNDEBUG)
    add_test(NAME storage_claim_model_offline COMMAND storage_claim_model_test)
    add_executable(storage_claim_native_test tests/storage_claim_native_test.cpp mods/storage_claim/native_ui.cpp)
    target_link_libraries(storage_claim_native_test PRIVATE sdgo_api user32)
    sdgo_strict(storage_claim_native_test)
    target_compile_options(storage_claim_native_test PRIVATE /UNDEBUG)
    add_test(NAME storage_claim_native_offline COMMAND storage_claim_native_test)
    # Optional execution of two hash-pinned native UI snippets. The game image
    # is read only; no entry point, imports, rendering or networking is started.
    set(SDGO_HANGAR_NATIVE_HOST "" CACHE FILEPATH "Local supported GOnline.exe for hangar mouse replay")
    if(SDGO_HANGAR_NATIVE_HOST)
        add_executable(hangar_native_input_test tests/hangar_native_input_test.cpp mods/hangar_manager/native_ui.cpp)
        target_link_libraries(hangar_native_input_test PRIVATE sdgo_api user32 bcrypt)
        sdgo_strict(hangar_native_input_test)
        target_compile_options(hangar_native_input_test PRIVATE /UNDEBUG)
        add_test(NAME hangar_native_input_replay COMMAND hangar_native_input_test "${SDGO_HANGAR_NATIVE_HOST}")
        set_tests_properties(hangar_native_input_replay PROPERTIES TIMEOUT 60)
        add_executable(storage_claim_replay_test tests/storage_claim_replay_test.cpp mods/storage_claim/native_ui.cpp)
        target_link_libraries(storage_claim_replay_test PRIVATE sdgo_api user32 bcrypt)
        sdgo_strict(storage_claim_replay_test)
        target_compile_options(storage_claim_replay_test PRIVATE /UNDEBUG)
        add_test(NAME storage_claim_native_replay COMMAND storage_claim_replay_test "${SDGO_HANGAR_NATIVE_HOST}")
        set_tests_properties(storage_claim_native_replay PROPERTIES TIMEOUT 60)
    endif()
    add_executable(shop_labels_test tests/shop_labels_test.cpp)
    target_link_libraries(shop_labels_test PRIVATE sdgo_api)
    sdgo_strict(shop_labels_test)
    add_dependencies(shop_labels_test shop_labels)
    add_test(NAME shop_labels_dll_contract COMMAND shop_labels_test "$<TARGET_FILE:shop_labels>" "${CMAKE_BINARY_DIR}/shop_labels_test_data")
    add_executable(shop_art_localization_ui_test tests/shop_art_localization_ui_test.cpp
                   mods/shop_art_localization/native_ui.cpp)
    target_include_directories(shop_art_localization_ui_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(shop_art_localization_ui_test PRIVATE sdgo_api user32)
    sdgo_strict(shop_art_localization_ui_test)
    target_compile_options(shop_art_localization_ui_test PRIVATE /UNDEBUG)
    add_test(NAME shop_art_localization_ui_offline COMMAND shop_art_localization_ui_test)
    add_executable(shop_catalog_mod_test tests/shop_catalog_mod_test.cpp)
    target_link_libraries(shop_catalog_mod_test PRIVATE sdgo_api)
    sdgo_strict(shop_catalog_mod_test)
    add_dependencies(shop_catalog_mod_test shop_catalog)
    add_executable(shop_catalog_contract_test tests/shop_catalog_contract_test.cpp)
    sdgo_strict(shop_catalog_contract_test)
    target_compile_options(shop_catalog_contract_test PRIVATE /UNDEBUG)
    add_test(NAME shop_catalog_sql_contract COMMAND shop_catalog_contract_test)
    if(SDGO_MANAGER_POWERSHELL)
        add_test(NAME shop_manager_ownership COMMAND "${SDGO_MANAGER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/shop_manager_ownership_test.ps1" -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>")
        set_tests_properties(shop_manager_ownership PROPERTIES TIMEOUT 60)
    endif()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME shop_catalog_configuration COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s tests -p test_shop_catalog.py -v)
        set_tests_properties(shop_catalog_configuration PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
        add_test(NAME shop_package_installer COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/shop_install_test.py" --build "${CMAKE_BINARY_DIR}")
        set_tests_properties(shop_package_installer PROPERTIES TIMEOUT 360)
    endif()
    add_executable(operator_catalog_test tests/operator_catalog_test.cpp)
    target_link_libraries(operator_catalog_test PRIVATE sdgo_api)
    sdgo_strict(operator_catalog_test)
    foreach(case RANGE 0 3)
        add_test(NAME operator_catalog_${case} COMMAND operator_catalog_test "$<TARGET_FILE:operator_catalog>"
                 "${CMAKE_BINARY_DIR}/operator-case-${case}" "${case}")
    endforeach()
    add_test(NAME operator_assistant_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
             -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_operator_assistant.py")
    set_tests_properties(operator_assistant_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_test(NAME daily_rewards_policy COMMAND daily_rewards_policy_test)
    set_tests_properties(daily_rewards_policy PROPERTIES TIMEOUT 30)
    add_library(catalog_conflict_mod SHARED tests/catalog_conflict_mod.cpp)
    target_link_libraries(catalog_conflict_mod PRIVATE sdgo_api)
    sdgo_strict(catalog_conflict_mod)
    add_executable(ai_catalog_test tests/ai_catalog_test.cpp)
    target_link_libraries(ai_catalog_test PRIVATE sdgo_api)
    sdgo_strict(ai_catalog_test)
    # Keep native fixture calls as hookable calls in optimized builds as well.
    target_compile_options(ai_catalog_test PRIVATE /Ob0)
    add_executable(ai_catalog_native_test tests/ai_catalog_native_test.cpp)
    target_link_libraries(ai_catalog_native_test PRIVATE sdgo_api minhook)
    sdgo_strict(ai_catalog_native_test)
    add_executable(ai_catalog_client_map_test EXCLUDE_FROM_ALL tests/ai_catalog_client_map_test.cpp)
    target_link_libraries(ai_catalog_client_map_test PRIVATE sdgo_api kernel32 shell32)
    sdgo_strict(ai_catalog_client_map_test)
    target_compile_options(ai_catalog_client_map_test PRIVATE /Gy /GS- /GR-)
    target_link_options(ai_catalog_client_map_test PRIVATE /BASE:0x400000 /DYNAMICBASE:NO /NODEFAULTLIB /ENTRY:Entry /IGNORE:4078)
    # Explicit optional acceptance probe requiring a locally supplied Agent EXE.
    # It is never staged or run by CI against game files.
    add_library(native_agent_cash_probe_mod SHARED tests/native_agent_cash_probe_mod.cpp)
    target_link_libraries(native_agent_cash_probe_mod PRIVATE sdgo_api)
    sdgo_strict(native_agent_cash_probe_mod)
    # Explicit local image probe; no game bytes are bundled or required in CI.
    add_executable(battle_reward_native_result_probe EXCLUDE_FROM_ALL tests/battle_reward_native_result_probe.cpp)
    target_link_libraries(battle_reward_native_result_probe PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_native_result_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_native_result_probe)
    # Exercise the shipped daily reward Mod in contribution-reward coexistence tests.
    add_executable(battle_rewards_runtime_test tests/battle_rewards_runtime_test.cpp)
    target_link_libraries(battle_rewards_runtime_test PRIVATE sdgo_api)
    target_include_directories(battle_rewards_runtime_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_rewards_runtime_test)
    add_dependencies(battle_rewards_runtime_test battle_rewards sdgo_core daily_rewards)
    add_test(NAME battle_reward_runtime_integration COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/battle_rewards_runtime_test.ps1" -Config "$<CONFIG>"
        -BuildDir "${CMAKE_BINARY_DIR}" -DailyRewardsDll "$<TARGET_FILE:daily_rewards>")
    set_tests_properties(battle_reward_runtime_integration PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 360 RUN_SERIAL TRUE)
    add_executable(battle_reward_mission_test tests/battle_reward_mission_test.cpp)
    target_link_libraries(battle_reward_mission_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_mission_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_mission_test)
    add_test(NAME battle_reward_mission_offline COMMAND battle_reward_mission_test)
    add_executable(battle_eligibility_observer_test tests/battle_eligibility_observer_test.cpp)
    target_link_libraries(battle_eligibility_observer_test PRIVATE sdgo_api)
    sdgo_strict(battle_eligibility_observer_test)
    add_test(NAME battle_eligibility_observer_offline COMMAND battle_eligibility_observer_test)
    set(SDGO_BATTLE_ELIGIBILITY_NATIVE_HOST "" CACHE FILEPATH "Optional local supported BattleServer.exe for eligibility PE checks")
    if(SDGO_BATTLE_ELIGIBILITY_NATIVE_HOST)
        add_test(NAME battle_eligibility_native_pe COMMAND battle_eligibility_observer_test "${SDGO_BATTLE_ELIGIBILITY_NATIVE_HOST}")
    endif()
    set_tests_properties(battle_reward_mission_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_stats_test tests/battle_reward_stats_test.cpp)
    target_link_libraries(battle_reward_stats_test PRIVATE sdgo_api)
    sdgo_strict(battle_reward_stats_test)
    add_test(NAME battle_reward_stats_offline COMMAND battle_reward_stats_test)
    set_tests_properties(battle_reward_stats_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_native_damage_test tests/battle_reward_native_damage_test.cpp)
    target_link_libraries(battle_reward_native_damage_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_native_damage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_native_damage_test)
    add_test(NAME battle_reward_native_damage_offline COMMAND battle_reward_native_damage_test)
    set_tests_properties(battle_reward_native_damage_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_jobs_test tests/battle_reward_jobs_test.cpp)
    target_link_libraries(battle_reward_jobs_test PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_jobs_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_jobs_test)
    add_test(NAME battle_reward_jobs_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reward_job_interop.py" "$<TARGET_FILE:battle_reward_jobs_test>")
    set_tests_properties(battle_reward_jobs_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(battle_reward_binding_test tests/battle_reward_binding_test.cpp)
    target_link_libraries(battle_reward_binding_test PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_binding_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_binding_test)
    add_test(NAME battle_reward_binding_offline COMMAND battle_reward_binding_test)

    add_test(NAME battle_reward_wallet_lifecycle_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_wallet*.py" -v)
    set_tests_properties(battle_reward_wallet_lifecycle_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(battle_reward_cash_notify_test tests/battle_reward_cash_notify_test.cpp)
    target_link_libraries(battle_reward_cash_notify_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_cash_notify_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_cash_notify_test)
    add_test(NAME battle_reward_cash_notify_offline COMMAND battle_reward_cash_notify_test)
    set_tests_properties(battle_reward_cash_notify_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_boss_test tests/battle_reward_boss_test.cpp)
    target_link_libraries(battle_reward_boss_test PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_boss_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_boss_test)
    add_test(NAME battle_reward_boss_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reward_boss_native_probe.py" "$<TARGET_FILE:battle_reward_boss_test>")
    set_tests_properties(battle_reward_boss_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_executable(battle_reward_combat_test tests/battle_reward_combat_test.cpp)
    target_link_libraries(battle_reward_combat_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_combat_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_combat_test)
    add_test(NAME battle_reward_combat_offline COMMAND battle_reward_combat_test)
    set_tests_properties(battle_reward_combat_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_runtime_state_test tests/battle_reward_runtime_state_test.cpp)
    target_link_libraries(battle_reward_runtime_state_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_runtime_state_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_runtime_state_test)
    add_test(NAME battle_reward_runtime_state_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_state_probe.py" "$<TARGET_FILE:battle_reward_runtime_state_test>")
    set_tests_properties(battle_reward_runtime_state_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(battle_reward_plan_test tests/battle_reward_plan_test.cpp)
    target_link_libraries(battle_reward_plan_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_plan_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_plan_test)
    add_test(NAME battle_reward_plan_offline COMMAND battle_reward_plan_test)
    set_tests_properties(battle_reward_plan_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_bridge_test tests/battle_reward_bridge_test.cpp)
    target_link_libraries(battle_reward_bridge_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_bridge_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_bridge_test)
    add_test(NAME battle_reward_bridge_offline COMMAND battle_reward_bridge_test)
    set_tests_properties(battle_reward_bridge_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_policy_test tests/battle_reward_policy_test.cpp)
    target_link_libraries(battle_reward_policy_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_policy_test)
    add_test(NAME battle_reward_policy_offline COMMAND battle_reward_policy_test)
    set_tests_properties(battle_reward_policy_offline PROPERTIES TIMEOUT 30)
    add_test(NAME battle_reward_cpp_parity COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reward_cpp_parity.py" --no-build --exe "$<TARGET_FILE:battle_reward_policy_test>")
    set_tests_properties(battle_reward_cpp_parity PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME battle_reward_owner_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_battle_reward*.py" -v)
    set_tests_properties(battle_reward_owner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME battle_reward_delivery_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_reward*.py" -v)
    set_tests_properties(battle_reward_delivery_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)

    add_executable(rank_progression_fix_test tests/rank_progression_fix_test.cpp)
    target_link_libraries(rank_progression_fix_test PRIVATE sdgo_api ole32 oleaut32 bcrypt uuid)
    sdgo_strict(rank_progression_fix_test)
    add_test(NAME rank_progression_fix_offline COMMAND rank_progression_fix_test)
    add_executable(friend_list_fix_test tests/friend_list_fix_test.cpp)
    target_link_libraries(friend_list_fix_test PRIVATE sdgo_api ole32 oleaut32 bcrypt uuid)
    sdgo_strict(friend_list_fix_test)
    add_test(NAME friend_list_fix_offline COMMAND friend_list_fix_test)
    add_executable(friend_list_client_fix_test tests/friend_list_client_fix_test.cpp)
    target_link_libraries(friend_list_client_fix_test PRIVATE sdgo_api minhook friend_list_client_fix)
    sdgo_strict(friend_list_client_fix_test)
    add_test(NAME friend_list_client_fix_offline COMMAND friend_list_client_fix_test)
    add_executable(friend_list_sql_emit tests/friend_list_sql_emit.cpp)
    sdgo_strict(friend_list_sql_emit)
    add_executable(friend_list_db_test tests/friend_list_db_test.cpp)
    target_link_libraries(friend_list_db_test PRIVATE ole32 oleaut32 bcrypt uuid)
    sdgo_strict(friend_list_db_test)
    add_executable(friend_unit_gift_policy_test tests/friend_unit_gift_policy_test.cpp)
    target_link_libraries(friend_unit_gift_policy_test PRIVATE sdgo_api)
    sdgo_strict(friend_unit_gift_policy_test)
    target_compile_options(friend_unit_gift_policy_test PRIVATE /UNDEBUG)
    add_test(NAME friend_unit_gift_policy_offline COMMAND friend_unit_gift_policy_test)
    set_tests_properties(friend_unit_gift_policy_offline PROPERTIES TIMEOUT 30)
    add_executable(friend_unit_gift_sql_emit tests/friend_unit_gift_sql_emit.cpp)
    sdgo_strict(friend_unit_gift_sql_emit)
    foreach(gift_test IN ITEMS friend_unit_gift_protocol friend_unit_gift_client_model storage_extension_registry)
        add_executable(${gift_test}_test tests/${gift_test}_test.cpp)
        target_link_libraries(${gift_test}_test PRIVATE sdgo_api)
        sdgo_strict(${gift_test}_test)
        target_compile_options(${gift_test}_test PRIVATE /UNDEBUG)
        add_test(NAME ${gift_test}_offline COMMAND ${gift_test}_test)
        set_tests_properties(${gift_test}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(friend_unit_gift_transport_test tests/friend_unit_gift_transport_test.cpp
                   mods/friend_unit_gift_client/transport.cpp)
    target_link_libraries(friend_unit_gift_transport_test PRIVATE sdgo_api ole32)
    sdgo_strict(friend_unit_gift_transport_test)
    target_compile_options(friend_unit_gift_transport_test PRIVATE /UNDEBUG)
    add_test(NAME friend_unit_gift_transport_offline COMMAND friend_unit_gift_transport_test)
    set_tests_properties(friend_unit_gift_transport_offline PROPERTIES TIMEOUT 30)
    add_executable(friend_unit_gift_native_ui_test tests/friend_unit_gift_native_ui_test.cpp
                   mods/friend_unit_gift_client/native_ui.cpp)
    target_link_libraries(friend_unit_gift_native_ui_test PRIVATE sdgo_api user32)
    sdgo_strict(friend_unit_gift_native_ui_test)
    target_compile_options(friend_unit_gift_native_ui_test PRIVATE /UNDEBUG)
    add_test(NAME friend_unit_gift_native_ui_offline COMMAND friend_unit_gift_native_ui_test)
    set_tests_properties(friend_unit_gift_native_ui_offline PROPERTIES TIMEOUT 30)
    add_executable(friend_unit_gift_native_inventory_test
                   tests/friend_unit_gift_native_inventory_test.cpp)
    target_link_libraries(friend_unit_gift_native_inventory_test PRIVATE sdgo_api)
    sdgo_strict(friend_unit_gift_native_inventory_test)
    add_test(NAME friend_unit_gift_native_inventory_offline
             COMMAND friend_unit_gift_native_inventory_test)
    set_tests_properties(friend_unit_gift_native_inventory_offline PROPERTIES TIMEOUT 30)
    add_executable(friend_unit_gift_client_runtime_policy_test
                   tests/friend_unit_gift_client_runtime_policy_test.cpp)
    target_link_libraries(friend_unit_gift_client_runtime_policy_test PRIVATE sdgo_api)
    sdgo_strict(friend_unit_gift_client_runtime_policy_test)
    target_compile_options(friend_unit_gift_client_runtime_policy_test PRIVATE /UNDEBUG)
    add_test(NAME friend_unit_gift_client_runtime_policy_offline
             COMMAND friend_unit_gift_client_runtime_policy_test)
    set_tests_properties(friend_unit_gift_client_runtime_policy_offline PROPERTIES TIMEOUT 30)
    add_library(unit_gift_observer_fixture SHARED tests/unit_gift_observer_fixture.cpp)
    target_link_libraries(unit_gift_observer_fixture PRIVATE sdgo_api)
    sdgo_strict(unit_gift_observer_fixture)
    set_target_properties(unit_gift_observer_fixture PROPERTIES
        OUTPUT_NAME friend_unit_gift_client
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit_gift_fixture/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit_gift_fixture/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit_gift_fixture/$<CONFIG>")
    add_library(unit_gift_missing_export_fixture SHARED tests/unit_gift_observer_fixture.cpp)
    target_link_libraries(unit_gift_missing_export_fixture PRIVATE sdgo_api)
    target_compile_definitions(unit_gift_missing_export_fixture PRIVATE UNIT_GIFT_OBSERVER_FIXTURE_NO_PRODUCT_EXPORTS=1)
    sdgo_strict(unit_gift_missing_export_fixture)
    set_target_properties(unit_gift_missing_export_fixture PROPERTIES
        OUTPUT_NAME friend_unit_gift_client
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit_gift_missing_export/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit_gift_missing_export/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit_gift_missing_export/$<CONFIG>")
    add_executable(unit_gift_actions_test tests/unit_gift_actions_test.cpp)
    target_link_libraries(unit_gift_actions_test PRIVATE sdgo_api)
    sdgo_strict(unit_gift_actions_test)
    target_compile_options(unit_gift_actions_test PRIVATE /UNDEBUG)
    add_dependencies(unit_gift_actions_test unit_gift_observer_fixture unit_gift_missing_export_fixture)
    add_test(NAME unit_gift_actions_offline COMMAND unit_gift_actions_test
             $<TARGET_FILE:unit_gift_observer_fixture>
             $<TARGET_FILE:unit_gift_missing_export_fixture>)
    set_tests_properties(unit_gift_actions_offline PROPERTIES TIMEOUT 30)
    set_tests_properties(rank_progression_fix_offline PROPERTIES TIMEOUT 30)
    add_executable(rank_progression_db_test tests/rank_progression_db_test.cpp)
    target_link_libraries(rank_progression_db_test PRIVATE ole32 oleaut32 bcrypt uuid)
    sdgo_strict(rank_progression_db_test)
    add_library(ai_catalog_provider_fixture SHARED tests/ai_catalog_provider_fixture.cpp)
    set_target_properties(ai_catalog_provider_fixture PROPERTIES OUTPUT_NAME ai_catalog
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider_fixture/$<CONFIG>/ai_catalog")
    sdgo_strict(ai_catalog_provider_fixture)
    add_executable(ai_catalog_provider_test tests/ai_catalog_provider_test.cpp)
    target_link_libraries(ai_catalog_provider_test PRIVATE sdgo_api)
    sdgo_strict(ai_catalog_provider_test)
    add_dependencies(ai_catalog_provider_test ai_catalog_provider_fixture)
    add_test(NAME ai_catalog_provider_offline COMMAND ai_catalog_provider_test $<TARGET_FILE:ai_catalog_provider_fixture>)
    set_tests_properties(ai_catalog_provider_offline PROPERTIES TIMEOUT 30)

    add_executable(room_commands_test tests/room_commands_test.cpp)
    target_link_libraries(room_commands_test PRIVATE sdgo_api)
    sdgo_strict(room_commands_test)
    add_test(NAME room_commands_offline COMMAND room_commands_test)
    set_tests_properties(room_commands_offline PROPERTIES TIMEOUT 30)

    add_executable(room_random_test tests/room_random_test.cpp)
    add_executable(random_loadout_regression_test tests/random_loadout_regression_test.cpp)
    target_link_libraries(random_loadout_regression_test PRIVATE sdgo_api)
    sdgo_strict(random_loadout_regression_test)
    add_test(NAME random_loadout_regression_offline COMMAND random_loadout_regression_test)
    target_link_libraries(room_random_test PRIVATE sdgo_api bcrypt)
    sdgo_strict(room_random_test)
    add_test(NAME room_random_offline COMMAND room_random_test)
    set_tests_properties(room_random_offline PROPERTIES TIMEOUT 30)
    add_library(room_random_head_probe SHARED tests/room_random_head_probe.cpp)
    add_executable(room_random_client_policy_test tests/room_random_client_policy_test.cpp)
    sdgo_strict(room_random_client_policy_test)
    add_test(NAME room_random_client_policy COMMAND room_random_client_policy_test)
    target_link_libraries(room_random_head_probe PRIVATE sdgo_api)
    sdgo_strict(room_random_head_probe)
    add_executable(room_random_client_runtime_test tests/room_random_client_runtime_test.cpp)
    target_link_libraries(room_random_client_runtime_test PRIVATE sdgo_api)
    sdgo_strict(room_random_client_runtime_test)
    add_dependencies(room_random_client_runtime_test sdgo_core room_random_client room_random_head_probe)
    add_test(NAME room_random_client_runtime COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/room_random_client_runtime_test.ps1"
        -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>")
    set_tests_properties(room_random_client_runtime PROPERTIES TIMEOUT 120)

    # Explicitly invoked read-only host probe; not a CTest or staged executable.
    add_executable(room_random_inventory_probe tests/room_random_inventory_probe.cpp mods/room_random/inventory.cpp)
    target_link_libraries(room_random_inventory_probe PRIVATE sdgo_api odbc32)
    sdgo_strict(room_random_inventory_probe)

    add_library(catalog_gate_probe SHARED tests/catalog_gate_probe.cpp)
    target_link_libraries(catalog_gate_probe PRIVATE sdgo_api)
    sdgo_strict(catalog_gate_probe)
    add_test(NAME mod_catalog_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_mod_catalog.py" -v)
    set_tests_properties(mod_catalog_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME game_test_runner_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_game_test.py" -v)
    set_tests_properties(game_test_runner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME game_test_fps_runner_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_game_test_fps*.py" -v)
    set_tests_properties(game_test_fps_runner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    foreach(plan routes mission_plan shop_plan unit_selection reuse environment ai_teams multiplayer)
        add_test(NAME game_test_${plan}_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_game_test_${plan}.py" -v)
        set_tests_properties(game_test_${plan}_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    endforeach()
    add_test(NAME game_test_server_report_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_game_test_server_report.py" -v)
    set_tests_properties(game_test_server_report_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_library(battle_ai_server_bridge_fixture SHARED tests/battle_ai_server_bridge_fixture.cpp)
    sdgo_strict(battle_ai_server_bridge_fixture)
    add_executable(pvp_team_plan_test tests/pvp_team_plan_test.cpp)
    target_link_libraries(pvp_team_plan_test PRIVATE sdgo_api)
    sdgo_strict(pvp_team_plan_test)
    add_test(NAME pvp_team_plan_offline COMMAND pvp_team_plan_test)
    foreach(mod pvp_ai match_ai coop_ai mission_ai)
        add_executable(${mod}_test "tests/${mod}_test.cpp")
        target_link_libraries(${mod}_test PRIVATE sdgo_api)
        sdgo_strict(${mod}_test)
        if(mod STREQUAL "pvp_ai")
            add_test(NAME ${mod}_offline
                COMMAND ${mod}_test "$<TARGET_FILE:battle_ai_server_bridge_fixture>")
        else()
            add_test(NAME ${mod}_offline COMMAND ${mod}_test)
        endif()
        set_tests_properties(${mod}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    add_dependencies(pvp_ai_test battle_ai_server_bridge_fixture)
    target_link_libraries(match_ai_test PRIVATE minhook)
    add_executable(mission_ai_lifecycle_dll_test tests/mission_ai_lifecycle_dll_test.cpp)
    target_link_libraries(mission_ai_lifecycle_dll_test PRIVATE sdgo_api)
    sdgo_strict(mission_ai_lifecycle_dll_test)
    add_dependencies(mission_ai_lifecycle_dll_test match_ai)
    add_test(NAME mission_ai_lifecycle_dll COMMAND mission_ai_lifecycle_dll_test "$<TARGET_FILE:match_ai>")
    add_executable(mission_ai_observer_test tests/mission_ai_observer_test.cpp)
    add_library(mission_respawn_server_probe SHARED tests/mission_respawn_server_probe.cpp)
    target_link_libraries(mission_respawn_server_probe PRIVATE sdgo_api)
    sdgo_strict(mission_respawn_server_probe)
    target_link_libraries(mission_ai_observer_test PRIVATE sdgo_api)
    sdgo_strict(mission_ai_observer_test)
    add_test(NAME mission_ai_observer_offline COMMAND mission_ai_observer_test)
    set_tests_properties(mission_ai_observer_offline PROPERTIES TIMEOUT 30)

    find_program(SDGO_POWERSHELL NAMES powershell pwsh)
    if(SDGO_POWERSHELL)
        add_test(NAME net_route_observer_runtime COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/net_route_observer_test.ps1"
            -Config $<CONFIG>)
        set_tests_properties(net_route_observer_runtime PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME fps_limit_runtime COMMAND "${Python3_EXECUTABLE}" -X utf8
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fps_limit_test.py"
            --config "$<CONFIG>" --build-dir "${CMAKE_BINARY_DIR}")
        set_tests_properties(fps_limit_runtime PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
        add_test(NAME daily_rewards_runtime COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/daily_rewards_runtime_test.ps1"
            -Config "$<CONFIG>")
        set_tests_properties(daily_rewards_runtime PROPERTIES TIMEOUT 120)
        add_test(NAME ai_catalog_integration COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_catalog_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        # This runs the complete client/server fixture matrix in child shells.
        set_tests_properties(ai_catalog_integration PROPERTIES TIMEOUT 180)
        add_test(NAME battle_ai_server_native_integration COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/battle_ai_server_native_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(battle_ai_server_native_integration PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        add_test(NAME catalog_gates_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_gate_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(catalog_gates_offline PROPERTIES TIMEOUT 60)
        add_test(NAME package_catalog_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_catalog_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        # Eight editions plus packaged PowerShell installer/update readbacks.
        set_tests_properties(package_catalog_offline PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
        foreach(check loader findwindow_fix)
            add_test(NAME ${check}_offline COMMAND "${SDGO_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/${check}_test.ps1"
                -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
            set_tests_properties(${check}_offline PROPERTIES TIMEOUT 60)
        endforeach()
        add_test(NAME installer_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_test.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}")
        # Exercises repeated client/Battle/World updates in child PowerShell processes.
        set_tests_properties(installer_offline PROPERTIES TIMEOUT 180)
        add_test(NAME mod_staging_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/staging_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(mod_staging_offline PROPERTIES TIMEOUT 90)
    endif()

    # Optional official Lua 5.0.3 source; never linked into a shipped mod.
    set(SDGO_LUA50_SOURCE "" CACHE PATH "Extracted official Lua 5.0.3 source for offline tests")
    if(SDGO_LUA50_SOURCE)
        if(NOT EXISTS "${SDGO_LUA50_SOURCE}/include/lua.h" OR NOT EXISTS "${SDGO_LUA50_SOURCE}/src/lapi.c")
            message(FATAL_ERROR "SDGO_LUA50_SOURCE must point to extracted official Lua 5.0.3 sources")
        endif()
        file(GLOB SDGO_LUA50_CORE CONFIGURE_DEPENDS "${SDGO_LUA50_SOURCE}/src/*.c")
        add_library(ai_test_lua50 STATIC ${SDGO_LUA50_CORE}
            "${SDGO_LUA50_SOURCE}/src/lib/lauxlib.c"
            "${SDGO_LUA50_SOURCE}/src/lib/lbaselib.c"
            "${SDGO_LUA50_SOURCE}/src/lib/lstrlib.c")
        target_include_directories(ai_test_lua50 PUBLIC "${SDGO_LUA50_SOURCE}/include")
        target_include_directories(ai_test_lua50 PRIVATE "${SDGO_LUA50_SOURCE}/src")
        target_compile_definitions(ai_test_lua50 PRIVATE _CRT_SECURE_NO_WARNINGS)
        add_executable(script_registration_test tests/script_registration_test.cpp)
        target_link_libraries(script_registration_test PRIVATE sdgo_script_registration ai_test_lua50)
        sdgo_strict(script_registration_test)
        target_link_options(script_registration_test PRIVATE /BASE:0x18000000 /DYNAMICBASE:NO)
        add_test(NAME script_registration_real_lua50 COMMAND script_registration_test)
        set_tests_properties(script_registration_real_lua50 PROPERTIES TIMEOUT 30)
        add_test(NAME unit_native_programs_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_native_programs.py" -v)
        set_tests_properties(unit_native_programs_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "SDGO_UNIT_PROGRAM_VALIDATOR=$<TARGET_FILE:script_registration_test>" TIMEOUT 60)
        set(SDGO_SCRIPT_NATIVE_CLIENT "" CACHE FILEPATH "Optional original client for bounded original Lua API replay")
        if(SDGO_SCRIPT_NATIVE_CLIENT)
            add_test(NAME script_registration_original_api_replay COMMAND script_registration_test --native-api "${SDGO_SCRIPT_NATIVE_CLIENT}")
            set_tests_properties(script_registration_original_api_replay PROPERTIES TIMEOUT 30)
        endif()
        add_executable(mission_ai_lua_test tools/lua50_host.c)
        target_link_libraries(mission_ai_lua_test PRIVATE ai_test_lua50)
        add_test(NAME mission_ai_lua_offline COMMAND mission_ai_lua_test mods/mission_ai/test/sim_mission.lua)
        set_tests_properties(mission_ai_lua_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    else()
        message(STATUS "Lua simulation disabled; set SDGO_LUA50_SOURCE to include it")
    endif()
endif()

# Optional full-title adapters use the current shared command grammar.
foreach(mode coop pvp)
    add_executable(${mode}_title_rules_test tests/${mode}_title_rules_test.cpp)
    target_link_libraries(${mode}_title_rules_test PRIVATE sdgo_api)
    sdgo_strict(${mode}_title_rules_test)
endforeach()

# Offline room-title tests and real-core isolated Agent bridge.
add_executable(room_metadata_common_test tests/room_metadata_common_test.cpp)
target_link_libraries(room_metadata_common_test PRIVATE room_metadata_common)
sdgo_strict(room_metadata_common_test)

add_executable(room_metadata_allocation_test tests/room_metadata_allocation_test.cpp mods/room_titles_server/room_titles_server_logic.cpp)
target_link_libraries(room_metadata_allocation_test PRIVATE room_metadata_common)
sdgo_strict(room_metadata_allocation_test)

add_executable(room_titles_client_state_test tests/room_titles_client_state_test.cpp)
target_link_libraries(room_titles_client_state_test PRIVATE room_titles_client_state)
sdgo_strict(room_titles_client_state_test)

add_executable(room_titles_transport_test tests/room_titles_transport_test.cpp)
target_link_libraries(room_titles_transport_test PRIVATE sdgo_api room_metadata_common)
sdgo_strict(room_titles_transport_test)

add_executable(room_titles_native_ui_test tests/room_titles_native_ui_test.cpp mods/room_titles/native_ui.cpp)
target_link_libraries(room_titles_native_ui_test PRIVATE sdgo_api user32)
sdgo_strict(room_titles_native_ui_test)

add_executable(room_titles_client_test tests/room_titles_client_test.cpp)
target_link_libraries(room_titles_client_test PRIVATE room_titles_client_state room_titles_native_transport user32)
sdgo_strict(room_titles_client_test)

add_executable(room_titles_server_test tests/room_titles_server_test.cpp mods/room_titles_server/room_titles_server_logic.cpp)
target_include_directories(room_titles_server_test PRIVATE mods/room_titles_server)
target_link_libraries(room_titles_server_test PRIVATE room_metadata_common)
sdgo_strict(room_titles_server_test)

add_executable(room_titles_server_native_test tests/room_titles_server_native_test.cpp mods/room_titles_server/room_titles_server_logic.cpp)
target_include_directories(room_titles_server_native_test PRIVATE mods/room_titles_server)
target_link_libraries(room_titles_server_native_test PRIVATE sdgo_api room_metadata_common bcrypt)
sdgo_strict(room_titles_server_native_test)

add_executable(room_title_native_test tests/room_title_native_test.cpp)
target_link_libraries(room_title_native_test PRIVATE bcrypt)
sdgo_strict(room_title_native_test)

add_executable(room_titles_agent_test tests/room_titles_agent_test.cpp)
target_link_libraries(room_titles_agent_test PRIVATE sdgo_api room_metadata_common)
sdgo_strict(room_titles_agent_test)

add_library(room_titles_agent_conflict_mod SHARED tests/room_titles_agent_test.cpp)
target_compile_definitions(room_titles_agent_conflict_mod PRIVATE ROOM_TITLES_AGENT_CONFLICT_MOD=1)
target_link_libraries(room_titles_agent_conflict_mod PRIVATE sdgo_api)
sdgo_strict(room_titles_agent_conflict_mod)


if(BUILD_TESTING)
    foreach(test room_metadata_common room_metadata_allocation room_titles_client_state room_titles_transport room_titles_native_ui room_titles_client room_titles_server room_titles_server_native coop_title_rules pvp_title_rules)
        add_test(NAME ${test}_offline COMMAND ${test}_test)
        set_tests_properties(${test}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    if(SDGO_POWERSHELL)
        add_test(NAME room_titles_agent_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/room_titles_agent_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(room_titles_agent_offline PROPERTIES TIMEOUT 90)
    endif()
    set(SDGO_ROOM_TITLE_NATIVE_HOST "" CACHE FILEPATH "Optional known BattleServer for bounded constructor fixture")
    set(SDGO_ROOM_TITLE_NATIVE_CLIENT "" CACHE FILEPATH "Optional known GOnline for isolated native input-limit replay")
    if(SDGO_ROOM_TITLE_NATIVE_CLIENT)
        add_test(NAME room_titles_native_input_offline COMMAND room_titles_native_ui_test "${SDGO_ROOM_TITLE_NATIVE_CLIENT}")
        set_tests_properties(room_titles_native_input_offline PROPERTIES TIMEOUT 30)
    endif()
    if(SDGO_ROOM_TITLE_NATIVE_HOST)
        add_test(NAME room_title_native_offline COMMAND room_title_native_test "${SDGO_ROOM_TITLE_NATIVE_HOST}")
        set_tests_properties(room_title_native_offline PROPERTIES TIMEOUT 30)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(resource_bindings_test tests/resource_bindings_test.cpp core/hooks.cpp)
    target_link_libraries(resource_bindings_test PRIVATE sdgo_resource_bindings minhook)
    target_include_directories(resource_bindings_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core")
    target_link_options(resource_bindings_test PRIVATE /BASE:0x18000000 /DYNAMICBASE:NO)
    sdgo_strict(resource_bindings_test)
    add_test(NAME resource_bindings_policy COMMAND resource_bindings_test)
    set(SDGO_RESOURCE_NATIVE_CLIENT "" CACHE FILEPATH "Optional original client for bounded resource CALL-site replay")
    if(SDGO_RESOURCE_NATIVE_CLIENT)
        add_test(NAME resource_bindings_original_site COMMAND resource_bindings_test --native-client "${SDGO_RESOURCE_NATIVE_CLIENT}")
        add_test(NAME resource_bindings_original_site_fatal COMMAND resource_bindings_test --native-client "${SDGO_RESOURCE_NATIVE_CLIENT}" --fatal)
        set_tests_properties(resource_bindings_original_site resource_bindings_original_site_fatal PROPERTIES TIMEOUT 45)
    endif()
    add_test(NAME unit_relocation_plan_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_relocation_plan.py" -v)
    set_tests_properties(unit_relocation_plan_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_executable(new_account_policy_test tests/new_account_policy_test.cpp)
    sdgo_strict(new_account_policy_test)
    add_test(NAME new_account_policy COMMAND new_account_policy_test)
    set_tests_properties(new_account_policy PROPERTIES TIMEOUT 30)
    # Opt-in SQL fixture: needs an isolated container and local credentials.
    add_executable(new_account_sql_test tests/new_account_sql_test.cpp)
    target_link_libraries(new_account_sql_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(new_account_sql_test)
    add_test(NAME collection_capsule_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_collection*.py" -v)
    set_tests_properties(collection_capsule_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(collection_capsule_sql_test tests/collection_capsule_sql_test.cpp)
    target_link_libraries(collection_capsule_sql_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(collection_capsule_sql_test)
endif()

# Battle AI component tests never enter a shipped payload.
add_executable(battle_ai_math_test tests/battle_ai_math_test.cpp)
sdgo_strict(battle_ai_math_test)
add_executable(battle_ai_capabilities_test tests/battle_ai_capabilities_test.cpp)
sdgo_strict(battle_ai_capabilities_test)
add_executable(battle_ai_observation_test tests/battle_ai_observation_test.cpp)
target_link_libraries(battle_ai_observation_test PRIVATE sdgo_api)
sdgo_strict(battle_ai_observation_test)
add_executable(battle_ai_navigation_test tests/battle_ai_navigation_test.cpp)
sdgo_strict(battle_ai_navigation_test)
add_executable(battle_ai_map_transport_test tests/battle_ai_map_transport_test.cpp)
sdgo_strict(battle_ai_map_transport_test)
add_executable(battle_ai_map_gate_test tests/battle_ai_map_gate_test.cpp)
sdgo_strict(battle_ai_map_gate_test)
add_executable(battle_ai_map_path_test tests/battle_ai_map_path_test.cpp)
sdgo_strict(battle_ai_map_path_test)
add_executable(battle_ai_map_geometry_test tests/battle_ai_map_geometry_test.cpp)
sdgo_strict(battle_ai_map_geometry_test)
if(BUILD_TESTING)
    add_test(NAME battle_ai_map_transport_offline COMMAND battle_ai_map_transport_test)
    add_test(NAME battle_ai_map_gate_offline COMMAND battle_ai_map_gate_test)
    add_test(NAME battle_ai_map_path_offline COMMAND battle_ai_map_path_test)
    add_test(NAME battle_ai_map_geometry_offline COMMAND battle_ai_map_geometry_test)
endif()
add_executable(battle_ai_special_budget_test tests/battle_ai_special_budget_test.cpp)
sdgo_strict(battle_ai_special_budget_test)
add_executable(battle_ai_items_test tests/battle_ai_items_test.cpp)
sdgo_strict(battle_ai_items_test)
add_executable(battle_ai_loadout_test tests/battle_ai_loadout_test.cpp)
sdgo_strict(battle_ai_loadout_test)
add_executable(battle_ai_server_native_test tests/battle_ai_server_native_test.cpp)
target_link_libraries(battle_ai_server_native_test PRIVATE sdgo_api)
sdgo_strict(battle_ai_server_native_test)
add_dependencies(battle_ai_server_native_test battle_ai_server ai_catalog_server sdgo_core)
add_library(battle_ai_bridge_fixture SHARED tests/battle_ai_bridge_fixture.cpp)
sdgo_strict(battle_ai_bridge_fixture)
add_executable(match_ai_loadout_bridge_test tests/match_ai_loadout_bridge_test.cpp)
target_link_libraries(match_ai_loadout_bridge_test PRIVATE sdgo_api)
sdgo_strict(match_ai_loadout_bridge_test)
if(TARGET ai_test_lua50)
 add_executable(battle_ai_lua_test tools/lua50_host.c)
 target_link_libraries(battle_ai_lua_test PRIVATE ai_test_lua50)
 add_executable(battle_ai_native_test tests/battle_ai_native_test.cpp)
 target_link_libraries(battle_ai_native_test PRIVATE ai_test_lua50 sdgo_api)
 sdgo_strict(battle_ai_native_test)
endif()

if(BUILD_TESTING)
    foreach(component math capabilities observation navigation special_budget items loadout)
        add_test(NAME battle_ai_${component}_offline COMMAND battle_ai_${component}_test)
    endforeach()
    if(TARGET battle_ai_lua_test)
        foreach(policy policy universal)
            add_test(NAME battle_ai_${policy}_offline
                COMMAND battle_ai_lua_test tests/battle_ai_${policy}.lua)
            set_tests_properties(battle_ai_${policy}_offline PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
        endforeach()
    endif()
endif()

# Independent hangar policy, native ABI and opt-in SQL fixtures; never staged.
if(BUILD_TESTING)
    add_executable(hangar_capacity_policy_test tests/hangar_capacity_policy_test.cpp)
    sdgo_strict(hangar_capacity_policy_test)
    add_test(NAME hangar_capacity_policy_offline COMMAND hangar_capacity_policy_test)
    set_tests_properties(hangar_capacity_policy_offline PROPERTIES TIMEOUT 30)
    add_executable(hangar_capacity_native_test tests/hangar_capacity_native_test.cpp)
    target_link_libraries(hangar_capacity_native_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(hangar_capacity_native_test)
    add_test(NAME hangar_capacity_native_offline COMMAND hangar_capacity_native_test)
    set_tests_properties(hangar_capacity_native_offline PROPERTIES TIMEOUT 30)
    add_executable(hangar_capacity_sql_emit tests/hangar_capacity_sql_emit.cpp)
    sdgo_strict(hangar_capacity_sql_emit)
    add_executable(hangar_capacity_sql_test tests/hangar_capacity_sql_test.cpp)
    target_link_libraries(hangar_capacity_sql_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(hangar_capacity_sql_test)
    target_compile_options(hangar_capacity_sql_test PRIVATE /Ob0)
    add_custom_target(pvp_population_regression_build DEPENDS
        game_test_quiet_audio_test game_test_unit_select_actions_test game_test_sealed_room_title_test
    sdgo_core d3d9_shim winmm_shim fps_limit
        pvp_ai match_ai room_random room_random_client game_test_client game_test_server
        pvp_ai_test match_ai_test pvp_team_plan_test pvp_title_rules_test
        room_random_test random_loadout_regression_test room_random_client_policy_test
        game_test_room_actions_test)
endif()

if(BUILD_TESTING)
    add_executable(announcement_policy_test tests/announcement_policy_test.cpp)
    target_link_libraries(announcement_policy_test PRIVATE sdgo_api)
    sdgo_strict(announcement_policy_test)
    add_test(NAME announcement_policy COMMAND announcement_policy_test)
    add_executable(announcement_window_messages_test tests/announcement_window_messages_test.cpp)
    target_link_libraries(announcement_window_messages_test PRIVATE ole32 oleaut32 uuid comctl32)
    sdgo_strict(announcement_window_messages_test)
    add_test(NAME announcement_window_messages COMMAND announcement_window_messages_test)
    add_executable(announcement_news_test tests/announcement_news_test.cpp)
    target_link_libraries(announcement_news_test PRIVATE sdgo_api winhttp)
    sdgo_strict(announcement_news_test)
    add_test(NAME announcement_news_model COMMAND announcement_news_test)
endif()

if(BUILD_TESTING)
    # These fixtures own a separate runtime tree and never enter a release stage.
    set(thread_fixture_root "${CMAKE_BINARY_DIR}/loader-thread/$<CONFIG>")
    add_library(loader_thread_probe SHARED tests/loader_thread_probe.cpp)
    target_link_libraries(loader_thread_probe PRIVATE sdgo_api)
    sdgo_strict(loader_thread_probe)
    set_target_properties(loader_thread_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${thread_fixture_root}/sdgo/mods/loader_thread_probe")
    file(GENERATE OUTPUT "${thread_fixture_root}/sdgo/mods/loader_thread_probe/mod.ini"
        CONTENT "[mod]\nid=loader_thread_probe\nname=Loader thread probe\napi=1\nhosts=*\ndll=loader_thread_probe.dll\n")
    foreach(mode IN ITEMS static dynamic legacy)
        add_executable(loader_thread_${mode} tests/loader_initial_thread_host.cpp)
        target_link_libraries(loader_thread_${mode} PRIVATE sdgo_api kernel32)
        target_compile_options(loader_thread_${mode} PRIVATE /GS- /Zl)
        target_link_options(loader_thread_${mode} PRIVATE /ENTRY:Run /NODEFAULTLIB)
        sdgo_strict(loader_thread_${mode})
        set_target_properties(loader_thread_${mode} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${thread_fixture_root}")
        add_dependencies(loader_thread_${mode} sdgo_core loader_thread_probe d3d9_shim)
        if(mode STREQUAL "static")
            target_compile_definitions(loader_thread_${mode} PRIVATE SDGO_STATIC_THREAD_FIXTURE)
            target_link_libraries(loader_thread_${mode} PRIVATE d3d9_shim)
        elseif(mode STREQUAL "legacy")
            target_compile_definitions(loader_thread_${mode} PRIVATE SDGO_LEGACY_THREAD_FIXTURE)
        endif()
        add_custom_command(TARGET loader_thread_${mode} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:d3d9_shim> "${thread_fixture_root}/d3d9.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:sdgo_core> "${thread_fixture_root}/sdgo/core.dll")
        add_test(NAME loader_initial_thread_${mode} COMMAND loader_thread_${mode})
        set_tests_properties(loader_initial_thread_${mode} PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
    endforeach()
    add_test(NAME unit_effect_resources_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_effect_resources.py" -v)
    set_tests_properties(unit_effect_resources_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_test(NAME unit_server_dependencies_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_server_dependencies.py" -v)
    set_tests_properties(unit_server_dependencies_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_test(NAME unit_model_integrity_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_model_integrity.py" -v)
    set_tests_properties(unit_model_integrity_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_test(NAME unit_native_content_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_native_content.py" -v)
    set_tests_properties(unit_native_content_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME unit_native_server_fields_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_native_server_fields.py" -v)
    set_tests_properties(unit_native_server_fields_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME unit_transform_graph_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_transform_graph.py" -v)
    set_tests_properties(unit_transform_graph_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_library(native_root_thread_probe SHARED tests/native_root_thread_probe.cpp)
    target_link_libraries(native_root_thread_probe PRIVATE sdgo_script_registration)
    sdgo_strict(native_root_thread_probe)
    add_library(native_skill_registry_probe SHARED tests/native_skill_registry_probe.cpp)
    target_link_libraries(native_skill_registry_probe PRIVATE sdgo_api)
    sdgo_strict(native_skill_registry_probe)
    add_library(native_skill_identity_actor_probe SHARED tests/native_skill_identity_actor_probe.cpp)
    target_link_libraries(native_skill_identity_actor_probe PRIVATE sdgo_api)
    sdgo_strict(native_skill_identity_actor_probe)
    add_library(native_skill_atlas_probe SHARED tests/native_skill_atlas_probe.cpp)
    target_link_libraries(native_skill_atlas_probe PRIVATE sdgo_api)
    sdgo_strict(native_skill_atlas_probe)
    add_library(native_model_binding_probe SHARED tests/native_model_binding_probe.cpp)
    target_link_libraries(native_model_binding_probe PRIVATE sdgo_api)
    sdgo_strict(native_model_binding_probe)
    add_executable(skill_atlas_format_test tests/skill_atlas_format_test.cpp
        support/native_resource_overlay/native_resource_overlay.cpp)
    target_link_libraries(skill_atlas_format_test PRIVATE sdgo_script_registration)
    sdgo_strict(skill_atlas_format_test)
    add_test(NAME unit_skill_atlases_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_skill_atlases.py" -v)
    set_tests_properties(unit_skill_atlases_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "SDGO_SKILL_ATLAS_VALIDATOR=$<TARGET_FILE:skill_atlas_format_test>" TIMEOUT 60)
    add_executable(skill_identity_test tests/skill_identity_test.cpp)
    target_link_libraries(skill_identity_test PRIVATE sdgo_skill_identity)
    sdgo_strict(skill_identity_test)
    add_test(NAME skill_identity_policy COMMAND skill_identity_test)
    add_executable(unit_transform_path_test tests/unit_transform_path_test.cpp)
    sdgo_strict(unit_transform_path_test)
    add_test(NAME unit_transform_path COMMAND unit_transform_path_test)
    add_executable(skill_identity_format_test tests/skill_identity_format_test.cpp)
    target_link_libraries(skill_identity_format_test PRIVATE sdgo_skill_identity)
    sdgo_strict(skill_identity_format_test)
    add_test(NAME unit_skill_identity_runtime_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_unit_skill_identity_runtime.py" -v)
    set_tests_properties(unit_skill_identity_runtime_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "SDGO_SKILL_IDENTITY_VALIDATOR=$<TARGET_FILE:skill_identity_format_test>" TIMEOUT 60)
    add_executable(skill_identity_native_test tests/skill_identity_native_test.cpp)
    target_link_libraries(skill_identity_native_test PRIVATE sdgo_skill_identity minhook)
    sdgo_strict(skill_identity_native_test)
    target_link_options(skill_identity_native_test PRIVATE /BASE:0x18000000 /DYNAMICBASE:NO)
    set(SDGO_SKILL_IDENTITY_SERVER "" CACHE FILEPATH "Optional original server for controlled skill-dispatch replay")
    if(SDGO_SKILL_IDENTITY_SERVER)
        add_test(NAME skill_identity_original_sites COMMAND skill_identity_native_test --native-server "${SDGO_SKILL_IDENTITY_SERVER}")
        add_test(NAME skill_identity_original_sites_rebased COMMAND skill_identity_native_test --native-server-rebased "${SDGO_SKILL_IDENTITY_SERVER}")
        set_tests_properties(skill_identity_original_sites skill_identity_original_sites_rebased PROPERTIES TIMEOUT 40)
    endif()
endif()
