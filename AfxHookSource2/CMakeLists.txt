cmake_minimum_required (VERSION 3.24)

project ("AfxHookSource2")

################################################################################

set(AFXHOOKSOURCE2_SHADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_line_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_line_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_line_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_line_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_drawtexture_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_drawtexture_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_drawtexture_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_drawtexture_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_blit_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_blit_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_fisheye_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_fisheye_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_depth_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_depth_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_depth_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_depth_ps_5_0.h"
)

set(AFXHOOKSOURCE2_SHADERS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_line_vs_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_line_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_drawtexture_vs_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_drawtexture_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_blit_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_fisheye_ps_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_depth_vs_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_depth_ps_5_0.fxc"
)

add_custom_command(
    WORKING_DIRECTORY ${shaderbuilder_BINARY_DIR}
    OUTPUT ${AFXHOOKSOURCE2_SHADERS}
    DEPENDS shaderbuilder ${AFXHOOKSOURCE2_SHADERS_SOURCES}
    COMMAND ShaderBuilder.exe --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_line_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_line_vs_5_0"
	COMMAND ShaderBuilder.exe --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_line_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_line_ps_5_0"
    COMMAND ShaderBuilder.exe --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_drawtexture_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_drawtexture_vs_5_0"
	COMMAND ShaderBuilder.exe --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_drawtexture_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_drawtexture_ps_5_0"
	COMMAND ShaderBuilder.exe --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_blit_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_blit_ps_5_0"
	COMMAND ShaderBuilder.exe --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_fisheye_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_fisheye_ps_5_0"
	COMMAND ShaderBuilder.exe --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_depth_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_depth_vs_5_0"
	COMMAND ShaderBuilder.exe --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/afx_depth_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/build/afx_depth_ps_5_0"
)

add_custom_target(AfxHookSource2Shaders
    DEPENDS ${AFXHOOKSOURCE2_SHADERS}
)


################################################################################

add_library(${PROJECT_NAME} MODULE)

add_dependencies(${PROJECT_NAME}
    OpenEXR
    AfxHookSource2Shaders
    AfxHookSource2Snippets
)

SET(AFXHOOKSOURCE2SNIPPETS_OUTPUTS ${AFXHOOKSOURCE2SNIPPETS_OUTPUTS} PARENT_SCOPE)

add_definitions(-DGAME_CS2)

#set_target_properties(
#    ${PROJECT_NAME}
#    PROPERTIES 
#        OUTPUT_NAME "${PROJECT_NAME}_IF_YOU_USE_THIS_TO_JOIN_GOTV_OR_VAC_SERVERS_YOU_WILL_GET_VAC_BANNED"
#)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# - generate PDB in Release too
# - enable Hot Reload in Debug
target_compile_options(${PROJECT_NAME} PRIVATE
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Zi>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/ZI>
	)
target_link_options(${PROJECT_NAME} PRIVATE
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/DEBUG /OPT:REF /OPT:ICF>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/INCREMENTAL>
	)

