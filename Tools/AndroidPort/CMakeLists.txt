cmake_minimum_required(VERSION 3.22)
# Android ARM64 client port: builds libwow_android.so for the APK.

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The portable runtime currently requires 64-bit pointers")
endif ()
add_compile_definitions(WOWDECOMP_ARCH_64=1)


set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "Use vendored MPQ codecs" FORCE)
set(STORM_SKIP_INSTALL ON CACHE BOOL "Embedded MPQ reader" FORCE)
set(STORM_BUILD_TESTS OFF CACHE BOOL "Use the port integration tests" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/ThirdParty/StormLib ${CMAKE_CURRENT_BINARY_DIR}/stormlib)
set_target_properties(storm PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
add_library(wow_storm_platform STATIC StormPlatform.cpp TimePlatform.cpp FunctionValidation.cpp SystemPlatform.cpp BlizzardLocks.cpp StormSync.cpp ThreadLocalStorage.cpp GxPlatform.cpp NativeInput.cpp)
target_sources(wow_storm_platform PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/SUni.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/ConvertUTF.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/CDataStore.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/Status.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/MD5.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/SBig.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/SArc4.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/SStr.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/SOption.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/soprintf.cpp
)
set_target_properties(wow_storm_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wow_storm_platform PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(wow_storm_platform PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Source/Tempest ${CMAKE_CURRENT_BINARY_DIR}/tempest)
set_target_properties(Tempest PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Tempest PUBLIC wow_storm_platform)
# Reconstructed code still includes intentional float/int bit representations.
target_compile_options(Tempest PRIVATE -fno-strict-aliasing)
add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Source/lua-5.1 ${CMAKE_CURRENT_BINARY_DIR}/lua)
set_target_properties(lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(lua PUBLIC wow_storm_platform)
# Bionic FORTIFY aborts inside Lua's GC (__strchr_chk on the weak-table __mode string,
# lgc.c traversetable): Lua strings live past their TString header, which the compiler's
# object-size bound does not cover. Lua (and the UI code using its strings) builds unfortified.
target_compile_options(lua PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

add_library(wow_port_core STATIC ArchiveReader.cpp AuthSession.cpp SrpAuthentication.cpp AuthWire.cpp ServerEndpoint.cpp NativeTcp.cpp OsNetAddress.cpp TextureDecoder.cpp SFilePlatform.cpp NativeAssetPaths.cpp ClientData.cpp OsFilePlatform.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/Blit.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/BLPFile/blp.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Images/MippedImg.cpp)
target_sources(wow_port_core PRIVATE RealmSession.cpp)
target_sources(wow_port_core PRIVATE WorldWire.cpp)
target_sources(wow_port_core PRIVATE WorldAuthentication.cpp WorldSession.cpp)
target_sources(wow_port_core PRIVATE CharacterList.cpp)
target_sources(wow_port_core PRIVATE CharacterLogin.cpp)
target_sources(wow_port_core PRIVATE WorldPresence.cpp WorldAssets.cpp ClientDb.cpp CharacterCustomization.cpp GlueScene.cpp TerrainObjects.cpp WorldObjects.cpp PlayerState.cpp ServerEvents.cpp)
target_sources(wow_port_core PRIVATE GlueResources.cpp)
target_sources(wow_port_core PRIVATE
    ${CMAKE_SOURCE_DIR}/WoW/Source/WowServices/BigInteger.cpp
    ${CMAKE_SOURCE_DIR}/WoW/Source/WowServices/SRP6_Client.cpp
    ${CMAKE_SOURCE_DIR}/WoW/Source/WowServices/SRP6_Random.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/FrameScript/FrameScript.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/FrameScript/FrameScript_Object.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/FrameScript/FrameScript_Scripts.cpp)
target_link_libraries(wow_port_core PUBLIC Tempest lua storm)
set_target_properties(wow_port_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wow_port_core PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(wow_port_core PRIVATE -Wall -Wextra -Wpedantic)

# Original device-independent graphics code. Platform device creation is supplied
# separately by the native backend; this target does not select Windows D3D/WGL.
file(GLOB GX_COMMON_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/*.cpp)
list(FILTER GX_COMMON_SOURCES EXCLUDE REGEX "/Blit.cpp$")
set(GX_LAYOUT_SOURCES ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/Buffer.cpp ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/VertexLayout.cpp)
list(REMOVE_ITEM GX_COMMON_SOURCES ${GX_LAYOUT_SOURCES})
add_library(wow_gx_layout STATIC ${GX_LAYOUT_SOURCES})
set_target_properties(wow_gx_layout PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(wow_gx_layout PUBLIC wow_port_core)
add_library(wow_gx_common STATIC ${GX_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/CGxDevice/CGxDevice.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/CGxDevice/CGxShader.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Gx/CGxDevice/EmergencyMem.cpp)
target_sources(wow_gx_common PRIVATE TerrainModelPlacement.cpp TerrainGeometry.cpp TerrainPreview.cpp TerrainMaterials.cpp TerrainAlpha.cpp TerrainSurface.cpp TerrainWalker.cpp M2Geometry.cpp M2File.cpp M2Animation.cpp M2AnimationPlayer.cpp M2AnimatedModel.cpp M2ModelPreview.cpp M2Geosets.cpp M2PreviewPlayback.cpp M2PreviewMaterials.cpp M2PreviewDrawOrder.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/ShaderEffect/ShaderEffectCore.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/ShaderEffect/ShaderEffectLighting.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Model2/M2Light.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Model2/M2LightingState.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Model2/M2Skinning.cpp)
target_link_libraries(wow_gx_common PUBLIC wow_gx_layout wow_port_core)
set_target_properties(wow_gx_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(wow_events STATIC NativeEvents.cpp NativeRecycler.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/Prop.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/Coordinate.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/CSBasePriorityQueue.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/EvtContext.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/EvtInput.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/EvtQueue.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/EvtTimer.cpp)
set_target_properties(wow_events PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(wow_events PUBLIC wow_port_core)

add_library(wow_gles STATIC GlesRenderer.cpp)
set_target_properties(wow_gles PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(wow_gles PUBLIC wow_gx_layout wow_port_core EGL GLESv2)
target_compile_options(wow_gles PRIVATE -Wall -Wextra -Wpedantic)
target_sources(wow_gx_common PRIVATE CGxDeviceGles.cpp M2ModelPreviewDraw.cpp M2PreviewTextures.cpp TerrainPreviewDraw.cpp TerrainPreviewTextures.cpp GxStaticMesh.cpp WmoModel.cpp WorldCollision.cpp WorldPlayer.cpp WorldScene.cpp WorldSky.cpp M2Effects.cpp)
target_link_libraries(wow_gx_common PUBLIC wow_gles)

# Original interface engine (CSimpleTop/FrameXML/FrameScript UI, Gxu fonts, textures) used
# for the in-game FrameXML interface. Decompiled sources are built warning-free (-w).
file(GLOB UI_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/Engine/Source/Frame/*.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/FrameXML/*.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Gxu/*.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Scrn/*.cpp)
add_library(wow_ui_thirdparty STATIC
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/base/ftsystem.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/base/ftinit.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/base/ftdebug.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/base/ftbase.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/autohint/autohint.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/cff/cff.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/cid/type1cid.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/psaux/psaux.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/psnames/psnames.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/raster/raster.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/sfnt/sfnt.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/smooth/smooth.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/truetype/truetype.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/type1/type1.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src/winfonts/winfnt.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/expat/lib/xmlparse.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/expat/lib/xmlrole.c
    ${CMAKE_SOURCE_DIR}/ThirdParty/expat/lib/xmltok.c)
target_include_directories(wow_ui_thirdparty PUBLIC ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/include
    ${CMAKE_SOURCE_DIR}/ThirdParty/expat/lib ${CMAKE_CURRENT_SOURCE_DIR}/compat
    PRIVATE ${CMAKE_SOURCE_DIR}/ThirdParty/freetype/src)
target_compile_options(wow_ui_thirdparty PRIVATE -w)
set_target_properties(wow_ui_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(wow_ui STATIC ${UI_ENGINE_SOURCES}
    ${CMAKE_SOURCE_DIR}/Engine/Source/Services/TextBlock.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Services/Texture.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Services/TextureBlob.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Services/AsyncFile.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Services/Camera.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Storm/Source/SRgn.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/Handle.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/RCString.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/RefCount.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Base/CDataAllocator.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/CEvent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Event/CMouseEvent.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/Images/tga.cpp
    UiPlatform.cpp)
target_compile_options(wow_ui PRIVATE -w -fno-strict-aliasing -fsigned-char -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_libraries(wow_ui PUBLIC wow_ui_thirdparty wow_gx_common wow_events)
set_target_properties(wow_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (ANDROID)
    if (NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR "The Android foundation currently targets arm64-v8a only")
    endif ()
    add_library(wow_android SHARED AndroidBridge.cpp NetworkBridge.cpp GlueBridge.cpp WorldLoader.cpp WorldModels.cpp UiRuntime.cpp UiGameData.cpp UiMinimap.cpp UiModelFrames.cpp CrashReport.cpp)
    add_dependencies(wow_android wow_gx_common)
    # Whole-archive the math module so unported dependencies cannot hide in an
    # unused object file. The rest is retained through the startup entry points.
    target_link_libraries(wow_android PRIVATE wow_ui wow_ui_thirdparty wow_gx_common wow_events
        -Wl,--whole-archive Tempest -Wl,--no-whole-archive GLESv2 log)
    # Support devices using either 4 KiB or 16 KiB memory pages.
    target_link_options(wow_android PRIVATE "-Wl,-z,max-page-size=16384")
    target_compile_options(wow_android PRIVATE -Wall -Wextra -Wpedantic)
endif ()
