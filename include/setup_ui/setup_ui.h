// setup_ui.h — the browser-based first-run setup subsystem for native game
// ports.
//
// A consumer that needs player-supplied files (disc images, ROMs, disk sets)
// constructs one Server, hands it a validator, and lets the player drive a
// responsive web page from any browser on this device or a phone sharing the
// network. The server stages every uploaded byte under a caller-owned private
// directory; the consumer's validator owns file identity and the decision to
// proceed. Nothing here knows what a valid file is.
//
// Threading: construct, start, and poll on the application's main thread;
// upload workers live inside the host. Poll returns the lifecycle events the
// consumer acts on (validation requests, completion, browser-open requests).
// The consumer's validator runs on the poll() caller's thread.
//
// Security: loopback is the default. LocalNetwork mode is an explicit,
// user-visible choice; every route then requires the one-time pairing token
// the consumer displays, and sharing stops with stop() or destruction.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace setup_ui {

enum class Scope : std::uint8_t { Loopback, LocalNetwork };

struct FileSpec {
  std::string key;   // stable identifier used by the wire protocol ("disk1")
  std::string name;  // exact expected file name ("Disk.1")
  std::string label; // human-readable step label ("Game disc 1")
};

struct Config {
  std::string title;   // page <h1> and browser tab title
  std::string message; // one-line instruction under the title
  std::string hint;    // small print under the drop zone ("or one ZIP …")
  std::string footer;  // small print at the bottom (privacy, brand)
  std::vector<FileSpec> files;
  bool accepts_archive = false; // one bounded ZIP containing the set
};

struct UploadedFile {
  FileSpec spec;
  std::filesystem::path path; // staged file inside the caller's staging root
  std::uintmax_t size = 0;
  bool is_archive = false; // accepted via accepts_archive
};

// What poll() hands the consumer. The consumer runs its own validation inside
// the Validate callback of the options; events exist for progress display and
// for launching the browser at the right moment.
enum class EventKind : std::uint8_t {
  Started,       // the listener is up; port carries the bound port
  Uploaded,      // one file landed in staging; uploaded carries name/size
  BatchComplete, // every expected file has arrived; the validator runs on poll()
  Validated,     // the consumer's Validate callback returned success
  Failed,        // validation failed; failed_message carries the validator's text
};

struct Event {
  EventKind kind;
  std::string failed_message; // Failed only
  std::string uploaded_name;  // Uploaded only
  std::uintmax_t uploaded_size = 0;
  std::uint16_t port = 0; // Started only
};

// The staged set is complete when Validate receives files.size() ==
// options.config.files.size() (or the single archive when accepts_archive is
// set). Validate returns an empty error string on success; a non-empty string
// is shown verbatim in the browser and the consumer must clean its own state.
using Validate = std::function<std::string(const std::vector<UploadedFile> &)>;

struct ServerOptions {
  Config config;
  // Private parent directory for staged uploads. Created when missing; the
  // host stages under one private child per attempt and owns its cleanup.
  std::filesystem::path staging_root;
  Scope scope = Scope::Loopback;
  std::size_t max_file_bytes = 64ULL * 1024ULL * 1024ULL;
  int timeout_seconds = 600; // idle listener lifetime; consumer may stop sooner
};

// One setup session. Copy/move are deleted: the server owns live sockets.
class Server {
public:
  explicit Server(ServerOptions options, Validate validate);
  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  // Binds and starts serving. Returns false (with last_error()) when the
  // listener cannot start. Idempotent while running.
  bool start();
  void stop();

  // Non-blocking. Delivers lifecycle events and invokes the validator on the
  // calling thread when an upload batch completes.
  void poll(std::vector<Event> &out);

  // URL the browser should open (loopback, or the LAN address with token).
  std::string url() const;
  // Token appended to LAN URLs; empty in loopback mode.
  std::string token() const;
  std::uint16_t port() const;
  bool running() const noexcept;
  const std::string &last_error() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace setup_ui
