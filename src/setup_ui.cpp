#include "setup_ui/setup_ui.h"

#include "assets.h"
#include "setup_ui/setup_ui_c.h"

#include <lucent/http.h>
#include <lucent/text.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

// The host owns serving, transfer, staging, and lifecycle over Lucent's
// bounded HTTP server. It never judges file content: the consumer's Validate
// callback runs when an upload batch completes, on the poll() caller's
// thread, and its verdict is both shown in the browser and reported through
// poll() events. Staging is one private child directory per attempt, cleaned
// on rejection or destruction.
namespace setup_ui {

namespace {

std::string content_type_for(std::string_view path) {
  if (path.ends_with(".html")) {
    return "text/html; charset=utf-8";
  }
  if (path.ends_with(".js")) {
    return "text/javascript; charset=utf-8";
  }
  return "application/octet-stream";
}

// decode a percent-encoded component (RFC 3986); '+' is a space in queries
bool url_decode(std::string_view value, std::string &out) {
  out.clear();
  out.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char c = value[index];
    if (c == '%') {
      if (index + 2 >= value.size()) {
        return false;
      }
      const auto decode_pair = [](char high, char low, char &out) {
        const auto digit = [](char d) -> int {
          if (d >= '0' && d <= '9') {
            return d - '0';
          }
          if (d >= 'a' && d <= 'f') {
            return d - 'a' + 10;
          }
          if (d >= 'A' && d <= 'F') {
            return d - 'A' + 10;
          }
          return -1;
        };
        const int hi = digit(high);
        const int lo = digit(low);
        if (hi < 0 || lo < 0) {
          return false;
        }
        out = static_cast<char>((hi << 4) | lo);
        return true;
      };
      char decoded = 0;
      if (!decode_pair(value[index + 1], value[index + 2], decoded)) {
        return false;
      }
      out.push_back(decoded);
      index += 2;
      continue;
    }
    if (c == '+') {
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
  }
  return true;
}

bool safe_staging_name(std::string_view name) {
  if (name.empty() || name.size() > 128) {
    return false;
  }
  for (std::size_t index = 0; index < name.size(); ++index) {
    const char c = name[index];
    if (c == '/' || c == '\\' || c == '\0' || c == ':') {
      return false;
    }
    if (c == '.' && (index == 0 || name[index - 1] == '/')) {
      return false; // refuse leading dots to keep the staging tree clean
    }
  }
  return true;
}

std::string random_token() {
  static constexpr char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
  std::random_device device;
  std::seed_seq sequence{device(), device(), device(), device(), device()};
  std::mt19937 engine(sequence);
  std::uniform_int_distribution<std::size_t> pick(0, sizeof kAlphabet - 2);
  std::string token;
  for (int index = 0; index < 6; ++index) {
    token.push_back(kAlphabet[pick(engine)]);
  }
  return token;
}

// Best-effort primary site-local IPv4 address of this host, used only to
// compose the LAN sharing URL. Loopback scope never calls it.
std::string local_address() {
#if defined(_WIN32)
  ULONG size = 15 * 1024;
  std::vector<unsigned char> storage(size);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const ULONG status =
        GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST, nullptr,
                             reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
      storage.resize(size);
      continue;
    }
    if (status != NO_ERROR) {
      return {};
    }
    for (const auto *adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(storage.data());
         adapter != nullptr; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
        continue;
      }
      for (const auto *entry = adapter->FirstUnicastAddress; entry != nullptr;
           entry = entry->Next) {
        if (entry->Address.lpSockaddr == nullptr ||
            entry->Address.lpSockaddr->sa_family != AF_INET) {
          continue;
        }
        char text[INET_ADDRSTRLEN] = {0};
        const auto *address = reinterpret_cast<const sockaddr_in *>(entry->Address.lpSockaddr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof text) == nullptr) {
          continue;
        }
        return text;
      }
    }
    return {};
  }
  return {};
#else
  ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return {};
  }
  std::string result;
  for (const ifaddrs *interface = interfaces; interface != nullptr;
       interface = interface->ifa_next) {
    if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    char text[INET_ADDRSTRLEN] = {0};
    const auto *address = reinterpret_cast<const sockaddr_in *>(interface->ifa_addr);
    if (ntohl(address->sin_addr.s_addr) == INADDR_LOOPBACK) {
      continue;
    }
    if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof text) != nullptr) {
      result = text;
      break;
    }
  }
  freeifaddrs(interfaces);
  return result;
