#include "rml_sdl_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>

namespace setup_ui {
namespace {

// RmlUi colours arrive as premultiplied RGBA bytes; SDL wants straight alpha.
SDL_FColor to_sdl_color(const Rml::ColourbPremultiplied &colour) {
  const float alpha = static_cast<float>(colour.alpha) / 255.0F;
  if (colour.alpha == 0) {
    return SDL_FColor{0.0F, 0.0F, 0.0F, 0.0F};
  }
  const auto unpremultiply = [alpha](std::uint8_t channel) {
    const float value = static_cast<float>(channel) / alpha;
    return std::min(1.0F, value / 255.0F);
  };
  return SDL_FColor{unpremultiply(colour.red), unpremultiply(colour.green),
                    unpremultiply(colour.blue), alpha};
}

} // namespace

struct SdlRmlRenderer::Geometry {
  std::vector<SDL_Vertex> vertices;
  std::vector<int> indices;
};

SdlRmlRenderer::SdlRmlRenderer(SDL_Renderer *renderer) : renderer_(renderer) {
}

SdlRmlRenderer::~SdlRmlRenderer() {
  for (SDL_Texture *texture : textures_) {
    if (texture != nullptr) {
      SDL_DestroyTexture(texture);
    }
  }
  textures_.clear();
}

Rml::CompiledGeometryHandle SdlRmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                            Rml::Span<const int> indices) {
  Geometry geometry;
  geometry.vertices.reserve(vertices.size());
  for (const Rml::Vertex &vertex : vertices) {
    SDL_Vertex out{};
    out.position.x = vertex.position.x;
    out.position.y = vertex.position.y;
    out.color = to_sdl_color(vertex.colour);
    out.tex_coord.x = vertex.tex_coord.x;
    out.tex_coord.y = vertex.tex_coord.y;
    geometry.vertices.push_back(out);
  }
  geometry.indices.assign(indices.begin(), indices.end());
  const Rml::CompiledGeometryHandle handle = next_geometry_++;
  geometry_.emplace(handle, std::move(geometry));
  ++geometry_created_;
  return handle;
}

void SdlRmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                    Rml::TextureHandle texture) {
  const auto found = geometry_.find(handle);
  if (found == geometry_.end() || found->second.indices.empty() || renderer_ == nullptr) {
    return;
  }
  SDL_Texture *sdl_texture = texture != 0 && texture <= textures_.size()
                                 ? textures_[static_cast<std::size_t>(texture - 1)]
                                 : nullptr;
  // SDL_RenderGeometry applies the translation through the vertex stream, so
  // shift a scratch copy rather than mutating the compiled geometry.
  std::vector<SDL_Vertex> shifted = found->second.vertices;
  for (SDL_Vertex &vertex : shifted) {
    vertex.position.x += translation.x;
    vertex.position.y += translation.y;
  }
  SDL_RenderGeometry(renderer_, sdl_texture, shifted.data(), static_cast<int>(shifted.size()),
                     found->second.indices.data(), static_cast<int>(found->second.indices.size()));
  ++draw_calls_;
}

void SdlRmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
  geometry_.erase(handle);
}

Rml::TextureHandle SdlRmlRenderer::LoadTexture(Rml::Vector2i &texture_dimensions,
                                               const Rml::String &source) {
  // The setup screen ships no image textures; a surface request is refused
  // rather than silently serving a blank one.
  (void)source;
  texture_dimensions = Rml::Vector2i{0, 0};
  return 0;
}

Rml::TextureHandle SdlRmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                   Rml::Vector2i source_dimensions) {
  if (renderer_ == nullptr || source_dimensions.x <= 0 || source_dimensions.y <= 0) {
    return 0;
  }
  SDL_Texture *texture =
      SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC,
                        source_dimensions.x, source_dimensions.y);
  if (texture == nullptr) {
    return 0;
  }
  SDL_UpdateTexture(texture, nullptr, source.data(), source_dimensions.x * 4);
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  textures_.push_back(texture);
  return static_cast<Rml::TextureHandle>(textures_.size());
}

void SdlRmlRenderer::ReleaseTexture(Rml::TextureHandle texture) {
  if (texture == 0 || texture > textures_.size()) {
    return;
  }
  SDL_Texture *&slot = textures_[static_cast<std::size_t>(texture - 1)];
  if (slot != nullptr) {
    SDL_DestroyTexture(slot);
    slot = nullptr;
  }
}

void SdlRmlRenderer::EnableScissorRegion(bool enable) {
  scissor_enabled_ = enable;
  if (renderer_ == nullptr) {
    return;
  }
  if (!enable) {
    SDL_SetRenderClipRect(renderer_, nullptr);
    return;
  }
  const SDL_Rect rect{scissor_.Left(), scissor_.Top(), scissor_.Width(), scissor_.Height()};
  SDL_SetRenderClipRect(renderer_, &rect);
}

void SdlRmlRenderer::SetScissorRegion(Rml::Rectanglei region) {
  scissor_ = region;
  if (renderer_ != nullptr && scissor_enabled_) {
    const SDL_Rect rect{region.Left(), region.Top(), region.Width(), region.Height()};
    SDL_SetRenderClipRect(renderer_, &rect);
  }
}

} // namespace setup_ui
