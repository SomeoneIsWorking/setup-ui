// rml_sdl_renderer.h — an RmlUi render interface over SDL3's 2D renderer.
//
// RmlUi 6 ships backends for OpenGL, Vulkan, DirectX, and SDL_GPU, but not for
// SDL_Renderer. Every host this port targets already presents through
// SDL_Renderer, so the setup screen uses the same path instead of creating a
// second graphics context. Geometry is drawn with SDL_RenderGeometry; the
// scissor region maps to SDL_RenderSetClipRect.
//
// Unsupported RmlUi features (clip masks, layers, filters) keep their default
// no-op behaviour, which is why the setup stylesheet avoids box shadows and
// other decorators that need them.
#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace setup_ui {

class SdlRmlRenderer final : public Rml::RenderInterface {
public:
  explicit SdlRmlRenderer(SDL_Renderer *renderer);
  ~SdlRmlRenderer() override;

  SdlRmlRenderer(const SdlRmlRenderer &) = delete;
  SdlRmlRenderer &operator=(const SdlRmlRenderer &) = delete;

  // -- Rml::RenderInterface -------------------------------------------------
  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                              Rml::Span<const int> indices) override;
  void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
  Rml::TextureHandle LoadTexture(Rml::Vector2i &texture_dimensions,
                                 const Rml::String &source) override;
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                     Rml::Vector2i source_dimensions) override;
  void ReleaseTexture(Rml::TextureHandle texture) override;
  void EnableScissorRegion(bool enable) override;
  void SetScissorRegion(Rml::Rectanglei region) override;

  // Test/diagnostic seam: geometry compiled since the last reset. A renderer
  // that silently drops every draw call is otherwise indistinguishable from a
  // document that produced none.
  std::size_t geometry_created() const {
    return geometry_created_;
  }
  std::size_t draw_calls() const {
    return draw_calls_;
  }
  void reset_counters() {
    geometry_created_ = 0;
    draw_calls_ = 0;
  }

private:
  struct Geometry;

  SDL_Renderer *renderer_;
  // Handles are plain integers owned by this table: RmlUi's handle type is an
  // integer, so geometry and textures are addressed through lookups rather
  // than by casting pointers into handles.
  std::unordered_map<Rml::CompiledGeometryHandle, Geometry> geometry_;
  std::vector<SDL_Texture *> textures_;
  Rml::CompiledGeometryHandle next_geometry_ = 1;
  std::size_t geometry_created_ = 0;
  std::size_t draw_calls_ = 0;
  bool scissor_enabled_ = false;
  Rml::Rectanglei scissor_;
};

} // namespace setup_ui
