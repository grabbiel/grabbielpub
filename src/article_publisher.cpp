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

int generate_media_uuid(int content_id, const std::string &filename) {
  std::string base = std::to_string(content_id) + "-" + filename;
  std::hash<std::string> hasher;
  size_t hash_value = hasher(base);
  return static_cast<int>(hash_value % 2147483647);
}

bool extract_image_metadata(const fs::path &image_path, int &width, int &height,
                            std::string &mime_type, int &size_bytes) {
  try {
    // Get file size
    size_bytes = fs::file_size(image_path);

    // Use ImageMagick to get dimensions and MIME type
    std::string cmd =
        "identify -format \"%w %h %m\" \"" + image_path.string() + "\"";
    std::string result = exec_command(cmd);

    // Parse the result
    std::istringstream iss(result);
    std::string format;
    if (!(iss >> width >> height >> format)) {
      log_to_file("Failed to parse image metadata: " + result);
      return false;
    }

    // Map format to MIME type
    static const std::unordered_map<std::string, std::string> format_to_mime = {
        {"JPEG", "image/jpeg"}, {"JPG", "image/jpeg"},  {"PNG", "image/png"},
        {"GIF", "image/gif"},   {"WEBP", "image/webp"}, {"HEIC", "image/heic"},
        {"BMP", "image/bmp"},   {"TIFF", "image/tiff"}};

    auto it = format_to_mime.find(format);
    if (it != format_to_mime.end()) {
      mime_type = it->second;
    } else {
      mime_type = "image/" + format;
    }

    return true;
  } catch (const std::exception &e) {
    log_to_file("Error extracting image metadata: " + std::string(e.what()));
    return false;
  }
}

bool extract_video_metadata(const fs::path &video_path, int &duration_seconds,
                            std::string &mime_type, int &size_bytes) {
  try {
    // Get file size
    size_bytes = fs::file_size(video_path);

    // Use ffprobe to get duration
    std::string cmd = "ffprobe -v error -show_entries format=duration -of "
                      "default=noprint_wrappers=1:nokey=1 \"" +
                      video_path.string() + "\"";
    std::string result = exec_command(cmd);

    // Parse the duration (might be a float in seconds)
    float duration_float = 0;
    std::istringstream iss(result);
    if (!(iss >> duration_float)) {
      log_to_file("Failed to parse video duration: " + result);
      return false;
    }

    duration_seconds = static_cast<int>(duration_float);

    // Map extension to MIME type
    std::string ext = video_path.extension().string();
    static const std::unordered_map<std::string, std::string> ext_to_mime = {
        {".mp4", "video/mp4"},
        {".mov", "video/quicktime"},
        {".webm", "video/webm"},
        {".avi", "video/x-msvideo"},
        {".mkv", "video/x-matroska"}};

    auto it = ext_to_mime.find(ext);
    if (it != ext_to_mime.end()) {
      mime_type = it->second;
    } else {
      mime_type = "video/mp4"; // Default to mp4
    }

    return true;
  } catch (const std::exception &e) {
    log_to_file("Error extracting video metadata: " + std::string(e.what()));
    return false;
  }
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

std::string read_summary_file(const fs::path &article_dir) {
  fs::path summary_path = article_dir / "summary.txt";
  if (!fs::exists(summary_path)) {
    log_to_file("Summary file not found, using empty summary");
    return "";
  }

  std::ifstream infile(summary_path);
  if (!infile.is_open()) {
    log_to_file("Failed to open summary.txt");
    return "";
  }

  std::stringstream buffer;
  buffer << infile.rdbuf();
  return buffer.str();
}

bool process_tags(int content_id, const std::string &tags_list) {
  if (tags_list.empty()) {
    return true; // No tags to process
  }

  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  // Parse comma-separated list
  std::stringstream ss(tags_list);
  std::string tag;

  while (std::getline(ss, tag, ',')) {
    // Trim whitespace
    tag.erase(0, tag.find_first_not_of(" \t"));
    tag.erase(tag.find_last_not_of(" \t") + 1);

    if (tag.empty())
      continue;

    // Check if tag exists, if not create it
    int tag_id = -1;
    sqlite3_stmt *stmt;
    const char *select_sql = "SELECT id FROM tags WHERE name = ?";

    if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      continue;
    }

    sqlite3_bind_text(stmt, 1, tag.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
      tag_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    } else {
      sqlite3_finalize(stmt);

      // Insert new tag
      const char *insert_sql = "INSERT INTO tags (name) VALUES (?)";
      if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
        continue;
      }

      sqlite3_bind_text(stmt, 1, tag.c_str(), -1, SQLITE_STATIC);

      if (sqlite3_step(stmt) != SQLITE_DONE) {
        log_to_file("Failed to insert tag: " + std::string(sqlite3_errmsg(db)));
        sqlite3_finalize(stmt);
        continue;
      }

      tag_id = (int)sqlite3_last_insert_rowid(db);
      sqlite3_finalize(stmt);
    }

    // Associate tag with content
    if (tag_id > 0) {
      const char *insert_content_tag = "INSERT OR IGNORE INTO content_tags "
                                       "(content_id, tag_id) VALUES (?, ?)";

      if (sqlite3_prepare_v2(db, insert_content_tag, -1, &stmt, nullptr) !=
          SQLITE_OK) {
        log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
        continue;
      }

      sqlite3_bind_int(stmt, 1, content_id);
      sqlite3_bind_int(stmt, 2, tag_id);

      if (sqlite3_step(stmt) != SQLITE_DONE) {
        log_to_file("Failed to associate tag: " +
                    std::string(sqlite3_errmsg(db)));
      }

      sqlite3_finalize(stmt);
    }
  }

  if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    log_to_file("Failed to commit transaction: " +
                std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_close(db);
  return true;
}

