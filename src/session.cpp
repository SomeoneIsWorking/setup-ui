// session.cpp — staged setup state shared by every consumer.
//
// The session owns: which requirements are present, where the chosen bytes are
// staged, when the consumer's validator runs, and the wording the screen
// shows. It never decides whether a file is the right one — that is the
// consumer's Validator — and it never opens a picker.
#include "setup_ui/setup_ui.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>

namespace setup_ui {
namespace {

std::string random_suffix() {
  static constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  std::random_device device;
  std::seed_seq sequence{device(), device(), device(), device()};
  std::mt19937 engine(sequence);
  std::uniform_int_distribution<std::size_t> pick(0, sizeof kAlphabet - 2);
  std::string suffix;
  for (int index = 0; index < 8; ++index) {
    suffix.push_back(kAlphabet[pick(engine)]);
  }
  return suffix;
}

} // namespace

struct Session::Impl {
  Config config;
  SessionOptions options;
  Validator validator;
  std::vector<Entry> entries;
  std::vector<StagedFile> staged;
  Status status = Status::Collecting;
  std::string message;
  std::string staging;
  double progress = 0.0;
  bool validated = false;

  const FileSpec *spec_for_name(const std::string &name) const {
    for (const FileSpec &spec : config.files) {
      if (spec.name == name) {
        return &spec;
      }
    }
    return nullptr;
  }

  bool ensure_staging() {
    if (!staging.empty()) {
      return true;
    }
    std::error_code status_code;
    std::filesystem::create_directories(options.staging_root, status_code);
    if (status_code) {
      message = "The setup area could not be created: " + status_code.message();
      return false;
    }
    const std::filesystem::path candidate = options.staging_root / ("setup-" + random_suffix());
    std::filesystem::create_directories(candidate, status_code);
    if (status_code) {
      message = "The setup area could not be created: " + status_code.message();
      return false;
    }
    staging = candidate.string();
    return true;
  }

  // Copies one chosen file into the private staging directory.
  bool stage_file(const FileSpec &spec, const std::filesystem::path &source, bool is_archive,
                  std::string &error) {
    std::error_code status_code;
    const auto size = std::filesystem::file_size(source, status_code);
    if (status_code || !std::filesystem::is_regular_file(source, status_code)) {
      error = "the selected file could not be read";
      return false;
    }
    if (size > options.max_file_bytes) {
      error = "the selected file is larger than this setup accepts";
      return false;
    }
    const std::string leaf = is_archive ? ("archive-" + random_suffix() + ".zip") : spec.name;
    const std::filesystem::path target = std::filesystem::path(staging) / leaf;
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing,
                              status_code);
    if (status_code) {
      error = "the selected file could not be copied: " + status_code.message();
      return false;
    }
    staged.push_back(StagedFile{spec, target, size, is_archive});
    return true;
  }

  void refresh_ready() {
    const std::size_t required = config.files.size();
    std::size_t provided = 0;
    // An archive stands in for the complete set.
    if (std::any_of(staged.begin(), staged.end(),
                    [](const StagedFile &file) { return file.is_archive; })) {
      provided = required;
    } else {
      for (const Entry &entry : entries) {
        provided += entry.provided ? 1U : 0U;
      }
    }
    if (provided >= required && required != 0) {
      status = status == Status::Accepted ? Status::Accepted : Status::Ready;
      message.clear();
      return;
    }
    // A partial selection is not a failure: the player still has rows to fill,
    // so the screen says which ones instead of leaving them to notice.
    if (provided != 0) {
      std::string missing;
      for (const Entry &entry : entries) {
        if (entry.provided) {
          continue;
        }
        missing += (missing.empty() ? "" : ", ") + entry.spec.name;
      }
      message = missing.empty() ? std::string{} : "Still needed: " + missing;
    } else {
      message.clear();
    }
  }

  void apply_staged_to_entries() {
    for (Entry &entry : entries) {
      entry.provided = false;
      entry.size = 0;
    }
    for (const StagedFile &file : staged) {
      if (file.is_archive) {
        continue;
      }
      for (Entry &entry : entries) {
        if (entry.spec.key == file.spec.key) {
          entry.provided = true;
          entry.size = file.size;
        }
      }
    }
  }
};

Session::Session(Config config, SessionOptions options, Validator validator)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->options = std::move(options);
  impl_->validator = std::move(validator);
  for (const FileSpec &spec : impl_->config.files) {
    impl_->entries.push_back(Entry{spec, false, 0});
  }
}

