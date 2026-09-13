#include "setup_ui/setup_ui_c.h"

#include "setup_ui/setup_ui.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The C ABI wraps one C++ Server. Validation threads through a fixed-shape
// callback so C consumers keep their own storage and policy; the staging
// paths handed to the validator remain valid only during that call.
namespace {

struct CState {
  setup_ui_validate_fn fn = nullptr;
  void *userdata = nullptr;
};

std::string validate_c(const std::vector<setup_ui::UploadedFile> &files, void *raw) {
  const auto *state = static_cast<CState *>(raw);
  if (!state || !state->fn) {
    return "setup UI is misconfigured: no validator";
  }
  std::vector<const char *> paths;
  std::vector<const char *> names;
  paths.reserve(files.size());
  names.reserve(files.size());
  for (const auto &file : files) {
    paths.push_back(file.path.string().c_str());
    names.push_back(file.spec.name.c_str());
  }
  char error[256] = {0};
  if (state->fn(state->userdata, paths.data(), names.data(), files.size(), error, sizeof error) ==
      0) {
    return {};
  }
  return error[0] ? std::string(error) : std::string("the files were not accepted");
}

setup_ui::Config convert_config(const setup_ui_config *config) {
  setup_ui::Config converted;
  if (config == nullptr) {
    return converted;
  }
  converted.title = config->title ? config->title : "Setup";
  converted.message = config->message ? config->message : "";
  converted.hint = config->hint ? config->hint : "";
  converted.footer = config->footer ? config->footer : "";
  converted.accepts_archive = config->accepts_archive != 0;
  converted.files.reserve(config->file_count);
  for (std::size_t index = 0; index < config->file_count; ++index) {
    const setup_ui_file &file = config->files[index];
    const char *name = file.name ? file.name : "";
    converted.files.push_back(setup_ui::FileSpec{name, name, file.label ? file.label : name});
  }
  return converted;
}

} // namespace

struct setup_ui_server {
  std::unique_ptr<setup_ui::Server> server;
  CState validator;
  std::string last_error;
  std::vector<std::string> messages; // event text valid until the next poll
};

// The ABI keeps positional parameters for C callers; scopes and byte budgets
// are deliberately adjacent even though their types are convertible.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
setup_ui_server *setup_ui_server_new(const setup_ui_config *config, const char *staging_root,
                                     int lan_scope, uint64_t max_file_bytes) {
  if (!config || !staging_root) {
    return nullptr;
  }
  auto *handle = new (std::nothrow) setup_ui_server();
  if (!handle) {
    return nullptr;
  }
  setup_ui::ServerOptions options;
  options.config = convert_config(config);
  options.staging_root = staging_root;
  options.scope = lan_scope ? setup_ui::Scope::LocalNetwork : setup_ui::Scope::Loopback;
  if (max_file_bytes != 0) {
    options.max_file_bytes = static_cast<std::size_t>(max_file_bytes);
  }
  handle->validator = CState{config->validate, config->userdata};
  setup_ui::Validate wrapped = [handle](const std::vector<setup_ui::UploadedFile> &files) {
    return validate_c(files, &handle->validator);
  };
  handle->server = std::make_unique<setup_ui::Server>(std::move(options), std::move(wrapped));
  return handle;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

void setup_ui_server_free(setup_ui_server *server) {
  delete server;
}

int setup_ui_server_start(setup_ui_server *server) {
  if (!server || !server->server) {
    return 0;
  }
  if (!server->server->start()) {
    server->last_error = server->server->last_error();
    return 0;
  }
  return 1;
}

void setup_ui_server_stop(setup_ui_server *server) {
  if (server && server->server) {
    server->server->stop();
  }
}

uint16_t setup_ui_server_port(setup_ui_server *server) {
  return server && server->server ? server->server->port() : 0;
}

size_t setup_ui_server_poll(setup_ui_server *server, setup_ui_event *events, size_t capacity) {
  if (!server || !server->server || events == nullptr || capacity == 0) {
    return 0;
  }
  std::vector<setup_ui::Event> delivered;
  server->server->poll(delivered);
  const size_t count = std::min(delivered.size(), capacity);
  // Message pointers must stay valid until the next poll: copy each event's
  // message into per-handle storage, then fill the event array from it.
  server->messages.clear();
  for (size_t index = 0; index < count; ++index) {
    const setup_ui::Event &source = delivered[index];
    server->messages.push_back(source.failed_message.empty() ? source.uploaded_name
                                                             : source.failed_message);
  }
  for (size_t index = 0; index < count; ++index) {
    const setup_ui::Event &source = delivered[index];
    events[index] = setup_ui_event{server->messages[index].c_str(), source.uploaded_size,
                                   static_cast<int>(source.kind), source.port};
  }
  return count;
}

int setup_ui_server_url(setup_ui_server *server, char *url, size_t url_capacity) {
  if (!server || !server->server || url == nullptr || url_capacity == 0) {
    return 0;
  }
  const std::string value = server->server->url();
  if (value.size() + 1 > url_capacity) {
    return 0;
  }
  std::memcpy(url, value.c_str(), value.size() + 1);
  return 1;
}

const char *setup_ui_last_error(setup_ui_server *server) {
  return server && !server->last_error.empty() ? server->last_error.c_str() : "";
}