int store_image_metadata(int content_id, int image_uuid,
                         const std::string &original_url,
                         const std::string &filename,
                         const std::string &mime_type, int size_bytes,
                         int width, int height, const std::string &image_type,
                         const std::string &processing_status) {
  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return -1;
  }

  std::string uuid_tmp = std::to_string(image_uuid);
  const char *uuid = uuid_tmp.c_str();

  // First check if image with this UUID already exists
  sqlite3_stmt *stmt;
  const char *select_sql = "SELECT id FROM images WHERE id = ?";
  int image_id = -1;

  if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return -1;
  }

  sqlite3_bind_int(stmt, 1, image_uuid);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    // Image already exists, update it
    image_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char *update_sql =
        "UPDATE images SET original_url = ?, filename = ?, mime_type = ?, "
        "size = ?, width = ?, height = ?, content_id = ?, image_type = ?, "
        "processing_status = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return -1;
    }

    sqlite3_bind_text(stmt, 1, original_url.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mime_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, size_bytes);
    sqlite3_bind_int(stmt, 5, width);
    sqlite3_bind_int(stmt, 6, height);
    sqlite3_bind_int(stmt, 7, content_id);
    sqlite3_bind_text(stmt, 8, image_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, processing_status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 10, image_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return -1;
    }
  } else {
    sqlite3_finalize(stmt);

    // Insert new image
    const char *insert_sql = "INSERT INTO images (id, original_url, filename, "
                             "mime_type, size, width, height, "
                             "content_id, image_type, processing_status) "
                             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return -1;
    }

    sqlite3_bind_int(stmt, 1, image_uuid);
    sqlite3_bind_text(stmt, 2, original_url.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, mime_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, size_bytes);
    sqlite3_bind_int(stmt, 6, width);
    sqlite3_bind_int(stmt, 7, height);
    sqlite3_bind_int(stmt, 8, content_id);
    sqlite3_bind_text(stmt, 9, image_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, processing_status.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return -1;
    }

    image_id = sqlite3_last_insert_rowid(db);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);

  return image_id;
}

