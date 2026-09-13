// view.cpp — the in-app setup screen: an SDL3 window rendering a Session
// through RmlUi.
//
// Ownership: this file owns the window, renderer, RmlUi context, input
// translation, and DOM updates. It creates no second graphics context and no
// browser; the same SDL_Renderer path the game presents through draws the
// screen. The document is the shared setup.rml/setup.rcss pair embedded at
// build time; wording and rows come from the Session's config.
#include "setup_ui/setup_ui.h"

#include "assets.h"
#include "rml_sdl_renderer.h"

#include <RmlUi/Core.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace setup_ui {
namespace {

// Serves the embedded document pair to RmlUi and falls back to the real
// filesystem for anything else the library asks for (fonts, in particular).
class SetupFileInterface final : public Rml::FileInterface {
public:
  // RmlUi addresses open files with an integer handle; this table owns the
  // streams rather than casting stream pointers into handles.
  Rml::FileHandle Open(const Rml::String &path) override {
    const Rml::String leaf = leaf_name(path);
    std::unique_ptr<Base> stream;
    if (leaf == "setup.rml") {
      stream = std::make_unique<MemoryFile>(std::string(assets::kDocumentRml));
    } else if (leaf == "setup.rcss") {
      stream = std::make_unique<MemoryFile>(std::string(assets::kDocumentRcss));
    } else {
      auto file = std::make_unique<FileFile>(std::ifstream(path, std::ios::binary));
      if (!file->file) {
        return 0;
      }
      stream = std::move(file);
    }
    files_.push_back(std::move(stream));
    return static_cast<Rml::FileHandle>(files_.size());
  }

  void Close(Rml::FileHandle file) override {
    if (file == 0 || file > files_.size()) {
      return;
    }
    files_[static_cast<std::size_t>(file - 1)].reset();
  }

  // RmlUi's own signature; the parameter order is fixed by the library.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  std::size_t Read(void *buffer, std::size_t size, Rml::FileHandle file) override {
    Base *stream = lookup(file);
    return stream != nullptr ? stream->Read(static_cast<char *>(buffer), size) : 0;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool Seek(Rml::FileHandle file, long offset, int origin) override {
    Base *stream = lookup(file);
    return stream != nullptr && stream->Seek(offset, origin);
  }

  std::size_t Tell(Rml::FileHandle file) override {
    const Base *stream = lookup(file);
    return stream != nullptr ? stream->Tell() : 0;
  }

private:
  struct Base {
    virtual ~Base() = default;
    virtual std::size_t Read(char *buffer, std::size_t size) = 0;
    virtual bool Seek(long offset, int origin) = 0;
    virtual std::size_t Tell() const = 0;
  };

  struct MemoryFile final : Base {
    explicit MemoryFile(std::string contents) : data(std::move(contents)) {
    }
    std::size_t Read(char *buffer, std::size_t size) override {
      const std::size_t available = data.size() - position;
      const std::size_t count = std::min(size, available);
      std::copy_n(data.data() + position, count, buffer);
      position += count;
      return count;
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool Seek(long offset, int origin) override {
      long target = offset;
      if (origin == SEEK_CUR) {
        target = static_cast<long>(position) + offset;
      } else if (origin == SEEK_END) {
        target = static_cast<long>(data.size()) + offset;
      }
      if (target < 0 || static_cast<std::size_t>(target) > data.size()) {
        return false;
      }
      position = static_cast<std::size_t>(target);
      return true;
    }
    std::size_t Tell() const override {
      return position;
    }
    std::string data;
    std::size_t position = 0;
  };

  struct FileFile final : Base {
    explicit FileFile(std::ifstream stream) : file(std::move(stream)) {
    }
    std::ifstream file;
    std::size_t Read(char *buffer, std::size_t size) override {
      file.read(buffer, static_cast<std::streamsize>(size));
      return static_cast<std::size_t>(file.gcount());
    }
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool Seek(long offset, int origin) override {
      file.clear();
      file.seekg(offset, origin == SEEK_CUR   ? std::ios::cur
                         : origin == SEEK_END ? std::ios::end
                                              : std::ios::beg);
      return static_cast<bool>(file);
    }
    std::size_t Tell() const override {
      auto &mutable_file = const_cast<std::ifstream &>(file);
      return static_cast<std::size_t>(mutable_file.tellg());
    }
  };

  // The table of open streams: RmlUi's file handles are integers, so streams
  // are looked up rather than cast into handles.
  std::vector<std::unique_ptr<Base>> files_;

