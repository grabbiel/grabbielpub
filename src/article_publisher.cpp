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

// Parses metadata.txt (simple key = value format)
std::unordered_map<std::string, std::string>
parse_metadata(const fs::path &metadata_path) {
  std::unordered_map<std::string, std::string> metadata;
  std::ifstream infile(metadata_path);
  std::string line;
  while (std::getline(infile, line)) {
    auto delimiter_pos = line.find('=');
    if (delimiter_pos != std::string::npos) {
      std::string key = line.substr(0, delimiter_pos);
      std::string value = line.substr(delimiter_pos + 1);
      metadata[key] = value;
    }
  }
  return metadata;
}

// Copies article files to persistent storage
bool store_article_files(const fs::path &article_dir, int content_id) {
  fs::path dest = STORAGE_ROOT + std::to_string(content_id);
  try {
    fs::create_directories(dest);
    for (const auto &entry : fs::directory_iterator(article_dir)) {
      if (entry.path().filename() == "metadata.txt")
        continue;
      fs::copy(entry.path(), dest / entry.path().filename(),
               fs::copy_options::overwrite_existing);
    }
    return true;
  } catch (const std::exception &e) {
    std::cerr << "Error storing article files: " << e.what() << std::endl;
    return false;
  }
}

// Inserts or updates content_blocks and articles
bool update_article_metadata(
    const std::unordered_map<std::string, std::string> &meta, int &content_id) {
  sqlite3 *db;
  if (sqlite3_open(DB_PATH.c_str(), &db) != SQLITE_OK)
    return false;

  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  const std::string title = meta.at("title");
  const std::string slug = meta.at("slug");
  const std::string body_md = meta.at("body_markdown");
  const std::string site_id = meta.at("site_id");

  sqlite3_stmt *stmt;
  const char *select_sql =
      "SELECT id FROM content_blocks WHERE url_slug = ? AND site_id = ?";
  sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, slug.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, site_id.c_str(), -1, SQLITE_STATIC);

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    content_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    const char *update_sql =
        "UPDATE articles SET body_markdown = ?, last_edited = "
        "CURRENT_TIMESTAMP WHERE content_id = ?";
    sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, body_md.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, content_id);
    sqlite3_step(stmt);
  } else {
    sqlite3_finalize(stmt);

    const char *insert_cb_sql =
        "INSERT INTO content_blocks (title, url_slug, type_id, site_id, "
        "language) VALUES (?, ?, 1, ?, 'en');";
    sqlite3_prepare_v2(db, insert_cb_sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, slug.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, site_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    content_id = (int)sqlite3_last_insert_rowid(db);

    const char *insert_article_sql =
        "INSERT INTO articles (content_id, body_markdown) VALUES (?, ?);";
    sqlite3_prepare_v2(db, insert_article_sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, content_id);
    sqlite3_bind_text(stmt, 2, body_md.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
  }

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
  sqlite3_close(db);
  return true;
}

void handle_publish_request(const HttpRequest &req, HttpResponse &res) {
  const std::string article_path =
      req.query_params.at("path"); // Path to the article directory
  fs::path meta_file = fs::path(article_path) / "metadata.txt";
  if (!fs::exists(meta_file)) {
    res.send(400, "Missing metadata.txt");
    return;
  }

  auto metadata = parse_metadata(meta_file);
  int content_id = -1;
  if (!update_article_metadata(metadata, content_id)) {
    res.send(500, "Database update failed");
    return;
  }

  if (!store_article_files(article_path, content_id)) {
    res.send(500, "File copy failed");
    return;
  }

  res.send(200, "Article published with ID: " + std::to_string(content_id));
}

int main() {
  HttpServer server(8082); // localhost only
  server.route("/publish", handle_publish_request);
  server.run();
  return 0;
}
