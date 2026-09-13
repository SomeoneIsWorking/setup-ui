#include "setup_ui/setup_ui.h"
#include "setup_ui/setup_ui_c.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Drives the shipping server over loopback with raw HTTP requests, the same
// transport path a browser uses. Asserts route behavior, staging paths,
// validation threading, and failure wording.
const char *expected_names[] = {"Alpha.bin", "Beta.bin", "Gamma.bin"};

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                    \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

bool send_all(int client, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto sent = ::send(client, bytes.data(), static_cast<int>(bytes.size()), 0);
    if (sent <= 0) {
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

std::string http_request(std::uint16_t port, const std::string &target,
                         const std::string &method = "GET", const std::string &body = {}) {
  int client = ::socket(AF_INET, SOCK_STREAM, 0);
  CHECK(client >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
    ::close(client);
    return {};
  }
  std::string wire = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  CHECK(send_all(client, wire));
  ::shutdown(client, SHUT_WR);
  std::string response;
  char block[4096];
  for (int count; (count = static_cast<int>(::recv(client, block, sizeof block, 0))) > 0;) {
    response.append(block, static_cast<std::size_t>(count));
  }
  ::close(client);
  return response;
}

std::string_view body_of(std::string_view response) {
  const std::size_t split = response.find("\r\n\r\n");
  return split == std::string_view::npos ? std::string_view{} : response.substr(split + 4);
}

setup_ui::Config test_config() {
  setup_ui::Config config;
  config.title = "Test setup";
  config.message = "Provide the files.";
  config.hint = "one archive is fine";
  config.footer = "no files leave this network";
  config.files = {
      {"a", "Alpha.bin", "Alpha"}, {"b", "Beta.bin", "Beta"}, {"c", "Gamma.bin", "Gamma"}};
  config.accepts_archive = true;
  return config;
}

setup_ui::ServerOptions test_options(const std::filesystem::path &staging_root) {
  setup_ui::ServerOptions options;
  options.config = test_config();
  options.staging_root = staging_root;
  options.max_file_bytes = 1 << 20;
  return options;
}

void drain(setup_ui::Server &server, std::vector<setup_ui::Event> &events) {
  server.poll(events);
}

} // namespace

// ── C ABI wrapper: the same flow through setup_ui_c.h ──────────────────────
namespace {

struct CValidateState {
  int calls = 0;
};

// The validator signature mirrors the C ABI; the path and name arrays are
// positionally bound and both index by file order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int c_validate(void *raw, const char *const *paths, const char *const *names, size_t count,
               char *error, size_t error_capacity) {
  auto *state = static_cast<CValidateState *>(raw);
  ++state->calls;
  if (count != 3) {
    std::snprintf(error, error_capacity, "expected three files, got %zu", count);
    return 1;
  }
  for (size_t index = 0; index < count; ++index) {
    std::FILE *file = std::fopen(paths[index], "rb");
    if (!file || std::strcmp(names[index], expected_names[index]) != 0) {
      if (file) {
        std::fclose(file);
      }
      std::snprintf(error, error_capacity, "file %zu was not accepted", index);
      return 1;
    }
    std::fclose(file);
  }
  return 0;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void test_c_abi(const std::filesystem::path &staging_root) {
  setup_ui_file files[] = {{"Alpha.bin", "Alpha"}, {"Beta.bin", "Beta"}, {"Gamma.bin", "Gamma"}};
  CValidateState state;
  setup_ui_config config{};
  config.title = "C setup";
  config.message = "bring the disks";
  config.files = files;
  config.file_count = 3;
  config.validate = c_validate;
  config.userdata = &state;
  setup_ui_server *server = setup_ui_server_new(&config, staging_root.string().c_str(), 0, 0);
  CHECK(server != nullptr);
  CHECK(setup_ui_server_start(server) == 1);
  const std::uint16_t port = setup_ui_server_port(server);
  CHECK(port != 0);
  char url[256] = {0};
  CHECK(setup_ui_server_url(server, url, sizeof url) == 1);
  CHECK(std::strncmp(url, "http://127.0.0.1:", 17) == 0);

  for (const char *name : {"Alpha.bin", "Beta.bin", "Gamma.bin"}) {
    std::string target = std::string("/api/upload?name=") + name;
    const std::string payload(64, static_cast<char>('0' + name[0]));
    const std::string response = http_request(port, target, "POST", payload);
    CHECK(response.find("200") != std::string::npos);
  }

  setup_ui_event events[8] = {};
  const std::size_t delivered = setup_ui_server_poll(server, events, 8);
  CHECK(delivered >= 4); // uploads + BatchComplete + a validation verdict
  bool validated = false;
  for (std::size_t index = 0; index < delivered; ++index) {
    if (events[index].kind == SETUP_UI_EVENT_VALIDATED) {
      validated = true;
    }
  }
  CHECK(validated);
  CHECK(state.calls == 1);
  // The C ABI owns its event numbering: the batch marker must never be
  // reported as a validation verdict.
  bool saw_batch_marker = false;
  for (std::size_t index = 0; index < delivered; ++index) {
    if (events[index].kind == SETUP_UI_EVENT_BATCH_COMPLETE) {
      saw_batch_marker = true;
    }
    CHECK(events[index].kind != SETUP_UI_EVENT_VALIDATED ||
          std::strcmp(events[index].message, "") == 0);
  }
  CHECK(saw_batch_marker);

  setup_ui_server_stop(server);
  setup_ui_server_free(server);
}

} // namespace

int main() {
  const auto staging_root = std::filesystem::temp_directory_path() /
                            ("setup-ui-test-" + std::to_string(static_cast<long>(::getpid())));
  std::filesystem::create_directories(staging_root);

  std::atomic<int> validation_calls{0};
  std::vector<std::string> validated_names;

  setup_ui::Server server(test_options(staging_root),
                          [&](const std::vector<setup_ui::UploadedFile> &files) {
                            validation_calls.fetch_add(1);
                            validated_names.clear();
                            for (const auto &file : files) {
                              validated_names.push_back(file.spec.name);
                            }
                            return std::string{};
                          });
  CHECK(server.start());
  CHECK(server.running());
  CHECK(server.port() != 0);
  CHECK(server.url().find("http://127.0.0.1:") == 0);
  CHECK(server.token().empty());

  // The page and controller are served with the right types.
  const auto page = http_request(server.port(), "/");
  CHECK(page.find("200 OK") != std::string::npos);
  CHECK(page.find("text/html") != std::string::npos);
  CHECK(body_of(page).find("id=\"status\"") != std::string::npos);
  const auto script = http_request(server.port(), "/setup.js");
  CHECK(script.find("text/javascript") != std::string::npos);

  // Config carries the consumer wording verbatim.
  const auto config_json = http_request(server.port(), "/api/config");
  CHECK(body_of(config_json).find("\"title\":\"Test setup\"") != std::string::npos);
  CHECK(body_of(config_json).find("\"accepts_archive\":true") != std::string::npos);

  // Unknown names are refused before anything is staged.
  const auto bad = http_request(server.port(), "/api/upload?name=Unknown.bin", "POST", "x");
  CHECK(bad.find("400") != std::string::npos);

  // Three uploads complete a batch; validation runs on the poll() thread.
  const auto one =
      http_request(server.port(), "/api/upload?name=Alpha.bin", "POST", std::string(2048, 'a'));
  CHECK(one.find("200") != std::string::npos);
  std::vector<setup_ui::Event> events;
  drain(server, events);
  bool saw_uploaded = false;
  for (const auto &event : events) {
    if (event.kind == setup_ui::EventKind::Uploaded && event.uploaded_name == "Alpha.bin") {
      saw_uploaded = true;
    }
  }
  CHECK(saw_uploaded);
  CHECK(validation_calls.load() == 0);

  const auto two =
      http_request(server.port(), "/api/upload?name=Beta.bin", "POST", std::string(1024, 'b'));
  CHECK(two.find("200") != std::string::npos);
  const auto three =
      http_request(server.port(), "/api/upload?name=Gamma.bin", "POST", std::string(512, 'c'));
  CHECK(three.find("200") != std::string::npos);

  drain(server, events);
  bool saw_batch = false;
  bool saw_validated = false;
  for (const auto &event : events) {
    if (event.kind == setup_ui::EventKind::BatchComplete) {
      saw_batch = true;
    }
    if (event.kind == setup_ui::EventKind::Validated) {
      saw_validated = true;
    }
  }
  CHECK(saw_batch);
  CHECK(saw_validated);
  CHECK(validation_calls.load() == 1);
  CHECK(validated_names.size() == 3);
  CHECK(validated_names[0] == "Alpha.bin");
  CHECK(validated_names[1] == "Beta.bin");
  CHECK(validated_names[2] == "Gamma.bin");

  // The staged file exists under the caller-owned staging root and carries
  // the uploaded bytes.
  std::ifstream staged(staging_root / "Alpha.bin" /* resolved below */, std::ios::binary);
  // staging root contains one setup-* child; find it:
  std::filesystem::path session;
  for (const auto &entry : std::filesystem::directory_iterator(staging_root)) {
    session = entry.path();
  }
  CHECK(!session.empty());
  const auto uploaded = (session / "Alpha.bin");
  CHECK(std::filesystem::file_size(uploaded) == 2048);

  // A start after validation succeeds.
  const auto start_ok = http_request(server.port(), "/api/start", "POST");
  CHECK(body_of(start_ok).find("\"ok\":true") != std::string::npos);

  // Rejection wording flows back to the browser.
  setup_ui::Server failing(test_options(staging_root),
                           [&](const std::vector<setup_ui::UploadedFile> &) {
                             return std::string{"wrong disc identity"};
                           });
  CHECK(failing.start());
  const auto up1 = http_request(failing.port(), "/api/upload?name=Alpha.bin", "POST", "aaa");
  const auto up2 = http_request(failing.port(), "/api/upload?name=Beta.bin", "POST", "bbb");
  const auto up3 = http_request(failing.port(), "/api/upload?name=Gamma.bin", "POST", "ccc");
  CHECK(up1.find("200") != std::string::npos && up2.find("200") != std::string::npos &&
        up3.find("200") != std::string::npos);
  drain(failing, events);
  bool saw_failed = false;
  for (const auto &event : events) {
    if (event.kind == setup_ui::EventKind::Failed &&
        event.failed_message == "wrong disc identity") {
      saw_failed = true;
    }
  }
  CHECK(saw_failed);
  const auto start_bad = http_request(failing.port(), "/api/start", "POST");
  CHECK(start_bad.find("400") != std::string::npos);
  CHECK(body_of(start_bad).find("wrong disc identity") != std::string::npos);

  test_c_abi(staging_root);

  server.stop();
  CHECK(!server.running());

  std::error_code cleanup;
  std::filesystem::remove_all(staging_root, cleanup);
  if (g_failures == 0) {
    std::fputs("all setup-ui tests passed\n", stdout);
    return 0;
  }
  std::fprintf(stderr, "%d failure(s)\n", g_failures);
  return 1;
}