  Base *lookup(Rml::FileHandle handle) const {
    if (handle == 0 || handle > files_.size()) {
      return nullptr;
    }
    return files_[static_cast<std::size_t>(handle - 1)].get();
  }

  static Rml::String leaf_name(const Rml::String &path) {
    const std::size_t separator = path.find_last_of("/\\");
    return separator == Rml::String::npos ? path : path.substr(separator + 1);
  }
};

class SetupSystemInterface final : public Rml::SystemInterface {
public:
  double GetElapsedTime() override {
    return static_cast<double>(SDL_GetTicks()) / 1000.0;
  }

  void SetMouseCursor(const Rml::String &cursor_name) override {
    SDL_SystemCursor shape = SDL_SYSTEM_CURSOR_DEFAULT;
    if (cursor_name == "pointer") {
      shape = SDL_SYSTEM_CURSOR_POINTER;
    } else if (cursor_name == "text") {
      shape = SDL_SYSTEM_CURSOR_TEXT;
    } else if (cursor_name == "cross") {
      shape = SDL_SYSTEM_CURSOR_CROSSHAIR;
    }
    SDL_Cursor *cursor = SDL_CreateSystemCursor(shape);
    SDL_SetCursor(cursor);
  }

  void SetClipboardText(const Rml::String &text) override {
    SDL_SetClipboardText(text.c_str());
  }

  void GetClipboardText(Rml::String &text) override {
    char *clipboard = SDL_GetClipboardText();
    if (clipboard != nullptr) {
      text = clipboard;
      SDL_free(clipboard);
    }
  }
};

// Escapes text destined for SetInnerRML so titles cannot inject markup.
std::string escape(const std::string &text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char character : text) {
    switch (character) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    default:
      escaped.push_back(character);
      break;
    }
  }
  return escaped;
}

// The first usable system UI font. A consumer that ships its own passes an
// explicit path instead; failing to find any font is reported, never ignored.
std::string default_font_path() {
  static constexpr const char *kCandidates[] = {
      "/system/fonts/Roboto-Regular.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
      "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/noto-sans/NotoSans-Regular.ttf",
      "/Library/Fonts/Arial.ttf",
      "/System/Library/Fonts/Helvetica.ttc"};
  for (const char *candidate : kCandidates) {
    std::ifstream file(candidate, std::ios::binary);
    if (file) {
      return candidate;
    }
  }
  return {};
}

class ButtonListener final : public Rml::EventListener {
public:
  ButtonListener(std::vector<Request> &queue, RequestKind kind) : queue_(queue), kind_(kind) {
  }
  void ProcessEvent(Rml::Event &event) override {
    if (event.GetId() == Rml::EventId::Click) {
      queue_.push_back(Request{kind_});
    }
  }

private:
  std::vector<Request> &queue_;
  RequestKind kind_;
};

} // namespace

struct View::Impl {
  Session *session = nullptr;
  ViewOptions options;
  SDL_Window *window = nullptr;
  SDL_Surface *surface = nullptr;
  SDL_Renderer *renderer = nullptr;
  std::unique_ptr<SdlRmlRenderer> render_interface;
  std::unique_ptr<SetupSystemInterface> system_interface;
  std::unique_ptr<SetupFileInterface> file_interface;
  Rml::Context *context = nullptr;
  Rml::ElementDocument *document = nullptr;
  Rml::Element *title = nullptr;
  Rml::Element *message = nullptr;
  Rml::Element *hint = nullptr;
  Rml::Element *footer = nullptr;
  Rml::Element *rows = nullptr;
  Rml::Element *status = nullptr;
  Rml::Element *start = nullptr;
  Rml::Element *again = nullptr;
  Rml::Element *chooser = nullptr;
  Rml::Element *progress = nullptr;
  Rml::Element *progress_fill = nullptr;
  std::vector<Request> queue;
  std::unique_ptr<ButtonListener> browse_listener;
  std::unique_ptr<ButtonListener> start_listener;
  std::unique_ptr<ButtonListener> cancel_listener;
  std::unique_ptr<ButtonListener> again_listener;
  std::string error;
  bool owns_sdl_video = false;
  bool running = false;
  bool rml_initialised = false;

  void apply_static_text() {
    const Config &config = session->config();
    if (title != nullptr) {
      title->SetInnerRML(escape(config.title));
    }
    if (message != nullptr) {
      message->SetInnerRML(escape(config.message));
    }
    if (hint != nullptr) {
      hint->SetInnerRML(escape(config.hint));
    }
    if (footer != nullptr) {
      footer->SetInnerRML(escape(config.footer));
    }
  }