std::string process_thumbnail(const fs::path &article_dir, int content_id,
                              bool is_published) {
  fs::path thumbnail_dir = article_dir / "thumbnail";
  if (!fs::exists(thumbnail_dir) || !fs::is_directory(thumbnail_dir)) {
    log_to_file("Thumbnail directory not found");
    return "";
  }

  // Find first image file in thumbnail directory
  std::string thumbnail_url = "";
  for (const auto &entry : fs::directory_iterator(thumbnail_dir)) {
    if (!entry.is_regular_file())
      continue;

    std::string ext = entry.path().extension().string();
    if (IMAGE_EXTENSIONS.count(ext)) {
      // Found thumbnail image
      std::string filename = entry.path().filename().string();
      int uuid = generate_media_uuid(content_id, "thumbnail-" + filename);

      // Extract image metadata
      int width = 0, height = 0, size_bytes = 0;
      std::string mime_type;
      if (!extract_image_metadata(entry.path(), width, height, mime_type,
                                  size_bytes)) {
        log_to_file("Failed to extract metadata for thumbnail: " +
                    entry.path().string());
        width = height = 0;
        size_bytes = fs::file_size(entry.path());
        mime_type = "image/jpeg"; // Default
      }

      if (is_published) {
        // Upload to GCS for published content
        std::string gcs_key = "images/thumbnails/" + std::to_string(uuid) + ext;
        std::string tmp_path = "/tmp/" + std::to_string(uuid) + ext;

        // Copy to tmp then upload to GCS
        fs::copy_file(entry.path(), tmp_path,
                      fs::copy_options::overwrite_existing);
        std::string cmd = "gsutil cp \"" + tmp_path + "\" gs://" +
                          GCS_PUBLIC_BUCKET + "/" + gcs_key;
        log_to_file("Uploading thumbnail to GCS: " + cmd);
        std::string result = exec_command(cmd);
        fs::remove(tmp_path);

        thumbnail_url = GCS_PUBLIC_URL + GCS_PUBLIC_BUCKET + "/" + gcs_key;
      } else {
        // For drafts, use local path
        fs::path local_dest =
            STORAGE_ROOT + std::to_string(content_id) + "/thumbnail" + ext;
        fs::create_directories(local_dest.parent_path());
        fs::copy_file(entry.path(), local_dest,
                      fs::copy_options::overwrite_existing);
        thumbnail_url = "thumbnail" + ext;
      }

      // Store thumbnail metadata in database
      std::string processing_status = "pending";
      int image_id = store_image_metadata(
          content_id, uuid, thumbnail_url, filename, mime_type, size_bytes,
          width, height, "thumbnail", processing_status);

      if (image_id <= 0) {
        log_to_file("Failed to store thumbnail metadata in database");
      }

      log_to_file("Processed thumbnail: " + thumbnail_url);
      break; // Use first image found
    }
  }

  return thumbnail_url;
}

int store_video_metadata(int content_id, const int &video_uuid,
                         const std::string &title, const std::string &gcs_path,
                         const std::string &mime_type, int size_bytes,
                         int duration_seconds, bool is_reel,
                         const std::string &processing_status) {
  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return -1;
  }

  std::string local_uuid = std::to_string(video_uuid);
  const char *uuid = local_uuid.c_str();

  // First check if video with this UUID already exists
  sqlite3_stmt *stmt;
  const char *select_sql = "SELECT id FROM videos WHERE id = ?";
  int video_id = -1;

  if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return -1;
  }

  sqlite3_bind_int(stmt, 1, video_uuid);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    // Video already exists, update it
    video_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char *update_sql =
        "UPDATE videos SET title = ?, gcs_path = ?, mime_type = ?, "
        "size_bytes = ?, duration_seconds = ?, content_id = ?, is_reel = ?, "
        "processing_status = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return -1;
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, gcs_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mime_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, size_bytes);
    sqlite3_bind_int(stmt, 5, duration_seconds);
    sqlite3_bind_int(stmt, 6, content_id);
    sqlite3_bind_int(stmt, 7, is_reel ? 1 : 0);
    sqlite3_bind_text(stmt, 8, processing_status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 9, video_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return -1;
    }
  } else {
    sqlite3_finalize(stmt);

    // Insert new video
    const char *insert_sql =
        "INSERT INTO videos (id, title, gcs_path, mime_type, size_bytes, "
        "duration_seconds, content_id, is_reel, processing_status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return -1;
    }

    sqlite3_bind_int(stmt, 1, video_uuid);
    sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, gcs_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, mime_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, size_bytes);
    sqlite3_bind_int(stmt, 6, duration_seconds);
    sqlite3_bind_int(stmt, 7, content_id);
    sqlite3_bind_int(stmt, 8, is_reel ? 1 : 0);
    sqlite3_bind_text(stmt, 9, processing_status.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return -1;
    }

    video_id = sqlite3_last_insert_rowid(db);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);

  return video_id;
}

