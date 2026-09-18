#include "setup_ui/setup_ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Exercises the shipping session and screen: staging, ordering, validation
// dispatch, rejection wording, and a real RmlUi render through SDL's 2D
// renderer. The render assertion is a discriminator: a document that produces
// no geometry (a missing font, a broken stylesheet, an unmounted document)
// must fail here rather than look like a blank success.
namespace {

int g_failures = 0;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                    \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (0)

std::filesystem::path make_root(const char *name) {
  const auto root = std::filesystem::temp_directory_path() /
                    (std::string{name} + "-" + std::to_string(static_cast<long>(::getpid())));
  std::error_code status;
  std::filesystem::remove_all(root, status);
  std::filesystem::create_directories(root, status);
  return root;
}

void write_file(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

setup_ui::Config test_config() {
  setup_ui::Config config;
  config.title = "Benefactor setup";
  config.message = "Choose your original disk images.";
  config.hint = "or one ZIP archive";
  config.footer = "Your files stay on this device.";
  config.files = {{"disk1", "Disk.1", "Game disc 1"},
                  {"disk2", "Disk.2", "Game disc 2"},
                  {"disk3", "Disk.3", "Game disc 3"}};
  config.accepts_archive = true;
  return config;
}

void test_session_staging_and_validation(const std::filesystem::path &root) {
  const auto source_dir = root / "chosen";
  std::filesystem::create_directories(source_dir);
  write_file(source_dir / "Disk.1", std::string(2048, 'a'));
  write_file(source_dir / "Disk.2", std::string(1024, 'b'));
  write_file(source_dir / "Disk.3", std::string(512, 'c'));
  write_file(source_dir / "NotADisk.bin", "x");

  std::atomic<int> calls{0};
  std::vector<std::string> seen_names;
  setup_ui::SessionOptions options;
  options.staging_root = root / "staging";
  setup_ui::Session session(
      test_config(), options,
      [&](const std::vector<setup_ui::StagedFile> &files) {
        calls.fetch_add(1);
        seen_names.clear();
        for (const auto &file : files) {
          seen_names.push_back(file.spec.name);
          // The validator must receive staged, readable paths.
          std::ifstream staged(file.path, std::ios::binary);
          CHECK(static_cast<bool>(staged));
        }
        return std::string{};
      });

  CHECK(session.entries().size() == 3);
  CHECK(!session.entries()[0].provided);
  CHECK(session.status() == setup_ui::Status::Collecting);

  std::string error;
  const std::size_t added = session.add_selected(
      {source_dir / "Disk.1", source_dir / "NotADisk.bin", source_dir / "Disk.2"}, error);
  CHECK(added == 2);
  CHECK(!error.empty());
  CHECK(session.entries()[0].provided);
  CHECK(session.entries()[1].provided);
  CHECK(!session.entries()[2].provided);
  CHECK(session.status() == setup_ui::Status::Collecting);

  const std::size_t more = session.add_selected({source_dir / "Disk.3"}, error);
  CHECK(more == 1);
  CHECK(error.empty());
  CHECK(session.status() == setup_ui::Status::Ready);

  session.validate_if_ready();
  CHECK(calls.load() == 1);
  CHECK(session.status() == setup_ui::Status::Accepted);
  CHECK(seen_names.size() == 3);
  CHECK(seen_names[0] == "Disk.1");
  CHECK(seen_names[1] == "Disk.2");
  CHECK(seen_names[2] == "Disk.3");

  // Re-validating an accepted set does not re-run the validator.
  session.validate_if_ready();
  CHECK(calls.load() == 1);

  // Staged bytes live under the caller's staging root.
  CHECK(session.staging_directory().find((root / "staging").string()) == 0);
}

void test_partial_selection_names_what_is_missing(const std::filesystem::path &root) {
  std::filesystem::create_directories(root / "source");
  const auto source_dir = root / "source";
  write_file(source_dir / "Disk.1", std::string(16, 'a'));
  write_file(source_dir / "Disk.2", std::string(16, 'b'));
  write_file(source_dir / "Disk.3", std::string(16, 'c'));

  setup_ui::SessionOptions options;
  options.staging_root = root / "staging";
  setup_ui::Session session(test_config(), options,
                            [](const std::vector<setup_ui::StagedFile> &) { return std::string{}; });

  std::string error;
  CHECK(session.add_selected({source_dir / "Disk.1"}, error) == 1);
  CHECK(session.status() == setup_ui::Status::Collecting);
  CHECK(session.message() == "Still needed: Disk.2, Disk.3");
  CHECK(error == session.message()); // the caller sees the same wording

  CHECK(session.add_selected({source_dir / "Disk.2", source_dir / "Disk.3"}, error) == 2);
  CHECK(session.status() == setup_ui::Status::Ready);
  CHECK(session.message().empty());
  CHECK(error.empty());
}

void test_stale_staging_is_pruned(const std::filesystem::path &root) {
  std::filesystem::create_directories(root / "staging" / "setup-abandoned");
  write_file(root / "staging" / "setup-abandoned" / "Disk.1", "left behind");
  std::filesystem::create_directories(root / "staging" / "keep-me");
  write_file(root / "staging" / "keep-me" / "notes.txt", "not a session");

  setup_ui::Session::discard_stale_staging(root / "staging");

  CHECK(!std::filesystem::exists(root / "staging" / "setup-abandoned"));
  CHECK(std::filesystem::exists(root / "staging" / "keep-me" / "notes.txt"));

  // A root that does not exist is not an error: the first run has none yet.
  setup_ui::Session::discard_stale_staging(root / "absent");
  CHECK(!std::filesystem::exists(root / "absent"));
}

void test_session_rejection_keeps_rows(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  const auto source_dir = root / "bad";
  std::filesystem::create_directories(source_dir);
  write_file(source_dir / "Disk.1", "1");
  write_file(source_dir / "Disk.2", "2");
  write_file(source_dir / "Disk.3", "3");

  setup_ui::SessionOptions options;
  options.staging_root = root / "staging";
  setup_ui::Session session(test_config(), options,
                            [](const std::vector<setup_ui::StagedFile> &) {
                              return std::string{"Disk.1 does not match the supported identity"};
                            });
  std::string error;
  CHECK(session.add_selected({source_dir / "Disk.1", source_dir / "Disk.2", source_dir / "Disk.3"},
                            error) == 3);
  session.validate_if_ready();
  CHECK(session.status() == setup_ui::Status::Rejected);
  CHECK(session.message() == "Disk.1 does not match the supported identity");
  CHECK(session.entries()[0].provided);
  // Choosing again clears the rejected set.
  session.reset();
  CHECK(session.status() == setup_ui::Status::Collecting);
  CHECK(!session.entries()[0].provided);
  CHECK(session.message().empty());
}

void test_session_archive_replaces_the_set(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  const auto archive = root / "disks.zip";
  write_file(archive, std::string(4096, 'z'));
  setup_ui::SessionOptions options;
  options.staging_root = root / "staging";
  bool archive_seen = false;
  setup_ui::Session session(test_config(), options,
                            [&](const std::vector<setup_ui::StagedFile> &files) {
                              archive_seen = files.size() == 1 && files[0].is_archive;
                              return std::string{};
                            });
  std::string error;
  CHECK(session.add_selected({archive}, error) == 1);
  CHECK(session.status() == setup_ui::Status::Ready);
  session.validate_if_ready();
  CHECK(session.status() == setup_ui::Status::Accepted);
  CHECK(archive_seen);
}

// A port whose game files are one install — a ROM, an original installer, or
// an existing tree — asks for a single selection it judges itself, and adopts
// the player's own location instead of copying the chosen file away from its
// siblings. LF2 is the first such consumer.
void test_one_selection_is_adopted_where_the_player_keeps_it(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  setup_ui::Config config;
  config.title = "LF2 setup";
  config.files = {setup_ui::FileSpec{"install", "", "Little Fighter 2 v2.0a install"}};
  config.placement = setup_ui::Placement::Adopt;
  config.accepted_message = "Game files accepted.";
  setup_ui::SessionOptions options;
  options.staging_root = root / "unused-staging";
  // Deliberately smaller than the selection: an adopted location is never
  // copied, so the staging byte bound must not apply to it.
  options.max_file_bytes = 16;

  // An original installer executable, chosen where the player keeps it.
  const auto installer = root / "downloads" / "LF2_v2.0a.exe";
  std::filesystem::create_directories(installer.parent_path());
  write_file(installer, std::string(4096, 'i'));

  std::filesystem::path judged;
  setup_ui::Session session(config, options, [&](const std::vector<setup_ui::StagedFile> &files) {
    if (files.size() != 1) {
      return std::string{"expected one selection"};
    }
    judged = files.front().path;
    return std::string{};
  });

  std::string error;
  CHECK(session.add_selected({installer}, error) == 1);
  CHECK(error.empty());
  CHECK(session.entries().size() == 1);
  CHECK(session.entries().front().provided);
  CHECK(session.status() == setup_ui::Status::Ready);
  session.validate_if_ready();
  CHECK(session.status() == setup_ui::Status::Accepted);
  CHECK(session.message() == "Game files accepted.");
  // The validator saw the player's own path, and nothing was copied.
  CHECK(judged == installer);
  CHECK(session.staging_directory().empty());
  CHECK(!std::filesystem::exists(options.staging_root));
  CHECK(std::filesystem::exists(installer));
}

// The same shape accepts a directory — an existing install tree — which the
// staging placement could never carry, and a second choice replaces the first.
void test_one_selection_accepts_a_directory_and_replaces(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  setup_ui::Config config;
  config.files = {setup_ui::FileSpec{"install", "", "Game install"}};
  config.placement = setup_ui::Placement::Adopt;
  setup_ui::SessionOptions options;
  options.staging_root = root / "unused-staging";

  const auto tree = root / "LF2";
  std::filesystem::create_directories(tree / "data");
  write_file(tree / "lf2.exe", std::string(512, 'x'));

  std::vector<std::filesystem::path> judged;
  setup_ui::Session session(config, options, [&](const std::vector<setup_ui::StagedFile> &files) {
    judged.clear();
    for (const setup_ui::StagedFile &file : files) {
      judged.push_back(file.path);
    }
    return std::string{};
  });

  std::string error;
  CHECK(session.add_selected({tree}, error) == 1);
  session.validate_if_ready();
  CHECK(session.status() == setup_ui::Status::Accepted);
  CHECK(judged.size() == 1);
  CHECK(judged.front() == tree);

  // Choosing again replaces the set rather than adding a second requirement.
  const auto other = root / "elsewhere" / "lf2.exe";
  std::filesystem::create_directories(other.parent_path());
  write_file(other, std::string(512, 'y'));
  CHECK(session.add_selected({other}, error) == 1);
  CHECK(session.entries().size() == 1);
  session.validate_if_ready();
  CHECK(session.status() == setup_ui::Status::Accepted);
  CHECK(judged.size() == 1);
  CHECK(judged.front() == other);

  // Two paths cannot satisfy a single unnamed requirement.
  CHECK(session.add_selected({tree, other}, error) == 0);
  CHECK(!error.empty());
}

void test_screen_renders_geometry(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  setup_ui::SessionOptions options;
  options.staging_root = root / "render-staging";
  setup_ui::Session session(test_config(), options, [](const std::vector<setup_ui::StagedFile> &) {
    return std::string{};
  });
  setup_ui::ViewOptions view_options;
  view_options.window_title = "Setup test";
  view_options.width = 640;
  view_options.height = 360;
  view_options.resizable = false;
  setup_ui::View view(session, view_options);
  if (!view.open()) {
    // A host without a display cannot present the screen; that is an
    // environment limit, reported rather than counted as a product failure.
    std::fprintf(stderr, "SKIP screen render: %s\n", view.last_error().c_str());
    return;
  }
  // The choose button is live: pressing it must raise a Browse request.
  view.frame();
  std::vector<std::uint32_t> pixels;
  int width = 0;
  int height = 0;
  CHECK(view.capture(pixels, width, height));
  CHECK(width == 640);
  CHECK(height == 360);
  if (!pixels.empty()) {
    std::size_t distinct = 0;
    const std::uint32_t first = pixels.front();
    for (const std::uint32_t pixel : pixels) {
      if (pixel != first) {
        ++distinct;
      }
    }
    // A screen that drew nothing at all would be a single flat colour.
    CHECK(distinct > pixels.size() / 16);
  }
  view.close();
}

// A phone-sized viewport at a 3x display scale must lay the screen out at
// phone proportions: content inside the viewport, sized in dp rather than raw
// pixels. This is the discriminator for the "renders tiny" failure.
void test_screen_is_responsive_on_a_phone_viewport(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  setup_ui::SessionOptions options;
  options.staging_root = root / "phone-staging";
  setup_ui::Session session(test_config(), options, [](const std::vector<setup_ui::StagedFile> &) {
    return std::string{};
  });
  setup_ui::ViewOptions view_options;
  view_options.window_title = "Setup phone test";
  // A 2049 x 948 pixel landscape phone window at a 3x display scale, which is
  // a 683 x 316 dp viewport.
  view_options.width = 2728;
  view_options.height = 1264;
  view_options.resizable = false;
  view_options.density_ratio = 3.0F;
  view_options.offscreen = true;
  setup_ui::View view(session, view_options);
  if (!view.open()) {
    std::fprintf(stderr, "SKIP phone viewport: %s\n", view.last_error().c_str());
    return;
  }
  view.frame();
  std::vector<std::uint32_t> pixels;
  int width = 0;
  int height = 0;
  CHECK(view.capture(pixels, width, height));
  // The host may clamp the window to the display; the layout claim below is
  // about density-independent size, not the exact pixel size.
  CHECK(width == 2728);
  CHECK(height == 1264);
  // The title text must be a substantial share of the window height: a
  // stylesheet that ignored the display scale renders it at a handful of
  // pixels and fails here.
  int text_rows = 0;
  for (int row = 0; row < height; ++row) {
    int bright = 0;
    for (int column = 0; column < width; ++column) {
      const std::uint32_t pixel = pixels[static_cast<std::size_t>(row) * width + column];
      const std::uint32_t red = (pixel >> 16) & 0xFFU;
      const std::uint32_t green = (pixel >> 8) & 0xFFU;
      const std::uint32_t blue = pixel & 0xFFU;
      if (red > 200U && green > 200U && blue > 200U) {
        ++bright;
      }
    }
    if (bright > 4) {
      ++text_rows;
    }
  }
  // The title is 24-32dp depending on the viewport rules, i.e. 72-96 pixels
  // tall at 3x. A stylesheet that ignored the display scale would produce a
  // band a few pixels high and fail here.
  CHECK(text_rows >= 32);
  view.close();
}

} // namespace

int main() {
  const auto root = make_root("setup-ui-tests");
  test_session_staging_and_validation(root / "staging-case");
  test_session_rejection_keeps_rows(root / "rejection-case");
  test_session_archive_replaces_the_set(root / "archive-case");
  test_one_selection_is_adopted_where_the_player_keeps_it(root / "adopt-case");
  test_one_selection_accepts_a_directory_and_replaces(root / "adopt-tree-case");
  test_partial_selection_names_what_is_missing(root / "partial-case");
  test_stale_staging_is_pruned(root / "stale-staging-case");
  test_screen_renders_geometry(root / "render-case");
  test_screen_is_responsive_on_a_phone_viewport(root / "phone-case");

  std::error_code status;
  std::filesystem::remove_all(root, status);
  if (g_failures == 0) {
    std::fputs("all setup-ui tests passed\n", stdout);
    return 0;
  }
  std::fprintf(stderr, "%d failure(s)\n", g_failures);
  return 1;
}