  void rebuild_rows() {
    if (rows == nullptr || document == nullptr) {
      return;
    }
    rows->SetInnerRML("");
    for (const Entry &entry : session->entries()) {
      Rml::ElementPtr row = document->CreateElement("div");
      row->SetClass("row", true);
      row->SetClass("provided", entry.provided);
      auto *row_element = rows->AppendChild(std::move(row));
      if (row_element == nullptr) {
        continue;
      }
      Rml::ElementPtr mark = document->CreateElement("span");
      mark->SetClass("mark", true);
      mark->SetInnerRML(entry.provided ? "&#10003;" : "");
      row_element->AppendChild(std::move(mark));
      Rml::ElementPtr label = document->CreateElement("span");
      label->SetClass("label", true);
      label->SetInnerRML(escape(entry.spec.label));
      row_element->AppendChild(std::move(label));
    }
  }

  void refresh() {
    // An accepted set hides the chooser and shows the completion text; a
    // rejected set keeps its rows and explains the refusal.
    if (start != nullptr) {
      // The start control stays visible so the flow is legible, and is
      // greyed out until the set validates.
      const bool ready =
          session->status() == Status::Ready || session->status() == Status::Accepted;
      start->SetClass("disabled", !ready);
    }
    if (again != nullptr) {
      again->SetProperty("visibility",
                         session->status() == Status::Collecting ? "hidden" : "visible");
    }
    if (chooser != nullptr) {
      const bool accepted = session->status() == Status::Accepted;
      chooser->SetProperty("visibility", accepted ? "hidden" : "visible");
    }
    if (status != nullptr) {
      status->SetClass("rejected", session->status() == Status::Rejected);
      status->SetClass("accepted", session->status() == Status::Accepted);
      const std::string shown = session->message();
      status->SetInnerRML(escape(shown));
    }
    if (progress_fill != nullptr) {
      std::ostringstream width;
      width << static_cast<int>(session->progress() * 100.0) << '%';
      progress_fill->SetProperty("width", width.str());
    }
    if (progress != nullptr) {
      progress->SetProperty("visibility", session->progress() > 0.0 ? "visible" : "hidden");
    }
    rebuild_rows();
  }

  // dp is one logical pixel on a 1x display; a phone reports a scale of 2-4,
  // so the stylesheet's dp sizes must follow it or the screen renders tiny.
  void apply_display_scale() {
    if (context == nullptr) {
      return;
    }
    float scale = options.density_ratio;
    if (scale <= 0.0F) {
      scale = window != nullptr ? SDL_GetWindowDisplayScale(window) : 1.0F;
    }
    if (scale < 1.0F) {
      scale = 1.0F;
    }
    context->SetDensityIndependentPixelRatio(scale);
  }

  // RmlUi's context dimensions are physical pixels; it divides by the
  // density-independent ratio itself to obtain the dp viewport.
  void apply_dimensions() {
    if (context == nullptr) {
      return;
    }
    int pixel_width = 0;
    int pixel_height = 0;
    if (window != nullptr) {
      SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    } else if (surface != nullptr) {
      pixel_width = surface->w;
      pixel_height = surface->h;
    }
    context->SetDimensions(Rml::Vector2i{pixel_width, pixel_height});
  }

  void handle_event(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      queue.push_back(Request{RequestKind::Cancel});
      break;
    case SDL_EVENT_KEY_DOWN:
      if (event.key.key == SDLK_ESCAPE) {
        queue.push_back(Request{RequestKind::Cancel});
      } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER ||
                 event.key.key == SDLK_SPACE) {
        if (session->status() == Status::Ready || session->status() == Status::Accepted) {
          queue.push_back(Request{RequestKind::Start});
        } else {
          queue.push_back(Request{RequestKind::Browse});
        }
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (context != nullptr) {
        context->ProcessMouseMove(static_cast<int>(event.motion.x),
                                  static_cast<int>(event.motion.y), 0);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (context != nullptr) {
        context->ProcessMouseMove(static_cast<int>(event.button.x),
                                  static_cast<int>(event.button.y), 0);
        context->ProcessMouseButtonDown(SDL_BUTTON_LEFT - 1, 0);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (context != nullptr) {
        context->ProcessMouseButtonUp(SDL_BUTTON_LEFT - 1, 0);
      }
      break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: {
      if (context == nullptr) {
        break;
      }
      int width = 0;
      int height = 0;
      SDL_GetWindowSizeInPixels(window, &width, &height);
      const int x = static_cast<int>(event.tfinger.x * static_cast<float>(width));
      const int y = static_cast<int>(event.tfinger.y * static_cast<float>(height));
      context->ProcessMouseMove(x, y, 0);
      if (event.type == SDL_EVENT_FINGER_DOWN) {
        context->ProcessMouseButtonDown(SDL_BUTTON_LEFT - 1, 0);
      } else if (event.type == SDL_EVENT_FINGER_UP) {
        context->ProcessMouseButtonUp(SDL_BUTTON_LEFT - 1, 0);
      }
      break;
    }
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
      apply_display_scale();
      apply_dimensions();
      break;
    }
    default:
      break;
    }
  }
};