bool process_reel_metadata(int content_id, int video_id,
                           const std::string &caption) {
  // If video is already identified as a reel in the videos table,
  // add/update the corresponding record in the reels table

  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  // Check if reel record exists
  sqlite3_stmt *stmt;
  const char *select_sql = "SELECT id FROM reels WHERE video_id = ?";

  if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_int(stmt, 1, video_id);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    // Reel exists, update it
    int reel_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char *update_sql = "UPDATE reels SET caption = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_text(stmt, 1, caption.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, reel_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }
  } else {
    // Create new reel
    sqlite3_finalize(stmt);

    const char *insert_sql =
        "INSERT INTO reels (video_id, caption, sort_order) VALUES (?, ?, 0)";

    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_int(stmt, 1, video_id);
    sqlite3_bind_text(stmt, 2, caption.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

bool store_content_metadata(int content_id, const std::string &key,
                            const std::string &value) {
  if (value.empty()) {
    return true; // Skip empty values
  }

  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  const char *sql = "INSERT OR REPLACE INTO content_metadata (content_id, key, "
                    "value) VALUES (?, ?, ?)";

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  sqlite3_bind_int(stmt, 1, content_id);
  sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, value.c_str(), -1, SQLITE_STATIC);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return true;
}

// Rewrites references like "media/photo.jpg" to their full GCS URL
void rewrite_media_references(
    const fs::path &article_dir,
    const std::unordered_map<std::string, std::string> &media_map,
    int content_id, bool is_published) {

  // If content is a draft, we don't rewrite media references
  if (!is_published) {
    log_to_file("Content is draft - preserving original media references");
    return;
  }

  log_to_file("Content is published - rewriting media references to GCS URLs");
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

    // 🖼️ Replace all media/xxx with their full GCS URL, but only for published
    // content
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
      // Handle CSS files
      std::regex css_pattern(R"(href\s*=\s*["'](?:\.\/)?([^"']*\.css)["'])");
      std::string new_content = std::regex_replace(
          content, css_pattern, "href=\"" + base_url + "$1\"");

      // Handle ALL JavaScript files (not just script.js)
      std::regex js_pattern(R"(src\s*=\s*["'](?:\.\/)?([^"']*\.js)["'])");
      new_content = std::regex_replace(new_content, js_pattern,
                                       "src=\"" + base_url + "$1\"");

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
bool store_article_files(const fs::path &article_dir, int content_id,
                         bool is_published) {
  log_to_file("Storing article files from " + article_dir.string() +
              " for content ID " + std::to_string(content_id) +
              " (Status: " + (is_published ? "published" : "draft") + ")");
  fs::path local_dest = STORAGE_ROOT + std::to_string(content_id);

  std::unordered_map<std::string, std::string> media_url_map;

  try {
    fs::create_directories(local_dest);
    log_to_file("Created local directory: " + local_dest.string());

    // First pass: Process media files (images/videos)
    for (const auto &entry : fs::recursive_directory_iterator(article_dir)) {
      if (entry.is_directory())
        continue;
      if (entry.path().filename() == "metadata.txt")
        continue;

      fs::path rel_path = fs::relative(entry.path(), article_dir);
      std::string ext = entry.path().extension().string();

      if (rel_path.string().find("thumbnail/") == 0) {
        // Skip thumbnail files, they're handled separately
        continue;
      }

      // Process media files
      if (IMAGE_EXTENSIONS.count(ext)) {
        // Process image file
        log_to_file("Processing image: " + entry.path().string());

        std::string filename = entry.path().filename().string();
        int uuid = generate_media_uuid(content_id, filename);

        // Extract image metadata
        int width = 0, height = 0, size_bytes = 0;
        std::string mime_type;
        if (!extract_image_metadata(entry.path(), width, height, mime_type,
                                    size_bytes)) {
          log_to_file("Failed to extract metadata for image: " +
                      entry.path().string());
          width = height = 0;
          size_bytes = fs::file_size(entry.path());
          mime_type = "image/jpeg"; // Default
        }

        // Generate GCS path
        std::string gcs_key = "images/originals/" + std::to_string(uuid) + ext;
        std::string gcs_url =
            GCS_PUBLIC_URL + GCS_PUBLIC_BUCKET + "/" + gcs_key;

        // Store image metadata in database
        std::string processing_status = "pending";
        int image_id = store_image_metadata(
            content_id, uuid, gcs_url, filename, mime_type, size_bytes, width,
            height, "content", processing_status);

        if (image_id <= 0) {
          log_to_file("Failed to store image metadata in database");
          continue;
        }

        // If publishing, upload to GCS
        if (is_published) {
          std::string tmp_path = "/tmp/" + std::to_string(uuid) + ext;
          fs::copy_file(entry.path(), tmp_path,
                        fs::copy_options::overwrite_existing);

          std::string cmd = "gsutil cp \"" + tmp_path + "\" gs://" +
                            GCS_PUBLIC_BUCKET + "/" + gcs_key;
          log_to_file("Uploading image to GCS: " + cmd);
          std::string result = exec_command(cmd);
          log_to_file("GCS upload result: " + result);

          fs::remove(tmp_path);
        }

        // Map local path to GCS URL for reference rewriting
        media_url_map["media/" + filename] = gcs_url;

        // Store file reference
        store_file_reference(content_id, ext.substr(1), gcs_url);

      } else if (VIDEO_EXTENSIONS.count(ext)) {
        // Process video file
        log_to_file("Processing video: " + entry.path().string());

        std::string filename = entry.path().filename().string();
        int uuid = generate_media_uuid(content_id, filename);

        // Extract video metadata
        int duration_seconds = 0, size_bytes = 0;
        std::string mime_type;
        if (!extract_video_metadata(entry.path(), duration_seconds, mime_type,
                                    size_bytes)) {
          log_to_file("Failed to extract metadata for video: " +
                      entry.path().string());
          duration_seconds = 0;
          size_bytes = fs::file_size(entry.path());
          mime_type = "video/mp4"; // Default
        }

        // Generate GCS path
        std::string gcs_key = "videos/originals/" + std::to_string(uuid) + ext;
        std::string gcs_url =
            GCS_PUBLIC_URL + GCS_PUBLIC_BUCKET + "/" + gcs_key;

        // Check if this is a reel (based on directory path or metadata)
        bool is_reel = (rel_path.string().find("reels/") == 0);

        // Store video metadata in database
        std::string processing_status = "pending";
        int video_id = store_video_metadata(
            content_id, uuid, filename, gcs_url, mime_type, size_bytes,
            duration_seconds, is_reel, processing_status);

        if (video_id <= 0) {
          log_to_file("Failed to store video metadata in database");
          continue;
        }

        // If publishing, upload to GCS
        if (is_published) {
          std::string tmp_path = "/tmp/" + std::to_string(uuid) + ext;
          fs::copy_file(entry.path(), tmp_path,
                        fs::copy_options::overwrite_existing);

          std::string cmd = "gsutil cp \"" + tmp_path + "\" gs://" +
                            GCS_PUBLIC_BUCKET + "/" + gcs_key;
          log_to_file("Uploading video to GCS: " + cmd);
          std::string result = exec_command(cmd);
          log_to_file("GCS upload result: " + result);

          fs::remove(tmp_path);
        }

        // Map local path to GCS URL for reference rewriting
        media_url_map["media/" + filename] = gcs_url;

        // Store file reference
        store_file_reference(content_id, ext.substr(1), gcs_url);
      }
    }

    // Rewrite media references in HTML/CSS/JS files as needed
    rewrite_media_references(article_dir, media_url_map, content_id,
                             is_published);

    // Second pass: Copy HTML/CSS/JS files
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

    // Clean up temp folder if needed
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
    const std::unordered_map<std::string, std::string> &meta, int &content_id,
    const fs::path &article_dir) {

  log_to_file("Updating article metadata");

  // Check for required keys first
  const std::vector<std::string> required_keys = {"title", "slug", "site_id"};

  for (const auto &key : required_keys) {
    if (meta.find(key) == meta.end()) {
      log_to_file("Error: Missing required metadata key: " + key);
      return false;
    }
  }

  // Extract values
  const std::string title = meta.at("title");
  const std::string slug = meta.at("slug");
  const std::string site_id = meta.at("site_id");

  // Read summary from file
  std::string summary = read_summary_file(article_dir);

  // Get status if provided, default to "draft"
  std::string status = "draft";
  if (meta.find("status") != meta.end()) {
    status = (meta.at("status") == "1") ? "published" : "draft";
  }

  // Check for tags
  std::string tags_list = "";
  if (meta.find("tags") != meta.end()) {
    tags_list = meta.at("tags");
  }

  // Check for read_time
  std::string read_time = "";
  if (meta.find("read_time") != meta.end()) {
    read_time = meta.at("read_time");
  }

  // Print metadata for debugging
  log_to_file("Processing metadata:");
  log_to_file("  title: " + title);
  log_to_file("  slug: " + slug);
  log_to_file("  site_id: " + site_id);
  log_to_file("  status: " + status);
  log_to_file("  summary length: " + std::to_string(summary.length()) +
              " chars");
  log_to_file("  tags: " + tags_list);
  log_to_file("  read_time: " + read_time);

  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
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

  bool is_new_content = false;
  bool status_changed_to_published = false;

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    content_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    log_to_file("Found existing content with ID: " +
                std::to_string(content_id));

    // Check if status changed to published
    const char *check_status_sql =
        "SELECT status FROM content_blocks WHERE id = ?";

    if (sqlite3_prepare_v2(db, check_status_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_int(stmt, 1, content_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
      std::string old_status = (char *)sqlite3_column_text(stmt, 0);
      if (old_status != "published" && status == "published") {
        status_changed_to_published = true;
        log_to_file("Status changed from " + old_status + " to published");
      }
    }

    sqlite3_finalize(stmt);

    // Update the article's summary
    const char *update_article_sql =
        "UPDATE articles SET summary = ?, last_edited = CURRENT_TIMESTAMP "
        "WHERE content_id = ?";

    if (sqlite3_prepare_v2(db, update_article_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_text(stmt, 1, summary.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, content_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }

    sqlite3_finalize(stmt);

    // Process thumbnail
    std::string thumbnail_url =
        process_thumbnail(article_dir, content_id, status == "published");

    // Update content_blocks with status and thumbnail
    const char *update_cb_sql =
        "UPDATE content_blocks SET status = ?, thumbnail_url = ? WHERE id = ?";

    if (sqlite3_prepare_v2(db, update_cb_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);

    if (!thumbnail_url.empty()) {
      sqlite3_bind_text(stmt, 2, thumbnail_url.c_str(), -1, SQLITE_STATIC);
    } else {
      sqlite3_bind_null(stmt, 2);
    }

    sqlite3_bind_int(stmt, 3, content_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }

    sqlite3_finalize(stmt);

    // Set published_at timestamp if status changed to published
    if (status_changed_to_published) {
      const char *update_published_at_sql =
          "UPDATE articles SET published_at = CURRENT_TIMESTAMP WHERE "
          "content_id = ?";

      if (sqlite3_prepare_v2(db, update_published_at_sql, -1, &stmt, nullptr) !=
          SQLITE_OK) {
        log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
        sqlite3_close(db);
        return false;
      }

      sqlite3_bind_int(stmt, 1, content_id);

      if (sqlite3_step(stmt) != SQLITE_DONE) {
        log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return false;
      }

      sqlite3_finalize(stmt);
    }
  } else {
    sqlite3_finalize(stmt);
    is_new_content = true;
    log_to_file("Creating new content entry");

    // Process thumbnail for new content
    std::string thumbnail_url =
        process_thumbnail(article_dir, content_id, status == "published");

    const char *insert_cb_sql =
        "INSERT INTO content_blocks (title, url_slug, type_id, site_id, "
        "status, language, thumbnail_url) "
        "VALUES (?, ?, 1, ?, ?, 'en', ?);";

    if (sqlite3_prepare_v2(db, insert_cb_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, slug.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, site_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, status.c_str(), -1, SQLITE_STATIC);

    if (!thumbnail_url.empty()) {
      sqlite3_bind_text(stmt, 5, thumbnail_url.c_str(), -1, SQLITE_STATIC);
    } else {
      sqlite3_bind_null(stmt, 5);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      log_to_file("SQL execution error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_finalize(stmt);
      sqlite3_close(db);
      return false;
    }

    sqlite3_finalize(stmt);

    content_id = (int)sqlite3_last_insert_rowid(db);
    log_to_file("Created content with new ID: " + std::to_string(content_id));

    const char *insert_article_sql = "INSERT INTO articles (content_id, "
                                     "summary, published_at) VALUES (?, ?, ?);";

    if (sqlite3_prepare_v2(db, insert_article_sql, -1, &stmt, nullptr) !=
        SQLITE_OK) {
      log_to_file("SQL prepare error: " + std::string(sqlite3_errmsg(db)));
      sqlite3_close(db);
      return false;
    }

    sqlite3_bind_int(stmt, 1, content_id);
    sqlite3_bind_text(stmt, 2, summary.c_str(), -1, SQLITE_STATIC);

    // If publishing immediately, set published_at
    if (status == "published") {
      sqlite3_bind_text(stmt, 3, "CURRENT_TIMESTAMP", -1, SQLITE_STATIC);
    } else {
      sqlite3_bind_null(stmt, 3);
    }

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

  // Process tags (if any)
  if (!tags_list.empty()) {
    process_tags(content_id, tags_list);
  }

  // Store read_time in content_metadata (if provided)
  if (!read_time.empty()) {
    store_content_metadata(content_id, "read_time", read_time);
  }

  return true;
}

void handle_publish_request(const HttpRequest &req, HttpResponse &res) {
  log_to_file("Received publish request");

  std::string article_path;
  bool is_published = false;

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

  // Check publish status
  auto status_it = req.query_params.find("status");
  if (status_it != req.query_params.end() && status_it->second == "1") {
    is_published = true;
    log_to_file("Content will be published as PUBLISHED (status=1)");
  } else {
    log_to_file("Content will be published as DRAFT (status=0)");
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

  // Add status from the query parameter to the metadata
  if (status_it != req.query_params.end()) {
    metadata["status"] = status_it->second;
  }

  int content_id = -1;

  if (!update_article_metadata(metadata, content_id, article_path)) {
    log_to_file("Database update failed for article at: " + article_path);
    res.send(500, "Database update failed");
    return;
  }

  if (!store_article_files(article_path, content_id, is_published)) {
    log_to_file("File storage failed for article at: " + article_path);
    res.send(500, "File storage failed");
    return;
  }

  log_to_file("Article " +
              std::string(is_published ? "published" : "saved as draft") +
              " successfully with ID: " + std::to_string(content_id));
  res.send(200, "Article " +
                    std::string(is_published ? "published" : "saved as draft") +
                    " with ID: " + std::to_string(content_id));
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