target_include_directories(${PROJECT_NAME} PRIVATE
    ./
    ${AFX_OPENEXR_INCLUDE_DIRECTORIES}
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${AFX_OPENEXR_LINK_DIRECTORIES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${AFX_OPENEXR_LINK_LIBRARIES}
    afx_hook_source2
	dxguid.lib
	ole32.lib # filmmaker folder picker (IFileOpenDialog / CoCreateInstance)
    bcrypt.lib # corrosion fails to detect this
    $<$<CONFIG:Debug>:msvcrtd.lib> # corrision messes this up in latest rust version
)

target_sources(${PROJECT_NAME} PRIVATE
    ../deps/release/Detours/src/detours.cpp
    ../deps/release/Detours/src/detours.h
    ../deps/release/Detours/src/detver.h
    ../deps/release/Detours/src/disasm.cpp
    ../deps/release/Detours/src/disolarm.cpp
    ../deps/release/Detours/src/disolarm64.cpp
    ../deps/release/Detours/src/disolia64.cpp
    ../deps/release/Detours/src/disolx64.cpp
    ../deps/release/Detours/src/disolx86.cpp
    ../deps/release/Detours/src/image.cpp
    ../deps/release/Detours/src/modules.cpp

    ../deps/release/prop/cs2/sdk_src/public/appframework/IAppSystem.h
    ../deps/release/prop/cs2/sdk_src/public/interfaces/interfaces.h
    ../deps/release/prop/cs2/sdk_src/public/tier0/memalloc.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/convar.cpp
    ../deps/release/prop/cs2/sdk_src/public/tier1/convar.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/iconvar.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/KeyValues.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/strtools.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/utlmap.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/utlmemory.cpp
    ../deps/release/prop/cs2/sdk_src/public/tier1/utlmemory.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.cpp
    ../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/utlvector.h
    ../deps/release/prop/cs2/sdk_src/public/tier1/bufferstring.cpp
    ../deps/release/prop/cs2/sdk_src/public/tier1/bufferstring.h
    ../deps/release/prop/cs2/sdk_src/public/vstdlib/IKeyValuesSystem.h
    ../deps/release/prop/cs2/sdk_src/public/Color.h
    ../deps/release/prop/cs2/sdk_src/public/cdll_int.h
    ../deps/release/prop/cs2/sdk_src/public/filesystem.h
    ../deps/release/prop/cs2/sdk_src/public/const.h
    ../deps/release/prop/cs2/sdk_src/public/entityhandle.h
    ../deps/release/prop/cs2/sdk_src/public/icvar.h
    ../deps/release/prop/cs2/sdk_src/public/igameevents.h
    ../deps/release/prop/cs2/sdk_src/public/igameuiservice.h
    ../deps/release/prop/cs2/sdk_src/tier1/KeyValues.cpp
    ../deps/release/prop/AfxHookSource/SourceSdkShared.cpp
    ../deps/release/prop/AfxHookSource/SourceSdkShared.h
    ../deps/release/prop/AfxHookSource/SourceInterfaces.h
    
    ../shared/vcpp/AfxAddr.cpp
    ../shared/vcpp/AfxAddr.h

    ../shared/AfxCommandLine.cpp
    ../shared/AfxCommandLine.h
    ../shared/AfxConsole.cpp
    ../shared/AfxConsole.h
    ../shared/AfxDetours.cpp
    ../shared/AfxDetours.h
    ../shared/AfxMath.cpp
    ../shared/AfxMath.h
    ../shared/AfxOutStreams.cpp
    ../shared/AfxOutStreams.h    
    ../shared/CamIO.cpp
    ../shared/CamIO.h
    ../shared/CamPath.cpp
    ../shared/CamPath.h
    ../shared/CamPathRs.cpp
    ../shared/CommandSystem.cpp
    ../shared/CommandSystem.h
    ../shared/ConsolePrinter.h
    ../shared/EasySampler.cpp
    ../shared/EasySampler.h
    ../shared/FFITools.h
    ../shared/FileTools.cpp
    ../shared/FileTools.h
    ../shared/FovScaling.cpp
    ../shared/FovScaling.h
    ../shared/GrowingBufferPoolThreadSafe.h
    ../shared/ImageTransformer.cpp
    ../shared/ImageTransformer.h    
    ../shared/OutVideoStreamCreators.h
    ../shared/OpenExrOutput.cpp
    ../shared/OpenExrOutput.h    
    ../shared/RawOutput.cpp
    ../shared/RawOutput.h    
    ../shared/RecordingSettings.cpp
    ../shared/RecordingSettings.h    
    ../shared/StringTools.cpp
    ../shared/StringTools.h
    ../shared/ThreadPool.h
    ../shared/binutils.cpp
    ../shared/binutils.h
    ../shared/MirvCamIO.cpp
    ../shared/MirvCamIO.h
    ../shared/MirvCampath.cpp
    ../shared/MirvCampath.h
	../shared/MirvDeathMsgFilter.cpp
	../shared/MirvDeathMsgFilter.h
    ../shared/MirvInput.cpp
    ../shared/MirvInput.h
    ../shared/MirvSkip.cpp
    ../shared/MirvSkip.h
    ../shared/RefCounted.h
    ../shared/RefCountedThreadSafe.h
    ../shared/TRefCounted.h

    addresses.cpp
    addresses.h

    AfxHookSource2Rs.cpp
    AfxHookSource2Rs.h

    AfxShaders.cpp
    AfxShaders.h

	CamIO.cpp
	CamIO.h

    CampathDrawer.cpp
    CampathDrawer.h

    ClientEntitySystem.cpp
    ClientEntitySystem.h

    ConsoleRs.cpp
    CVarRs.cpp

	DeathMsg.cpp
	DeathMsg.h

    GameEvents.cpp
    GameEvents.h

	Globals.cpp
	Globals.h

    hlaeFolder.cpp
    hlaeFolder.h
	
	ViewModel.cpp
	ViewModel.h

    main.cpp

	MirvColors.cpp
	MirvColors.h

	MirvCommands.cpp
	MirvCommands.h

	MirvFix.cpp
	MirvFix.h

	MirvTime.cpp
	MirvTime.h

    ReplaceName.cpp
    ReplaceName.h

    RenderCommands.cpp
    RenderCommands.h

    RenderServiceHooks.cpp
    RenderServiceHooks.h

    RenderSystemDX11Hooks.cpp
    RenderSystemDX11Hooks.h

    ViewportScaler.cpp
    ViewportScaler.h
    FisheyePass.cpp
    FisheyePass.h

    ReShadeAdvancedfx.cpp
    ReShadeAdvancedfx.h

	SchemaSystem.cpp
	SchemaSystem.h

	SceneSystem.cpp
	SceneSystem.h

    StreamSettings.h

    WrpConsole.cpp
    WrpConsole.h

    # --- Filmmaker demo-browser feature (self-contained module) ---
    Filmmaker/Filmmaker.cpp
    Filmmaker/Filmmaker.h
    Filmmaker/FilmmakerCommand.cpp
    Filmmaker/FilmmakerCommandUtil.cpp
    Filmmaker/FilmmakerCommandUtil.h
    Filmmaker/MarkerCommand.cpp
    Filmmaker/CameraTimelineCommand.cpp
    Filmmaker/CameraEditorCommand.cpp
    Filmmaker/FollowCommand.cpp
    Filmmaker/GraphEditorCommand.cpp

    Filmmaker/Config/DemoFolderStore.cpp
    Filmmaker/Config/DemoFolderStore.h

    Filmmaker/Demo/DemoEntry.h
    Filmmaker/Demo/DemoHeaderReader.cpp
    Filmmaker/Demo/DemoHeaderReader.h
    Filmmaker/Demo/DemoInfoReader.cpp
    Filmmaker/Demo/DemoInfoReader.h
    Filmmaker/Demo/DemoInfoHelper.cpp
    Filmmaker/Demo/DemoInfoHelper.h
    Filmmaker/Demo/DemoLibrary.cpp
    Filmmaker/Demo/DemoLibrary.h
    Filmmaker/Demo/DemoScanner.cpp
    Filmmaker/Demo/DemoScanner.h
    Filmmaker/Demo/PlayingDemoPath.cpp
    Filmmaker/Demo/PlayingDemoPath.h

    Filmmaker/Panorama/FilmmakerGuiJs.h
    Filmmaker/Panorama/FilmmakerMenu.cpp
    Filmmaker/Panorama/FilmmakerMenu.h
    Filmmaker/Panorama/PanoramaBridge.cpp
    Filmmaker/Panorama/PanoramaBridge.h
    Filmmaker/Panorama/MovieHud.cpp
    Filmmaker/Panorama/MovieHud.h
    Filmmaker/Panorama/MovieHudJs.h
    Filmmaker/Panorama/MarkerHud.cpp
    Filmmaker/Panorama/MarkerHud.h
    Filmmaker/Panorama/MarkerHudJs.h
    Filmmaker/Panorama/FxDebugHud.cpp
    Filmmaker/Panorama/FxDebugHud.h
    Filmmaker/Panorama/FxDebugHudJs.h
    Filmmaker/Panorama/CameraTimelineHud.cpp
    Filmmaker/Panorama/CameraTimelineHud.h
    Filmmaker/Panorama/CameraTimelineJs.h
    Filmmaker/Panorama/CameraEditorHud.cpp
    Filmmaker/Panorama/CameraEditorHud.h
    Filmmaker/Panorama/ConfigHud.cpp
    Filmmaker/Panorama/ConfigHud.h
    Filmmaker/Panorama/ConfigHudJs.h
    Filmmaker/Panorama/ConfigHudModifiersJs.h
    Filmmaker/MvmTestCommand.cpp
    Filmmaker/MvmTest.h
    Filmmaker/Panorama/TestHud.cpp
    Filmmaker/Panorama/TestHud.h
    Filmmaker/Panorama/TestHudJs.h
    Filmmaker/Cosmetics/CosmeticUiQueries.cpp
    Filmmaker/Cosmetics/CosmeticUiQueries.h
    Filmmaker/FilmmakerDebugCommand.cpp
    Filmmaker/FilmmakerDebugCommand.h
    Filmmaker/Panorama/PanoramaFindPanel.cpp
    Filmmaker/Panorama/PanoramaFindPanel.h
    Filmmaker/Panorama/CameraEditorJs.h
    Filmmaker/Panorama/CameraEditorWidgetsJs.h
    Filmmaker/Panorama/CameraEditorCustomizeJs.h
    Filmmaker/Panorama/CameraEditorCustomizePreviewJs.h
    Filmmaker/Panorama/CameraEditorFollowInspectorJs.h
    Filmmaker/Panorama/CameraEditorPathInspectorJs.h
    Filmmaker/Panorama/CameraEditorDebugJs.h
    Filmmaker/Panorama/GraphEditorExperimentHud.cpp
    Filmmaker/Panorama/GraphEditorExperimentHud.h
    Filmmaker/Panorama/GraphEditorJs.h
    Filmmaker/Panorama/DemoBarButtons.cpp
    Filmmaker/Panorama/DemoBarButtons.h
    Filmmaker/Panorama/DemoBarButtonsJs.h

    Filmmaker/Movie/CameraBridge.h
    Filmmaker/Movie/CamMarkers.cpp
    Filmmaker/Movie/CamMarkers.h
    Filmmaker/Movie/CamPathEval.cpp
    Filmmaker/Movie/CamPathEval.h
    Filmmaker/Movie/CamPlayback.cpp
    Filmmaker/Movie/CamPlayback.h
    Filmmaker/Movie/CameraPath.cpp
    Filmmaker/Movie/CameraPathPicking.cpp
    Filmmaker/Movie/CameraPath.h
    Filmmaker/Movie/GraphExpModel.cpp
    Filmmaker/Movie/GraphExpModel.h
    Filmmaker/Movie/FollowCamera.cpp
    Filmmaker/Movie/FollowCamera.h
    Filmmaker/Movie/FollowCameraMath.cpp
    Filmmaker/Movie/FollowCameraMath.h
    Filmmaker/Movie/FollowTargetProviders.cpp
    Filmmaker/Movie/FollowTargetProviders.h
    Filmmaker/Movie/FollowTargetDiscovery.cpp
    Filmmaker/Movie/FollowTargetDiscovery.h
    Filmmaker/Movie/FollowEventIndex.cpp
    Filmmaker/Movie/FollowEventIndex.h
    Filmmaker/Movie/FollowCameraStateJson.cpp
    Filmmaker/Movie/ActionCam.cpp
    Filmmaker/Movie/ActionCam.h
    Filmmaker/Movie/DemoEndHold.cpp
    Filmmaker/Movie/DemoEndHold.h
    Filmmaker/Movie/BodyCam.cpp
    Filmmaker/Movie/BodyCam.h
    Filmmaker/Movie/ViewFx.cpp
    Filmmaker/Movie/ViewFx.h
    Filmmaker/Movie/ViewFxVm.cpp
    Filmmaker/Movie/ViewFxVm.h
    Filmmaker/Movie/ParticleFx.cpp
    Filmmaker/Movie/ParticleFx.h
    Filmmaker/Movie/ParticleFxInternal.h
    Filmmaker/Movie/ParticleFxRules.cpp
    Filmmaker/Movie/ParticleFxHook.cpp
    Filmmaker/Movie/ParticleFxSpray.cpp
    Filmmaker/Movie/ParticleFxMoney.cpp
    Filmmaker/Movie/ParticleFxSettings.cpp
    Filmmaker/Movie/ParticleFxDiagnostics.cpp
    Filmmaker/Movie/ParticleFxPrefetch.cpp
    Filmmaker/Movie/FxAlign.cpp
    Filmmaker/Movie/FxAlign.h
    Filmmaker/Movie/ThirdPersonCamera.cpp
    Filmmaker/Movie/ThirdPersonCamera.h
    Filmmaker/Movie/ThirdPersonCameraMath.h
    Filmmaker/Movie/MovieMode.cpp
    Filmmaker/Movie/MovieMode.h

    Filmmaker/Cosmetics/CosmeticProfile.cpp
    Filmmaker/Cosmetics/CosmeticProfile.h
    Filmmaker/Cosmetics/CosmeticCatalog.cpp
    Filmmaker/Cosmetics/CosmeticCatalog.h
    Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp
    Filmmaker/Cosmetics/CosmeticDemoSync.cpp
    Filmmaker/Cosmetics/CosmeticPaintKitBridge.cpp
    Filmmaker/Cosmetics/CosmeticModelSwapInternal.h
    Filmmaker/Cosmetics/CosmeticFnResolver.cpp
    Filmmaker/Cosmetics/CosmeticKnifeSwap.cpp
    Filmmaker/Cosmetics/CosmeticGloveSwap.cpp
    Filmmaker/Cosmetics/CosmeticOverrideSystem.h
    Filmmaker/Cosmetics/CosmeticModelSwap.cpp
    Filmmaker/Cosmetics/CosmeticModelSwap.h
    Filmmaker/Cosmetics/CosmeticDirectComposite.cpp
    Filmmaker/Cosmetics/CosmeticDirectComposite.h
    Filmmaker/Cosmetics/CosmeticAnimFix.cpp
    Filmmaker/Cosmetics/CosmeticAnimFix.h
    Filmmaker/Cosmetics/CosmeticCommands.cpp
    Filmmaker/Cosmetics/CosmeticDebug.cpp
    Filmmaker/Cosmetics/CosmeticDebugInternal.h
    Filmmaker/Cosmetics/CosmeticDebugEvents.cpp
    Filmmaker/Cosmetics/CosmeticDebugLog.cpp
    Filmmaker/Cosmetics/CosmeticDebugLog.h
    Filmmaker/Cosmetics/CosmeticGloveLabels.cpp
    Filmmaker/Cosmetics/CosmeticGloveLabels.h
    Filmmaker/Cosmetics/CosmeticGloveSkinTable.cpp

    Filmmaker/Platform/FolderPicker.cpp
    Filmmaker/Platform/FolderPicker.h
    Filmmaker/Platform/JsonBuilder.cpp
    Filmmaker/Platform/JsonBuilder.h
    Filmmaker/Platform/JsonParser.cpp
    Filmmaker/Platform/JsonParser.h
    Filmmaker/Platform/ProtobufWire.cpp
    Filmmaker/Platform/ProtobufWire.h
    Filmmaker/Platform/TextEncoding.cpp
    Filmmaker/Platform/TextEncoding.h
)

source_group("AfxHookSource2/Filmmaker" REGULAR_EXPRESSION "/AfxHookSource2/Filmmaker/")

add_executable(FollowCameraMathTests
    ../automation/tests/follow-camera-math-tests.cpp
    Filmmaker/Movie/FollowCameraMath.cpp
    Filmmaker/Movie/FollowCameraMath.h
    Filmmaker/Movie/ThirdPersonCameraMath.h
)
target_compile_features(FollowCameraMathTests PRIVATE cxx_std_17)
add_test(NAME FollowCameraMathTests COMMAND FollowCameraMathTests)

# This is far form proper, but it will do for now:
source_group("deps/release/Detours" REGULAR_EXPRESSION "/deps/release/Detours/")
source_group("deps/release/Detours/src" REGULAR_EXPRESSION "/deps/release/Detours/src/")
source_group("deps/release/prop" REGULAR_EXPRESSION "/deps/release/prop/")
source_group("deps/release/prop/AfxHookSource" REGULAR_EXPRESSION "/deps/release/prop/AfxHookSource")
source_group("deps/release/prop/cs2" REGULAR_EXPRESSION "/deps/release/prop/cs2/")
source_group("deps/release/prop/cs2/sdk_src/public" REGULAR_EXPRESSION "/deps/release/prop/cs2/sdk_src/public/")
source_group("deps/release/prop/cs2/sdk_src/public/appframework" REGULAR_EXPRESSION "/deps/release/prop/cs2/sdk_src/public/appframework/")
source_group("deps/release/prop/cs2/sdk_src/public/interfaces" REGULAR_EXPRESSION "/deps/release/prop/cs2/sdk_src/public/interfaces/")
source_group("deps/release/prop/cs2/sdk_src/public/tier0" REGULAR_EXPRESSION "/deps/release/prop/cs2/sdk_src/public/tier0/")
source_group("deps/release/prop/cs2/sdk_src/public/tier1" REGULAR_EXPRESSION "/deps/release/prop/cs2/sdk_src/public/tier1/")


source_group("shared" REGULAR_EXPRESSION "/shared/")
source_group("shared/hooks" REGULAR_EXPRESSION "/shared/hooks/")
source_group("shared/vcpp" REGULAR_EXPRESSION "/shared/vcpp/")
source_group("AfxHookSource2" REGULAR_EXPRESSION "/AfxHookSource2/")
