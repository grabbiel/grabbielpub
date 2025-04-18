#include "http_server.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
const std::string DB_PATH = "/var/lib/grabbiel-db/content.db";
const std::string STORAGE_ROOT = "/var/lib/article-content/";
const std::string GCS_PUBLIC_BUCKET = "grabbiel-media-public";
const std::string GCS_PUBLIC_URL = "https://storage.googleapis.com/";
const std::string LOG_FILE = "/tmp/article-publisher.log";
const std::unordered_set<std::string> VM_ALLOWED = {"html", "css", "js"};
const std::unordered_set<std::string> IMAGE_EXTENSIONS = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".heic", ".bmp", ".tiff"};
const std::unordered_set<std::string> VIDEO_EXTENSIONS = {
    ".mp4", ".mov", ".webm", ".avi", ".mkv"};
const std::vector<std::string> REQUIRED_FILES = {"index.html", "style.css",
                                                 "script.js"};

void log_to_file(const std::string &message) {
  std::ofstream log_file(LOG_FILE, std::ios::app);
  if (log_file) {
    log_file << "[" << time(nullptr) << "] " << message << std::endl;
    log_file.close();
  } else {
    std::cerr << "Failed to open log file: " << LOG_FILE << std::endl;
  }
}

std::string generate_uuid() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 15);
  const char *hex = "0123456789abcdef";
  std::string uuid;
  for (int i = 0; i < 32; ++i) {
    uuid += hex[dis(gen)];
  }
  return uuid;
}

bool validate_article_structure(const fs::path &article_dir) {
  for (const auto &filename : REQUIRED_FILES) {
    fs::path full_path = article_dir / filename;
    if (!fs::exists(full_path)) {
      log_to_file("Validation failed: missing required file " + filename);
      return false;
    }
  }
  log_to_file("Validation passed: all required files present");
  return true;
}

std::string exec_command(const std::string &cmd) {
  std::string result;
  char buffer[128];

  log_to_file("Executing command: " + cmd);

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    log_to_file("Error executing command: popen() failed");
    return "Error executing command";
  }

  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }

  int status = pclose(pipe);
  if (status != 0) {
    log_to_file("Command execution failed with status: " +
                std::to_string(status));
  } else {
    log_to_file("Command executed successfully");
  }

  return result;
}

// Parses metadata.txt (simple key = value format)
std::unordered_map<std::string, std::string>
parse_metadata(const fs::path &metadata_path) {
  std::unordered_map<std::string, std::string> metadata;
  std::ifstream infile(metadata_path);
  std::string line;

  if (!infile.is_open()) {
    log_to_file("Error: Cannot open metadata file at " +
                metadata_path.string());
    return metadata;
  }

  while (std::getline(infile, line)) {
    auto delimiter_pos = line.find('=');
    if (delimiter_pos != std::string::npos) {
      std::string key = line.substr(0, delimiter_pos);
      std::string value = line.substr(delimiter_pos + 1);
      metadata[key] = value;
      log_to_file("Parsed metadata: " + key + " = " + value);
    }
  }

  // Log which required keys are missing
  for (const auto &key : {"title", "slug", "body_markdown", "site_id"}) {
    if (metadata.find(key) == metadata.end()) {
      log_to_file("Error: Required key missing from metadata: " +
                  std::string(key));
    }
  }

  return metadata;
}

std::string regex_escape(const std::string &s) {
  static const std::regex esc{R"([-[\]{}()*+?.,\^$|#\s])"};
  return std::regex_replace(s, esc, R"(\$&)");
}

// Rewrites references like "media/photo.jpg" to their full GCS URL
void rewrite_media_references(
    const fs::path &article_dir,
    const std::unordered_map<std::string, std::string> &media_map,
    int content_id) {

  std::vector<std::string> target_files = {"index.html", "script.js",
                                           "style.css"};
  std::string base_url =
      "https://server.grabbiel.com/article/" + std::to_string(content_id) + "/";

  for (const auto &filename : target_files) {
    fs::path file_path = article_dir / filename;
    if (!fs::exists(file_path))
      continue;

    std::ifstream in(file_path);
    if (!in) {
      std::cerr << "[rewrite] Failed to open " << file_path << " for reading\n";
      continue;
    }

    std::stringstream buffer;
    buffer << in.rdbuf();
    in.close();
    std::string content = buffer.str();

    bool modified = false;

    // 🖼️ Replace all media/xxx with their full GCS URL
    for (const auto &[local_path, gcs_url] : media_map) {
      std::regex pattern(regex_escape(local_path));
      std::string new_content = std::regex_replace(content, pattern, gcs_url);
      if (new_content != content) {
        content = new_content;
        modified = true;
      }
    }

    // 📄 Replace local script and style references with article URL-based ones
    if (filename == "index.html") {
      std::regex css_pattern(R"(href\s*=\s*["']style\.css["'])");
      std::regex js_pattern(R"(src\s*=\s*["']script\.js["'])");

      std::string new_content = std::regex_replace(
          content, css_pattern, "href=\"" + base_url + "style.css\"");
      if (new_content != content) {
        content = new_content;
        modified = true;
      }

      new_content = std::regex_replace(content, js_pattern,
                                       "src=\"" + base_url + "script.js\"");
      if (new_content != content) {
        content = new_content;
        modified = true;
      }
    }

    if (modified) {
      std::ofstream out(file_path);
      if (!out) {
        std::cerr << "[rewrite] Failed to open " << file_path
                  << " for writing\n";
        continue;
      }
      out << content;
      std::cout << "[rewrite] Rewrote media references in: " << file_path
                << "\n";
    }
  }
}