#endif
}

} // namespace

struct Server::Impl {
  ServerOptions options;
  Validate validate;
  std::unique_ptr<lucent::http::Server> server;
  std::string error;
  std::string lan_token;
  std::filesystem::path staging; // private child of staging_root for this session

  // Locked by poll()/worker handoff.
  std::mutex mutex;
  std::vector<UploadedFile> pending;
  std::vector<Event> queue;
  std::string session_error; // validator message mirrored back to the browser
  std::atomic<bool> validated_once{false};
  bool batch_pending = false; // set by the worker; poll() clears after validation

  bool create_staging() {
    std::error_code status;
    std::filesystem::create_directories(options.staging_root, status);
    if (status) {
      error = "cannot create the staging directory: " + status.message();
      return false;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
      std::string leaf = "setup-" + random_token();
      std::filesystem::path candidate = options.staging_root / leaf;
      if (std::filesystem::create_directory(candidate, status) && !status) {
        staging = candidate;
        return true;
      }
    }
    error = "cannot create a private staging directory";
    return false;
  }

  void push(Event event) {
    std::lock_guard lock(mutex);
    queue.push_back(std::move(event));
  }

  // Runs on the poll() caller's thread when a batch-complete marker is out.
  void run_validation() {
    std::vector<UploadedFile> batch;
    {
      std::lock_guard lock(mutex);
      batch = pending;
    }
    if (batch.empty()) {
      return;
    }
    std::string verdict = validate(batch);
    {
      std::lock_guard lock(mutex);
      if (verdict.empty()) {
        queue.push_back(Event{EventKind::Validated, {}, {}, 0});
        session_error.clear();
        validated_once.store(true);
      } else {
        queue.push_back(Event{EventKind::Failed, verdict, {}, 0});
        session_error = verdict;
        // Rejected bytes stay staged until the consumer validates a
        // replacement: uploads append, nothing is deleted behind its back.
      }
    }
  }

  lucent::http::Response validation_failure(const std::string &message) {
    std::ostringstream json;
    json << "{\"ok\":false,\"error\":";
    json << '"' << message << '"';
    json << '}';
    return lucent::http::Response::json(400, "Bad Request", json.str());
  }

  lucent::http::Response handle(const lucent::http::Request &request) {
    const auto token_ok = [&]() {
      if (options.scope != Scope::LocalNetwork) {
        return true;
      }
      // Every LAN route requires the pairing token in the path.
      const std::string_view path = request.path();
      std::string decoded;
      if (path.size() < lan_token.size() + 2 || path.front() != '/' ||
          !url_decode(path.substr(1, lan_token.size()), decoded) || decoded != lan_token ||
          path[lan_token.size() + 1] != '/') {
        return false;
      }
      return true;
    };
    if (!token_ok()) {
      return lucent::http::Response::text(404, "Not Found", "not found\n");
    }

    std::string_view path = request.path();
    // Strip the LAN token prefix so route matching stays uniform.
    if (options.scope == Scope::LocalNetwork && path.size() > lan_token.size() + 1) {
      path.remove_prefix(lan_token.size() + 1);
    }
    if (path == "/") {
      lucent::http::Response response = lucent::http::Response::text(
          200, "OK",
          std::string(reinterpret_cast<const char *>(assets::kIndexBytes), assets::kIndexSize));
      response.content_type = content_type_for("index.html");
      return response;
    }
    if (path == "/setup.js") {
      lucent::http::Response response = lucent::http::Response::text(
          200, "OK",
          std::string(reinterpret_cast<const char *>(assets::kScriptBytes), assets::kScriptSize));
      response.content_type = content_type_for("setup.js");
      return response;
    }
    if (path == "/api/config") {
      const Config &config = options.config;
      std::ostringstream json;
      json << "{\"title\":";
      json << '"' << config.title << '"';
      json << ",\"message\":";
      json << '"' << config.message << '"';
      json << ",\"hint\":";
      json << '"' << config.hint << '"';
      json << ",\"footer\":";
      json << '"' << config.footer << '"';
      json << ",\"accepts_archive\":" << (config.accepts_archive ? "true" : "false");
      json << ",\"files\":[";
      for (std::size_t index = 0; index < config.files.size(); ++index) {
        const FileSpec &spec = config.files[index];
        if (index != 0) {
          json << ',';
        }
        json << "{\"key\":";
        json << '"' << spec.key << '"';
        json << ",\"name\":";
        json << '"' << spec.name << '"';
        json << ",\"label\":";
        json << '"' << spec.label << '"';
        json << '}';
      }
      json << "]}";
      return lucent::http::Response::json(200, "OK", json.str());
    }
    if (path == "/api/upload" && request.method == "POST") {
      return handle_upload(request);
    }
    if (path == "/api/start" && request.method == "POST") {
      std::string mirror;
      bool have_batch = false;
      {
        std::lock_guard lock(mutex);
        mirror = session_error;
        have_batch = validated_once.load();
      }
      if (!mirror.empty()) {
        return validation_failure(mirror);
      }
      if (!have_batch) {
        // The batch is complete but the consumer has not polled yet; its
        // validator runs on the poll() caller's thread, so report acceptance
        // as pending instead of running title policy here.
        return lucent::http::Response::json(202, "Accepted", "{\"ok\":true,\"pending\":true}");
      }
      return lucent::http::Response::json(200, "OK", "{\"ok\":true}");
    }
    return lucent::http::Response::text(404, "Not Found", "not found\n");
  }

