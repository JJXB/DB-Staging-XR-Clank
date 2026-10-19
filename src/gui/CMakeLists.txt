target_sources(dosboxcommon PRIVATE
  clipboard.cpp
  cyshades.cpp
  cybermaxx.cpp
  iglasses.cpp
  trackers.cpp
  mapper.cpp
  sdl_gui.cpp
  titlebar.cpp
  xr.cpp
  osd.cpp
  xrinput.cpp
  xrstereo.cpp

  render/auto_image_adjustments.cpp
  render/auto_shader_switcher.cpp
  render/deinterlacer.cpp
  render/framegen.cpp
  render/lsfg.cpp
  render/opengl_renderer.cpp
  render/render.cpp
  render/scaler/scalers.cpp
  render/sdl_renderer.cpp
  render/vulkan_renderer.cpp
  render/d3d11_renderer.cpp
  render/d3d12_renderer.cpp
  render/shader.cpp
  render/shader_manager.cpp
  render/shader_pipeline.cpp
  render/shader_pragma_parser.cpp
)

if(APPLE)
target_sources(dosboxcommon PRIVATE
  render/macos_colorspace.mm
)
endif()

if (OPT_DEBUGGER)
  target_link_libraries(dosboxcommon PRIVATE imgui::imgui)
endif()

target_link_libraries(dosboxcommon PRIVATE OpenGL::GL simpleini)

if (C_OPENGL)
  target_link_libraries(dosboxcommon PRIVATE glad)
endif()

if (C_OPENXR)
  target_link_libraries(dosboxcommon PRIVATE OpenXR::openxr_loader)
endif()

if (C_OPENXR)
  target_sources(dosboxcommon PRIVATE
    render/xr_raster.cpp
  )
endif()

if (C_XR_VULKAN)
  target_include_directories(dosboxcommon PRIVATE ${Vulkan_INCLUDE_DIR})
  target_sources(dosboxcommon PRIVATE
    xrvulkan.cpp
    render/lsfg_vk.cpp
  )
endif()

if (C_XR_D3D11)
  target_sources(dosboxcommon PRIVATE
    xrd3d11.cpp
  )
  target_link_libraries(dosboxcommon PRIVATE d3d11 dxgi)
endif()

if (C_XR_D3D12)
  target_sources(dosboxcommon PRIVATE
    xrd3d12.cpp
  )
  target_link_libraries(dosboxcommon PRIVATE d3d12 dxgi d3dcompiler)
endif()