View::View(Session &session, ViewOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->session = &session;
  impl_->options = std::move(options);
}

View::~View() {
  close();
}

bool View::open() {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    impl_->error = std::string{"SDL video is unavailable: "} + SDL_GetError();
    return false;
  }
  impl_->owns_sdl_video = true;

  const int width = std::max(320, impl_->options.width);
  const int height = std::max(240, impl_->options.height);
  if (impl_->options.offscreen) {
    // An offscreen target: same renderer interface, arbitrary size, and no
    // dependency on a display large enough to hold it.
    impl_->surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
    if (impl_->surface == nullptr) {
      impl_->error = std::string{"the offscreen setup surface could not be created: "} +
                     SDL_GetError();
      close();
      return false;
    }
    impl_->renderer = SDL_CreateSoftwareRenderer(impl_->surface);
    if (impl_->renderer == nullptr) {
      impl_->error = std::string{"the offscreen setup renderer could not be created: "} +
                     SDL_GetError();
      close();
      return false;
    }
  } else {
    impl_->window = SDL_CreateWindow(impl_->options.window_title.c_str(), width, height,
                                     impl_->options.resizable ? SDL_WINDOW_RESIZABLE : 0);
    if (impl_->window == nullptr) {
      impl_->error = std::string{"the setup window could not be created: "} + SDL_GetError();
      close();
      return false;
    }
    impl_->renderer = SDL_CreateRenderer(impl_->window, nullptr);
    if (impl_->renderer == nullptr) {
      impl_->renderer = SDL_CreateRenderer(impl_->window, "software");
    }
    if (impl_->renderer == nullptr) {
      impl_->error = std::string{"the setup surface could not be created: "} + SDL_GetError();
      close();
      return false;
    }
  }
  SDL_SetDefaultTextureScaleMode(impl_->renderer, SDL_SCALEMODE_NEAREST);

  impl_->render_interface = std::make_unique<SdlRmlRenderer>(impl_->renderer);
  impl_->system_interface = std::make_unique<SetupSystemInterface>();
  impl_->file_interface = std::make_unique<SetupFileInterface>();
  Rml::SetRenderInterface(impl_->render_interface.get());
  Rml::SetSystemInterface(impl_->system_interface.get());
  Rml::SetFileInterface(impl_->file_interface.get());
  if (!Rml::Initialise()) {
    impl_->error = "RmlUi could not be initialised";
    close();
    return false;
  }
  impl_->rml_initialised = true;

  const std::string font =
      impl_->options.font_path.empty() ? default_font_path() : impl_->options.font_path;
  if (font.empty()) {
    impl_->error = "no usable UI font was found; name one in ViewOptions::font_path";
    close();
    return false;
  }
  // The stylesheet names the family explicitly so the screen does not depend
  // on whatever family name a host's font file happens to carry.
  if (!Rml::LoadFontFace(font, "setup", Rml::Style::FontStyle::Normal,
                         Rml::Style::FontWeight::Normal, true)) {
    impl_->error = "the UI font could not be loaded: " + font;
    close();
    return false;
  }

  impl_->context = Rml::CreateContext("setup", Rml::Vector2i{width, height});
  if (impl_->context == nullptr) {
    impl_->error = "the setup context could not be created";
    close();
    return false;
  }
  impl_->apply_display_scale();
  impl_->apply_dimensions();
  impl_->document =
      impl_->context->LoadDocumentFromMemory(std::string(assets::kDocumentRml), "setup.rml");
  if (impl_->document == nullptr) {
    impl_->error = "the setup document could not be loaded";
    close();
    return false;
  }
  impl_->document->Show();

  impl_->title = impl_->document->GetElementById("title");
  impl_->message = impl_->document->GetElementById("message");
  impl_->hint = impl_->document->GetElementById("hint");
  impl_->footer = impl_->document->GetElementById("footer");
  impl_->rows = impl_->document->GetElementById("rows");
  impl_->status = impl_->document->GetElementById("status");
  impl_->start = impl_->document->GetElementById("start");
  impl_->again = impl_->document->GetElementById("again");
  impl_->chooser = impl_->document->GetElementById("chooser");
  impl_->progress = impl_->document->GetElementById("progress");
  impl_->progress_fill = impl_->document->GetElementById("progress-fill");

  impl_->browse_listener = std::make_unique<ButtonListener>(impl_->queue, RequestKind::Browse);
  impl_->start_listener = std::make_unique<ButtonListener>(impl_->queue, RequestKind::Start);
  impl_->cancel_listener = std::make_unique<ButtonListener>(impl_->queue, RequestKind::Cancel);
  impl_->again_listener = std::make_unique<ButtonListener>(impl_->queue, RequestKind::Browse);
  if (auto *browse = impl_->document->GetElementById("browse")) {
    browse->AddEventListener(Rml::EventId::Click, impl_->browse_listener.get());
  }
  if (impl_->start != nullptr) {
    impl_->start->AddEventListener(Rml::EventId::Click, impl_->start_listener.get());
  }
  if (impl_->again != nullptr) {
    impl_->again->AddEventListener(Rml::EventId::Click, impl_->again_listener.get());
    (void)impl_->cancel_listener;
  }

  impl_->apply_static_text();
  impl_->running = true;
  impl_->refresh();
  return true;
}

