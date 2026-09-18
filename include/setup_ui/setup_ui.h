// setup_ui.h — an in-app first-run setup screen for native game ports.
//
// A port that needs player-supplied files (disc images, ROMs, disk sets)
// shows this screen inside its own window instead of opening a browser or a
// system message box. The screen owns presentation and staged-file state; the
// consumer owns the platform's file picker, file identity, persistence, and
// the decision to start.
//
// Layout: Session holds what the title requires and what has been provided.
// View renders a Session through RmlUi (HTML/CSS-like markup) in an SDL3
// window and reports the player's requests back as events. A consumer loop
// looks like:
//
//   setup_ui::Session session(config, validator);
//   setup_ui::View view(session, view_options);
//   if (!view.open()) { ... }
//   while (view.running()) {
//     for (const auto &request : view.poll()) {
//       if (request.kind == setup_ui::RequestKind::Browse)
//         platform_picker([&](auto paths) { session.add_selected(paths); });
//     }
//     view.frame();
//   }
//
// Everything here runs on the thread that owns the SDL window.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace setup_ui {

// One required input, in the order the player should provide them.
struct FileSpec {
  std::string key; // stable identifier used by the consumer ("disk1")
  // Exact expected file name ("Disk.1"). An empty name makes this the port's
  // single requirement, satisfied by whichever one thing the player selects —
  // a ROM, a disc image, an original installer, an archive, or an existing
  // install directory — with the consumer's Validator deciding whether it is
  // the right one. A config that uses an empty name has exactly one FileSpec,
  // because there is nothing to tell two such selections apart by.
  std::string name;
  std::string label; // human-readable row label ("Game disc 1")
};

// Where a chosen selection is handed to the Validator.
enum class Placement : std::uint8_t {
  // Copy the bytes into this session's private staging directory first, so
  // the port holds a private, complete set however the player's copy was
  // reached. Regular files only, bounded by SessionOptions::max_file_bytes.
  Stage,
  // Hand the Validator the path the player chose, unchanged. A port whose
  // selection is a directory, or a file whose siblings are part of the
  // install, needs those surroundings: copying the chosen file on its own
  // would lose them. Nothing is copied and no byte bound is applied, so the
  // consumer owns every bound on what it reads from that location.
  Adopt,
};

struct Config {
  std::string title;   // screen heading and window title
  std::string message; // one line under the heading
  std::string hint;    // small print under the choose button
  std::string footer;  // small print at the bottom
  std::vector<FileSpec> files;
  bool accepts_archive = false; // one bounded ZIP standing in for a named set
  Placement placement = Placement::Stage;
  // The button that asks the consumer's picker to open. A port needing one
  // selection says so ("Choose your game files"); the plural default suits a
  // named set.
  std::string browse_label = "Choose files";
  // Shown when the validator accepts the set, just before the port starts.
  std::string accepted_message = "Files accepted.";
};

// One required row as the screen shows it.
struct Entry {
  FileSpec spec;
  bool provided = false;
  std::uintmax_t size = 0;
};

enum class Status : std::uint8_t {
  Collecting, // waiting for the player to provide files
  Ready,      // every requirement is present; worth validating
  Rejected,   // the validator refused the set; message() says why
  Accepted,   // the validator accepted the set; the consumer may start
};

// The consumer's policy over a complete selected set. Called on the same
// thread that drives the View. Returns an empty string to accept, or a
// human-readable reason to reject. Each `path` is readable: under
// Placement::Stage it is this session's private copy, and under
// Placement::Adopt it is the location the player chose, which may be a
// directory.
struct StagedFile {
  FileSpec spec;
  std::filesystem::path path;
  std::uintmax_t size = 0; // bytes; zero for an adopted directory
  bool is_archive = false;
};
using Validator = std::function<std::string(const std::vector<StagedFile> &)>;

// Where staged uploads live. The consumer supplies a private directory; the
// session creates and cleans one child per attempt. Both are unused under
// Placement::Adopt, which copies nothing.
struct SessionOptions {
  std::filesystem::path staging_root;
  std::size_t max_file_bytes = 64ULL * 1024ULL * 1024ULL;
};

class Session {
public:
  Session(Config config, SessionOptions options, Validator validator);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  // Adds every selected file that matches a requirement by exact name, plus
  // one archive when the config allows it; a config whose single FileSpec has
  // an empty name instead takes whatever one path was selected. Copies the
  // bytes into staging unless the config adopts the chosen location.
  // Returns the count added; `error` names the first rejection.
  std::size_t add_selected(const std::vector<std::filesystem::path> &paths, std::string &error);

  // Removes staged state so the player can choose again.
  void reset();

  // Runs the validator when every requirement is present (idempotent per set).
  void validate_if_ready();

  const Config &config() const;
  const std::vector<Entry> &entries() const;
  Status status() const;
  const std::string &message() const;      // rejection or completion text
  double progress() const;                 // 0..1 bytes staged for the current copy
  void set_progress(double fraction);      // consumer copy progress (0..1)
  const std::string &staging_directory() const;

  // Removes the staging directories an earlier session left behind. A session
  // removes its own directory when it is destroyed, but a process that is
  // killed with the screen open (Android force-stop, a crash) never runs that
  // destructor, and a half-copied directory is never reusable.
  static void discard_stale_staging(const std::filesystem::path &staging_root);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class RequestKind : std::uint8_t {
  Browse, // the player asked to choose files
  Start,  // the player confirmed a validated set
  Cancel, // the player dismissed the screen
};

struct Request {
  RequestKind kind;
};

struct ViewOptions {
  std::string window_title = "Setup";
  int width = 960;
  int height = 540;
  bool resizable = true;
  // Font used by the screen. Empty selects the first usable system font; a
  // consumer may name an exact file it ships.
  std::string font_path;
  // dp ratio. Zero follows the host display scale (the normal case); a value
  // pins it, which is how a phone-sized screen is reproduced on a desktop for
  // verification.
  float density_ratio = 0.0F;
  // Render to an offscreen surface instead of a window. Used for exact-size
  // verification (a phone viewport larger than the build host's display) and
  // for hosts that present elsewhere.
  bool offscreen = false;
};

// The setup screen: an SDL3 window rendering a Session through RmlUi.
class View {
public:
  View(Session &session, ViewOptions options);
  ~View();
  View(const View &) = delete;
  View &operator=(const View &) = delete;

  // Creates the window, renderer, and document. Returns false with
  // last_error() set when the platform cannot present the screen.
  bool open();
  void close();
  bool running() const noexcept;

  // Player requests since the last call.
  std::vector<Request> poll();
  // Renders one frame and processes input. Cheap enough to call every frame.
  void frame();

  // Ends the loop after the consumer finished starting from an accepted set.
  void finish();

  const std::string &last_error() const noexcept;

  // Test seam: renders the current document into an ARGB8888 buffer of the
  // view's size without a window. Requires SDL video to be available.
  bool capture(std::vector<std::uint32_t> &pixels, int &width, int &height);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace setup_ui
