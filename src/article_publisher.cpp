#include "http_server.hpp" // Reuse from media_manager
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
const std::string DB_PATH = "/var/lib/grabbiel-db/content.db";
const std::string STORAGE_ROOT = "/var/lib/article-content/";
const std::string GCS_BUCKET = "gs://grabbiel-media";
const std::string GCS_PUBLIC_BUCKET = "gs://grabbiel-media-public";
const std::string LOG_FILE = "/tmp/article-publisher.log";

// Logging functions (adapted from media_manager.cpp)
void log_to_file(const std::string &message) {
  std::ofstream log_file(LOG_FILE, std::ios::app);
  if (log_file) {
    log_file << "[" << time(nullptr) << "] " << message << std::endl;
    log_file.close();
  } else {
    std::cerr << "Failed to open log file: " << LOG_FILE << std::endl;
  }
}

// Execute a shell command and get output (adapted from media_manager.cpp)
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

// Store file reference in database
bool store_file_reference(int content_id, const std::string &file_type,
                          const std::string &file_path) {
  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK) {
    log_to_file("Failed to open database: " + std::string(sqlite3_errmsg(db)));
    sqlite3_close(db);
    return false;
  }

  // Insert file reference
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

// Function to recursively upload files to GCS - simplified version
bool recursive_upload(const fs::path &dir_path, int content_id) {
  std::string script_path =
      "/tmp/gcs_upload_" + std::to_string(content_id) + ".sh";
  std::ofstream script_file(script_path);

  if (!script_file) {
    log_to_file("Failed to create GCS upload script at " + script_path);
    return false;
  }

  std::string gcs_content_path =
      "gs://grabbiel-media-public/articles/" + std::to_string(content_id);

  script_file << "#!/bin/bash\n";
  script_file << "# Script to upload article files to GCS\n";
  script_file << "set -e\n\n";
  script_file << "cd " << dir_path.string() << "\n\n";

  script_file << "# Upload image files recursively\n";
  script_file << "echo 'Uploading files...'\n";
  script_file
      << "find . -type f | grep -v 'metadata.txt' | while read -r file; do\n";
  script_file << "  echo \"Uploading $file...\"\n";

  // Strip leading './' from the file path
  script_file << "  clean_path=${file#./}\n";
  script_file << "  dest_path=\"" << gcs_content_path << "/$clean_path\"\n";
  script_file << "  gsutil cp \"$file\" \"$dest_path\"\n";
  script_file << "done\n\n";

  // List all files to verify upload
  script_file << "# List all uploaded files\n";
  script_file << "echo 'Listing all uploaded files:'\n";
  script_file << "gsutil ls -r \"" << gcs_content_path << "/\"\n";

  script_file.close();

  // Make script executable
  std::string chmod_cmd = "chmod +x " + script_path;
  system(chmod_cmd.c_str());

  // Execute the script
  log_to_file("Running GCS upload script: " + script_path);
  std::string exec_cmd = "bash " + script_path + " 2>&1";
  std::string result = exec_command(exec_cmd);
  log_to_file("GCS upload script output: " + result);

  // Clean up
  std::string rm_cmd = "rm " + script_path;
  system(rm_cmd.c_str());

  // Check if upload was successful
  std::string verify_cmd = "gsutil ls " + gcs_content_path + " 2>/dev/null";
  std::string verify_result = exec_command(verify_cmd);

  if (!verify_result.empty()) {
    log_to_file("Verified uploads to " + gcs_content_path);
    return true;
  } else {
    log_to_file("Failed to verify uploads to " + gcs_content_path);
    return false;
  }
}

// Copies article files to both local storage and GCS
bool store_article_files(const fs::path &article_dir, int content_id) {
  log_to_file("Storing article files from " + article_dir.string() +
              " for content ID " + std::to_string(content_id));

  // Setup local storage path
  fs::path local_dest = STORAGE_ROOT + std::to_string(content_id);

  try {
    // Create local directory
    fs::create_directories(local_dest);
    log_to_file("Created local directory: " + local_dest.string());

    // Track if we have any files to copy
    bool has_files = false;

    // Copy files to local storage
    for (const auto &entry : fs::recursive_directory_iterator(article_dir)) {
      if (entry.is_directory()) {
        // Create the directory in the destination
        fs::path rel_path = fs::relative(entry.path(), article_dir);
        fs::path dest_dir = local_dest / rel_path;
        fs::create_directories(dest_dir);
        log_to_file("Created directory: " + dest_dir.string());
        continue;
      }

      // Skip metadata.txt
      if (entry.path().filename() == "metadata.txt") {
        continue;
      }

      has_files = true;
      fs::path rel_path = fs::relative(entry.path(), article_dir);
      fs::path dest_path = local_dest / rel_path;

      // Create parent directories if they don't exist
      fs::create_directories(dest_path.parent_path());

      // Copy the file
      log_to_file("Copying file: " + entry.path().string() + " to " +
                  dest_path.string());
      fs::copy_file(entry.path(), dest_path,
                    fs::copy_options::overwrite_existing);
      log_to_file("Copied to local path: " + dest_path.string());

      // Extract file type (extension without dot)
      std::string file_type = entry.path().extension().string();
      if (!file_type.empty()) {
        file_type = file_type.substr(1); // Remove leading dot
      } else {
        file_type = "bin";
      }

      // Store file reference in database
      log_to_file("Storing reference in database: " + rel_path.string() +
                  " with type: " + file_type);
      store_file_reference(content_id, file_type, rel_path.string());
    }

    if (!has_files) {
      log_to_file("No files to copy in article directory");
      return true;
    }

    // Upload to GCS
    bool gcs_success = recursive_upload(article_dir, content_id);
    log_to_file("GCS upload " +
                std::string(gcs_success ? "successful" : "failed"));

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

  // First try to get path from query parameters
  auto it = req.query_params.find("path");
  if (it != req.query_params.end()) {
    article_path = it->second;
    log_to_file("Using path from query parameter: " + article_path);
  }
  // If not found in query params, try using the request body
  else if (!req.body.empty()) {
    article_path = req.body;
    log_to_file("Using path from request body: " + article_path);
  }
  // If neither is available, return an error
  else {
    log_to_file("No path provided in query parameters or request body");
    res.send(400, "Missing path parameter. Provide it either as a query "
                  "parameter '?path=' or in the request body.");
    return;
  }

  // Check if the article path exists
  fs::path meta_file = fs::path(article_path) / "metadata.txt";
  if (!fs::exists(meta_file)) {
    log_to_file("Metadata file not found at: " + meta_file.string());
    res.send(400, "Missing metadata.txt at path: " + article_path);
    return;
  }

  // Parse metadata
  auto metadata = parse_metadata(meta_file);
  int content_id = -1;

  // Update database with metadata
  if (!update_article_metadata(metadata, content_id)) {
    log_to_file("Database update failed for article at: " + article_path);
    res.send(500, "Database update failed");
    return;
  }

  // Store files (local & GCS)
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