void View::close() {
  if (impl_->document != nullptr) {
    impl_->document->Close();
    impl_->document = nullptr;
  }
  if (impl_->context != nullptr) {
    Rml::RemoveContext("setup");
    impl_->context = nullptr;
  }
  if (impl_->rml_initialised) {
    Rml::Shutdown();
    impl_->rml_initialised = false;
  }
  impl_->render_interface.reset();
  impl_->system_interface.reset();
  impl_->file_interface.reset();
  if (impl_->renderer != nullptr) {
    SDL_DestroyRenderer(impl_->renderer);
    impl_->renderer = nullptr;
  }
  if (impl_->window != nullptr) {
    SDL_DestroyWindow(impl_->window);
    impl_->window = nullptr;
  }
  if (impl_->surface != nullptr) {
    SDL_DestroySurface(impl_->surface);
    impl_->surface = nullptr;
  }
  if (impl_->owns_sdl_video) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    impl_->owns_sdl_video = false;
  }
  impl_->running = false;
}

bool View::running() const noexcept {
  return impl_->running;
}

std::vector<Request> View::poll() {
  std::vector<Request> requests;
  requests.swap(impl_->queue);
  return requests;
}

void View::frame() {
  if (!impl_->running) {
    return;
  }
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    impl_->handle_event(event);
  }
  impl_->refresh();
  impl_->context->Update();
  SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
  SDL_RenderClear(impl_->renderer);
  impl_->context->Render();
  SDL_RenderPresent(impl_->renderer);
  impl_->session->validate_if_ready();
}

void View::finish() {
  impl_->running = false;
}

const std::string &View::last_error() const noexcept {
  return impl_->error;
}

bool View::capture(std::vector<std::uint32_t> &pixels, int &width, int &height) {
  pixels.clear();
  width = 0;
  height = 0;
  if (impl_->renderer == nullptr) {
    return false;
  }
  // Draw into the frame that is about to be read: the swap chain's contents
  // after a present are undefined, so a capture that reads the presented
  // buffer would report an empty frame for a screen that rendered fine.
  impl_->context->Update();
  SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
  SDL_RenderClear(impl_->renderer);
  impl_->context->Render();
  SDL_Surface *surface = SDL_RenderReadPixels(impl_->renderer, nullptr);
  if (surface == nullptr) {
    return false;
  }
  SDL_Surface *converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(surface);
  if (converted == nullptr) {
    return false;
  }
  width = converted->w;
  height = converted->h;
  pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  const auto *source = static_cast<const std::uint8_t *>(converted->pixels);
  for (int row = 0; row < height; ++row) {
    const auto *line = source + static_cast<std::size_t>(row) * converted->pitch;
    for (int column = 0; column < width; ++column) {
      std::uint32_t value = 0;
      std::memcpy(&value, line + static_cast<std::size_t>(column) * 4, sizeof value);
      pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(column)] = value;
    }
  }
  SDL_DestroySurface(converted);
  return true;
}

} // namespace setup_ui