// Store file reference in database
bool store_file_reference(int content_id, const std::string &file_type,
                          const std::string &file_path) {
  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_stmt *stmt;
  const char *sql = "INSERT INTO content_files (content_id, file_type, "
                    "file_path, is_main) VALUES (?, ?, ?, 0);";

  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    log_to_file("Error preparing insert statement: " +
                std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_int(stmt, 1, content_id);
  sqlite3_bind_text(stmt, 2, file_type.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, file_path.c_str(), -1, SQLITE_STATIC);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    log_to_file("Error inserting file reference: " +
                std::string(sqlite3_errmsg(db)));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

// Copies article files to both local storage and GCS
bool store_article_files(const fs::path &article_dir, int content_id) {
  log_to_file("Storing article files from " + article_dir.string() +
              " for content ID " + std::to_string(content_id));
  fs::path local_dest = STORAGE_ROOT + std::to_string(content_id);

  std::unordered_map<std::string, std::string> media_url_map;

  try {
    fs::create_directories(local_dest);
    log_to_file("Created local directory: " + local_dest.string());

    for (const auto &entry : fs::recursive_directory_iterator(article_dir)) {
      if (entry.is_directory())
        continue;
      if (entry.path().filename() == "metadata.txt")
        continue;

      fs::path rel_path = fs::relative(entry.path(), article_dir);
      std::string ext = entry.path().extension().string();
      std::string file_type;
      if (!ext.empty())
        file_type = ext.substr(1);
      else
        file_type = "bin";

      if (VM_ALLOWED.count(file_type)) {
        // Defer copying until after we rewrite HTML/JS/CSS
        continue;
      }

      std::string category;
      if (IMAGE_EXTENSIONS.count(ext)) {
        category = "images/originals/";
      } else if (VIDEO_EXTENSIONS.count(ext)) {
        category = "videos/originals/";
      } else {
        log_to_file("Unsupported media type skipped: " + entry.path().string());
        continue;
      }

      std::string random_name = generate_uuid() + ext;
      std::string gcs_key = category + random_name;
      std::string tmp_path = "/tmp/" + random_name;
      fs::copy_file(entry.path(), tmp_path,
                    fs::copy_options::overwrite_existing);

      std::string cmd = "gsutil cp \"" + tmp_path + "\" gs://" +
                        GCS_PUBLIC_BUCKET + "/" + gcs_key;
      log_to_file("Uploading media file to GCS: " + cmd);
      std::string result = exec_command(cmd);
      log_to_file("GCS upload result: " + result);

      fs::remove(tmp_path);

      std::string gcs_url = GCS_PUBLIC_URL + GCS_PUBLIC_BUCKET + "/" + gcs_key;
      media_url_map["media/" + entry.path().filename().string()] = gcs_url;
      store_file_reference(content_id, file_type, gcs_url);
    }

    // 🧠 Patch references in-place before saving static files
    rewrite_media_references(article_dir, media_url_map, content_id);

    // ⬇️ Now copy HTML/JS/CSS files AFTER they were patched
    for (const auto &entry : fs::recursive_directory_iterator(article_dir)) {
      if (entry.is_directory())
        continue;
      if (entry.path().filename() == "metadata.txt")
        continue;

      fs::path rel_path = fs::relative(entry.path(), article_dir);
      std::string ext = entry.path().extension().string();
      std::string file_type;
      if (!ext.empty())
        file_type = ext.substr(1);
      else
        file_type = "bin";

      if (VM_ALLOWED.count(file_type)) {
        fs::path dest = local_dest / rel_path;
        fs::create_directories(dest.parent_path());
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
        store_file_reference(content_id, file_type, rel_path.string());
        log_to_file("Copied local-only file: " + rel_path.string());
      }
    }

    // 🧹 Clean up /tmp/ folder if article_dir was a temp upload
    if (article_dir.string().rfind("/tmp/", 0) == 0) {
      std::error_code ec;
      fs::remove_all(article_dir, ec);
      if (ec) {
        log_to_file("⚠️ Failed to clean up temp folder: " +
                    article_dir.string());
      } else {
        log_to_file("🧹 Cleaned up temp folder: " + article_dir.string());
      }
    }

    return true;

  } catch (const std::exception &e) {
    log_to_file("Error storing article files: " + std::string(e.what()));
    return false;
  }
}

// Inserts or updates content_blocks and articles
bool update_article_metadata(
    const std::unordered_map<std::string, std::string> &meta, int &content_id) {
  log_to_file("Updating article metadata");

  // Check for required keys first
  const std::vector<std::string> required_keys = {"title", "slug",
                                                  "body_markdown", "site_id"};

  for (const auto &key : required_keys) {
    if (meta.find(key) == meta.end()) {
      log_to_file("Error: Missing required metadata key: " + key);
      return false;
    }
  }

  // Now safely extract values
  const std::string title = meta.at("title");
  const std::string slug = meta.at("slug");
  const std::string body_md = meta.at("body_markdown");
  const std::string site_id = meta.at("site_id");

  // Print metadata for debugging
  log_to_file("Processing metadata:");
  log_to_file("  title: " + title);
  log_to_file("  slug: " + slug);
  log_to_file("  site_id: " + site_id);
  log_to_file("  body_md length: " + std::to_string(body_md.length()) +
              " chars");

  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database at " + DB_PATH + ": " +
                std::string(sqlite3_errmsg(db)));
    return false;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  sqlite3_stmt *stmt;
  const char *select_sql =
      "SELECT id FROM content_blocks WHERE url_slug = ? AND site_id = ?";

  if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, site_id.c_str(), -1, SQLITE_STATIC);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    content_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    log_to_file("Found existing content with ID: " +
                std::to_string(content_id));

    const char *update_sql =
        "UPDATE articles SET body_markdown = ?, last_edited = "
        "CURRENT_TIMESTAMP WHERE content_id = ?";

    if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_text(stmt, 1, body_md.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, content_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }
  } else {
    sqlite3_finalize(stmt);
    log_to_file("Creating new content entry");

    const char *insert_cb_sql =
        "INSERT INTO content_blocks (title, url_slug, type_id, site_id, "
        "language) VALUES (?, ?, 1, ?, 'en');";

    if (sqlite3_prepare_v2(db, insert_cb_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, slug.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, site_id.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }

    sqlite3_finalize(stmt);

    content_id = (int)sqlite3_last_insert_rowid(db);
    log_to_file("Created content with new ID: " + std::to_string(content_id));

    const char *insert_article_sql =
        "INSERT INTO articles (content_id, body_markdown) VALUES (?, ?);";

    if (sqlite3_prepare_v2(db, insert_article_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_int(stmt, 1, content_id);
    sqlite3_bind_text(stmt, 2, body_md.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }
  }

  sqlite3_finalize(stmt);

  if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    log_to_file("Failed to commit transaction: " +
                std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  return true;
}

void handle_publish_request(const HttpRequest &req, HttpResponse &res) {
  log_to_file("Received publish request");

  std::string article_path;

  // Log headers for debugging
  log_to_file("Request headers:");
  for (const auto &pair : req.headers) {
    log_to_file("[Header] " + pair.first + ": " + pair.second);
  }

  // Log query params
  log_to_file("Query parameters:");
  for (const auto &pair : req.query_params) {
    log_to_file("[Query] " + pair.first + ": " + pair.second);
  }

  // Determine article path
  auto it = req.query_params.find("path");
  if (it != req.query_params.end()) {
    article_path = it->second;
    log_to_file("Using path from query parameter: " + article_path);
  } else if (!req.body.empty()) {
    article_path = req.body;
    log_to_file("Using path from request body: " + article_path);
  } else {
    log_to_file("No path provided in query parameters or request body");
    res.send(400, "Missing path parameter. Provide it either as a query "
                  "parameter '?path=' or in the request body.");
    return;
  }

  // Validate metadata.txt exists
  fs::path meta_file = fs::path(article_path) / "metadata.txt";
  if (!fs::exists(meta_file)) {
    log_to_file("Metadata file not found at: " + meta_file.string());
    res.send(400, "Missing metadata.txt at path: " + article_path);
    return;
  }

  // ✅ Validate required article file: index.html only
  fs::path index_file = fs::path(article_path) / "index.html";
  if (!fs::exists(index_file)) {
    log_to_file("Missing index.html at: " + index_file.string());
    res.send(400, "Article is missing required file: index.html");
    return;
  }

  // Proceed with metadata parsing and database update
  auto metadata = parse_metadata(meta_file);
  int content_id = -1;

  if (!update_article_metadata(metadata, content_id)) {
    log_to_file("Database update failed for article at: " + article_path);
    res.send(500, "Database update failed");
    return;
  }

  if (!store_article_files(article_path, content_id)) {
    log_to_file("File storage failed for article at: " + article_path);
    res.send(500, "File storage failed");
    return;
  }

  log_to_file("Article published successfully with ID: " +
              std::to_string(content_id));
  res.send(200, "Article published with ID: " + std::to_string(content_id));
}

int main() {
  log_to_file("Starting Article Publisher Service");

  // Create storage directory if it doesn't exist
  std::string mkdir_cmd = "mkdir -p " + STORAGE_ROOT;
  exec_command(mkdir_cmd);

  HttpServer server(8082); // localhost only
  server.route("/publish", handle_publish_request);

  log_to_file("Server initialized, listening on port 8082");
  server.run();

  log_to_file("Server shutting down");
  return 0;
}