  // Reads the staged upload from Lucent's body buffer and writes it to the
  // staging directory. One file per request; the browser uploads each file
  // separately and the wire keeps names in headers.
  lucent::http::Response handle_upload(const lucent::http::Request &request) {
    std::string name;
    if (!request_query_value(request, "name", name) || !safe_staging_name(name)) {
      return lucent::http::Response::text(400, "Bad Request", "bad upload name\n");
    }
    // Find the spec: by exact expected name or, for archives, any .zip name.
    const FileSpec *spec = nullptr;
    bool is_archive = false;
    for (const FileSpec &candidate : options.config.files) {
      if (candidate.name == name) {
        spec = &candidate;
        break;
      }
    }
    if (!spec && options.config.accepts_archive && name.size() > 4 &&
        lucent::text::ascii_iequals(name.substr(name.size() - 4), ".zip")) {
      is_archive = true;
    }
    if (!spec && !is_archive) {
      return lucent::http::Response::text(400, "Bad Request",
                                          "this file is not part of the required set\n");
    }
    if (request.body.empty()) {
      return lucent::http::Response::text(400, "Bad Request", "empty upload\n");
    }
    if (request.body.size() > options.max_file_bytes) {
      return lucent::http::Response::text(413, "Content Too Large",
                                          "the file exceeds the size limit\n");
    }
    std::filesystem::path target = staging / name;
    {
      std::ofstream file(target, std::ios::binary | std::ios::trunc);
      if (!file) {
        return lucent::http::Response::text(500, "Internal Server Error",
                                            "cannot stage the upload\n");
      }
      file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
      if (!file) {
        return lucent::http::Response::text(500, "Internal Server Error",
                                            "cannot write the upload\n");
      }
    }
    {
      std::lock_guard lock(mutex);
      if (!is_archive) {
        std::erase_if(pending, [&](const UploadedFile &item) {
          return item.spec.name == name;
        });
        pending.push_back(UploadedFile{*spec, target, request.body.size()});
      } else {
        std::erase_if(pending, [](const UploadedFile &item) {
          return item.is_archive;
        });
        FileSpec archive_spec{"archive", name, name};
        pending.push_back(UploadedFile{archive_spec, target, request.body.size()});
        pending.back().is_archive = true;
      }
    }
    push(Event{EventKind::Uploaded, {}, name, request.body.size()});
    // Batch completion is only a marker here: the consumer's validator runs
    // on the poll() caller's thread, never on an HTTP worker.
    const std::size_t expected = is_archive ? 1 : options.config.files.size();
    std::size_t have = 0;
    {
      std::lock_guard lock(mutex);
      have = pending.size();
    }
    if (have >= expected) {
      std::lock_guard lock(mutex);
      if (!batch_pending) {
        batch_pending = true;
        queue.push_back(Event{EventKind::BatchComplete, {}, {}, 0});
      }
    }
    return lucent::http::Response::json(200, "OK", "{\"ok\":true}");
  }