Session::~Session() {
  if (!impl_->staging.empty()) {
    std::error_code status_code;
    std::filesystem::remove_all(impl_->staging, status_code);
  }
}

std::size_t Session::add_selected(const std::vector<std::filesystem::path> &paths,
                                  std::string &error) {
  error.clear();
  if (paths.empty()) {
    error = "no files were selected";
    return 0;
  }
  if (!impl_->ensure_staging()) {
    error = impl_->message;
    return 0;
  }
  // One archive replaces the whole set; otherwise every selection must match a
  // requirement by exact name.
  if (impl_->config.accepts_archive && paths.size() == 1) {
    const std::string name = paths.front().filename().string();
    const bool archive = name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
    if (archive) {
      reset();
      if (!impl_->ensure_staging()) {
        error = impl_->message;
        return 0;
      }
      FileSpec archive_spec{"archive", name, name};
      if (!impl_->stage_file(archive_spec, paths.front(), true, error)) {
        return 0;
      }
      impl_->status = Status::Ready;
      impl_->message.clear();
      return 1;
    }
  }
  std::size_t added = 0;
  for (const std::filesystem::path &path : paths) {
    const std::string name = path.filename().string();
    const FileSpec *spec = impl_->spec_for_name(name);
    if (spec == nullptr) {
      if (error.empty()) {
        error = "the selection contains a file that is not part of the set: " + name;
      }
      continue;
    }
    const bool duplicate =
        std::any_of(impl_->staged.begin(), impl_->staged.end(), [&](const StagedFile &file) {
          return !file.is_archive && file.spec.key == spec->key;
        });
    if (duplicate) {
      continue;
    }
    std::string copy_error;
    if (!impl_->stage_file(*spec, path, false, copy_error)) {
      if (error.empty()) {
        error = copy_error;
      }
      continue;
    }
    ++added;
  }
  impl_->apply_staged_to_entries();
  if (added != 0 && error.empty()) {
    impl_->message.clear();
  }
  impl_->refresh_ready();
  if (error.empty()) {
    // The screen reports what is still missing from the set.
    error = impl_->status == Status::Ready || impl_->status == Status::Accepted ? std::string{}
                                                                              : impl_->message;
  }
  return added;
}

void Session::discard_stale_staging(const std::filesystem::path &staging_root) {
  std::error_code status_code;
  std::filesystem::directory_iterator entries(staging_root, status_code);
  if (status_code) {
    return;
  }
  for (const std::filesystem::directory_entry &entry : entries) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("setup-", 0) == 0) {
      std::error_code remove_code;
      std::filesystem::remove_all(entry.path(), remove_code);
    }
  }
}

void Session::reset() {
  if (!impl_->staging.empty()) {
    std::error_code status_code;
    std::filesystem::remove_all(impl_->staging, status_code);
  }
  impl_->staging.clear();
  impl_->staged.clear();
  impl_->validated = false;
  impl_->status = Status::Collecting;
  impl_->message.clear();
  impl_->progress = 0.0;
  impl_->apply_staged_to_entries();
}

void Session::validate_if_ready() {
  if (impl_->status != Status::Ready || impl_->validated) {
    return;
  }
  if (impl_->config.files.empty() && impl_->staged.empty()) {
    return;
  }
  impl_->validated = true;
  if (!impl_->validator) {
    impl_->status = Status::Accepted;
    return;
  }
  std::string verdict = impl_->validator(impl_->staged);
  if (verdict.empty()) {
    impl_->status = Status::Accepted;
    impl_->message = "Disk set accepted.";
    return;
  }
  // A rejected set stays staged so its rows remain visible; the player can
  // choose again, which discards it.
  impl_->status = Status::Rejected;
  impl_->message = std::move(verdict);
  impl_->validated = false;
}

const Config &Session::config() const { return impl_->config; }

const std::vector<Entry> &Session::entries() const { return impl_->entries; }

Status Session::status() const { return impl_->status; }

const std::string &Session::message() const { return impl_->message; }

double Session::progress() const { return impl_->progress; }

void Session::set_progress(double fraction) {
  impl_->progress = std::clamp(fraction, 0.0, 1.0);
}

const std::string &Session::staging_directory() const { return impl_->staging; }

} // namespace setup_ui