  static bool request_query_value(const lucent::http::Request &request, std::string_view key,
                                  std::string &out) {
    for (const std::string_view token : split_query(request.query())) {
      const std::size_t equals = token.find('=');
      if (equals == std::string_view::npos) {
        continue;
      }
      std::string decoded_key;
      if (!url_decode(token.substr(0, equals), decoded_key) || decoded_key != key) {
        continue;
      }
      return url_decode(token.substr(equals + 1), out);
    }
    return false;
  }

  static std::vector<std::string_view> split_query(std::string_view query) {
    std::vector<std::string_view> tokens;
    while (!query.empty()) {
      const std::size_t ampersand = query.find('&');
      tokens.push_back(query.substr(0, ampersand));
      query.remove_prefix(std::min(
          ampersand == std::string_view::npos ? query.size() : ampersand + 1, query.size()));
    }
    return tokens;
  }
};

Server::Server(ServerOptions options, Validate validate) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
  impl_->validate = std::move(validate);
  if (impl_->options.scope == Scope::LocalNetwork) {
    impl_->lan_token = random_token();
  }
}

Server::~Server() {
  stop();
  if (impl_ && !impl_->staging.empty()) {
    std::error_code status;
    std::filesystem::remove_all(impl_->staging, status);
  }
}

bool Server::start() {
  if (impl_->server && impl_->server->running()) {
    return true;
  }
  if (!impl_->create_staging()) {
    return false;
  }
  lucent::http::ServerOptions http_options;
  http_options.max_body_bytes = impl_->options.max_file_bytes;
  http_options.listen_scope = impl_->options.scope == Scope::Loopback
                                  ? lucent::http::ListenScope::Loopback
                                  : lucent::http::ListenScope::LocalNetwork;
  Impl *host = impl_.get();
  impl_->server = std::make_unique<lucent::http::Server>(
      http_options, [host](const lucent::http::Request &request) {
        return host->handle(request);
      });
  if (!impl_->server->start()) {
    impl_->error = "cannot start the setup listener";
    return false;
  }
  impl_->push(Event{EventKind::Started, {}, {}, impl_->server->port()});
  return true;
}

void Server::stop() {
  if (impl_->server) {
    impl_->server->stop();
  }
}

void Server::poll(std::vector<Event> &out) {
  out.clear();
  bool validate_now = false;
  {
    std::lock_guard lock(impl_->mutex);
    out.swap(impl_->queue);
    for (const Event &event : out) {
      if (event.kind == EventKind::BatchComplete) {
        validate_now = true;
      }
    }
    if (validate_now) {
      impl_->batch_pending = false;
    }
  }
  if (!validate_now) {
    return;
  }
  // The validator runs on this thread, outside the queue mutex. Its verdict
  // joins the same delivered batch so the consumer sees exactly one complete
  // cycle per poll: uploads, the marker, then Validated or Failed.
  impl_->run_validation();
  std::lock_guard lock(impl_->mutex);
  for (auto &event : impl_->queue) {
    out.push_back(std::move(event));
  }
  impl_->queue.clear();
}

std::string Server::url() const {
  if (!impl_->server || !impl_->server->running()) {
    return {};
  }
  std::ostringstream url;
  url << "http://";
  if (impl_->options.scope == Scope::LocalNetwork) {
    url << local_address() << ':' << impl_->server->port() << '/' << impl_->lan_token << '/';
  } else {
    url << "127.0.0.1:" << impl_->server->port() << '/';
  }
  return url.str();
}

std::string Server::token() const {
  return impl_->lan_token;
}

std::uint16_t Server::port() const {
  return impl_->server ? impl_->server->port() : 0;
}

bool Server::running() const noexcept {
  return impl_->server && impl_->server->running();
}

const std::string &Server::last_error() const noexcept {
  return impl_->error;
}

} // namespace setup_ui
