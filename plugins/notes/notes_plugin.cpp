// SPDX-License-Identifier: AGPL-3.0-only
//
// Notes plugin: persistent SQLite + FTS5 scratchpad, ported from the
// mcp_notes_server example.
//
// Configuration comes from the environment:
//   MCP_NOTES_DB        database path (default:
//   $XDG_STATE_HOME/mcp-notes/notes.db) MCP_NOTES_READONLY  "1"/"true" blocks
//   writes and deletes
//
// Destructive deletes use host elicitation (HostInterface::elicit), which the
// server routes to the session that invoked the tool.

#include "plugin.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

  namespace fs = std::filesystem;

  // =====================================================================
  // SQLite RAII shell
  // =====================================================================

  struct SqliteError : public std::runtime_error
  {
    using std::runtime_error::runtime_error;
  };

  class Stmt
  {
  public:
    Stmt(void) = default;
    Stmt(sqlite3* db, std::string_view sql)
    {
      if(sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()),
                            &m_raw, nullptr)
         != SQLITE_OK)
      {
        throw SqliteError(std::string{ "sqlite prepare failed: " }
                          + sqlite3_errmsg(db));
      }
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& o) noexcept : m_raw(o.m_raw) { o.m_raw = nullptr; }
    Stmt& operator=(Stmt&& o) noexcept
    {
      if(this != &o)
      {
        if(m_raw)
        {
          sqlite3_finalize(m_raw);
        }
        m_raw = o.m_raw;
        o.m_raw = nullptr;
      }
      return *this;
    }
    ~Stmt(void)
    {
      if(m_raw)
      {
        sqlite3_finalize(m_raw);
      }
    }

    sqlite3_stmt* get(void) const noexcept { return m_raw; }
    sqlite3_stmt* operator->(void) const noexcept { return m_raw; }

    void bind(int i, std::string_view s)
    {
      if(sqlite3_bind_text(m_raw, i, s.data(), static_cast<int>(s.size()),
                           SQLITE_TRANSIENT)
         != SQLITE_OK)
      {
        throw SqliteError("sqlite bind failed");
      }
    }
    void bind(int i, std::int64_t v)
    {
      if(sqlite3_bind_int64(m_raw, i, v) != SQLITE_OK)
      {
        throw SqliteError("sqlite bind failed");
      }
    }
    void bind_null(int i)
    {
      if(sqlite3_bind_null(m_raw, i) != SQLITE_OK)
      {
        throw SqliteError("sqlite bind failed");
      }
    }

    int step(void) { return sqlite3_step(m_raw); }

    std::string text(int col) const
    {
      const auto* p = sqlite3_column_text(m_raw, col);
      const auto n = sqlite3_column_bytes(m_raw, col);
      return p ? std::string{ reinterpret_cast<const char*>(p),
                              static_cast<std::size_t>(n) }
               : std::string{};
    }
    std::int64_t i64(int col) const
    {
      return sqlite3_column_int64(m_raw, col);
    }
    bool is_null(int col) const
    {
      return sqlite3_column_type(m_raw, col) == SQLITE_NULL;
    }

  private:
    sqlite3_stmt* m_raw = nullptr;
  };

  class Db
  {
  public:
    Db(const std::string& path, bool readonly)
    {
      const int flags = readonly
                            ? (SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX)
                            : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                               | SQLITE_OPEN_FULLMUTEX);
      if(sqlite3_open_v2(path.c_str(), &m_handle, flags, nullptr) != SQLITE_OK)
      {
        const std::string msg
            = m_handle ? sqlite3_errmsg(m_handle) : "unknown sqlite failure";
        if(m_handle)
        {
          sqlite3_close(m_handle);
        }
        m_handle = nullptr;
        throw SqliteError(std::string{ "sqlite open failed: " } + msg);
      }
      if(!readonly)
      {
        (void)exec("PRAGMA journal_mode = WAL");
        (void)exec("PRAGMA synchronous  = NORMAL");
        (void)exec("PRAGMA foreign_keys = ON");
      }
    }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db(void)
    {
      if(m_handle)
      {
        sqlite3_close(m_handle);
      }
    }

    sqlite3* handle(void) const noexcept { return m_handle; }

    void exec(std::string_view sql)
    {
      char* err = nullptr;
      if(sqlite3_exec(m_handle, std::string{ sql }.c_str(), nullptr, nullptr,
                      &err)
         != SQLITE_OK)
      {
        const std::string msg = err ? err : "unknown error";
        if(err)
        {
          sqlite3_free(err);
        }
        throw SqliteError(std::string{ "sqlite exec failed: " } + msg);
      }
    }

    Stmt prepare(std::string_view sql) { return Stmt{ m_handle, sql }; }

  private:
    sqlite3* m_handle = nullptr;
  };

  constexpr std::string_view kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS notes (
    rowid      INTEGER PRIMARY KEY,
    key        TEXT UNIQUE NOT NULL,
    value      TEXT NOT NULL,
    tags       TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS notes_updated_at
  ON notes (updated_at DESC);

CREATE VIRTUAL TABLE IF NOT EXISTS notes_fts USING fts5 (
    key, value, tags,
    tokenize = 'porter unicode61'
);

CREATE TRIGGER IF NOT EXISTS notes_ai AFTER INSERT ON notes BEGIN
    INSERT INTO notes_fts(rowid, key, value, tags)
    VALUES (new.rowid, new.key, new.value, COALESCE(new.tags, ''));
END;
CREATE TRIGGER IF NOT EXISTS notes_au AFTER UPDATE ON notes BEGIN
    DELETE FROM notes_fts WHERE rowid = old.rowid;
    INSERT INTO notes_fts(rowid, key, value, tags)
    VALUES (new.rowid, new.key, new.value, COALESCE(new.tags, ''));
END;
CREATE TRIGGER IF NOT EXISTS notes_ad AFTER DELETE ON notes BEGIN
    DELETE FROM notes_fts WHERE rowid = old.rowid;
END;
)sql";

  // =====================================================================
  // Helpers
  // =====================================================================

  std::int64_t now_unix_seconds(void)
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::string default_db_path(void)
  {
    const char* state = std::getenv("XDG_STATE_HOME");
    fs::path base;
    if(state && *state)
    {
      base = state;
    }
    else if(const char* home = std::getenv("HOME"); home && *home)
    {
      base = fs::path{ home } / ".local" / "state";
    }
    else
    {
      base = fs::current_path();
    }
    base /= "mcp-notes";
    std::error_code ec;
    fs::create_directories(base, ec);
    return (base / "notes.db").string();
  }

  QJsonValue jsv(const std::string& s)
  {
    return QJsonValue(QString::fromStdString(s));
  }
  QJsonValue jiv(std::int64_t v) { return QJsonValue(static_cast<qint64>(v)); }

  bool env_flag(const char* name)
  {
    const char* value = std::getenv(name);
    if(value == nullptr)
    {
      return false;
    }
    const std::string s{ value };
    return s == "1" || s == "true" || s == "TRUE" || s == "yes";
  }

  // =====================================================================
  // Plugin
  // =====================================================================

  class NotesPlugin : public mcp::PluginInterface
  {
    Q_OBJECT
    Q_CLASSINFO("id", "notes")
    Q_CLASSINFO("version", "1.0.0")
    Q_CLASSINFO("description",
                "Persistent SQLite-backed scratchpad with FTS5 search.")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "notes_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

  public:
    explicit NotesPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent)
    {
    }

    void initialize(const mcp::HostInterface& host) override
    {
      m_host = host;

      const char* db_env = std::getenv("MCP_NOTES_DB");
      m_db_path = (db_env != nullptr && *db_env != '\0')
                      ? std::string{ db_env }
                      : default_db_path();
      m_readonly = env_flag("MCP_NOTES_READONLY");

      m_db = std::make_unique<Db>(m_db_path, m_readonly);
      if(!m_readonly)
      {
        m_db->exec(kSchemaSql);
      }

      registerPublicSlots();
    }

    void shutdown(void) override { m_db.reset(); }

  public slots:
    QString write(QString key, QString value, QString tags)
    {
      if(m_readonly)
      {
        throw std::runtime_error("notes/write: server is read-only");
      }
      const std::string k = key.toStdString();
      const std::string v = value.toStdString();
      const std::string t = tags.toStdString();
      if(k.empty())
      {
        throw std::runtime_error("notes/write: `key` is required");
      }
      if(k.size() > 256)
      {
        throw std::runtime_error("notes/write: key too long (>256 chars)");
      }
      if(v.empty())
      {
        throw std::runtime_error("notes/write: `value` is required");
      }

      const auto now = now_unix_seconds();
      auto stmt = m_db->prepare(R"sql(
            INSERT INTO notes(key, value, tags, created_at, updated_at)
            VALUES (?1, ?2, ?3, ?4, ?4)
            ON CONFLICT(key) DO UPDATE SET
                value      = excluded.value,
                tags       = excluded.tags,
                updated_at = excluded.updated_at
        )sql");
      stmt.bind(1, k);
      stmt.bind(2, v);
      if(t.empty())
      {
        stmt.bind_null(3);
      }
      else
      {
        stmt.bind(3, t);
      }
      stmt.bind(4, now);
      if(stmt.step() != SQLITE_DONE)
      {
        throw SqliteError(std::string{ "notes/write: " }
                          + sqlite3_errmsg(m_db->handle()));
      }
      return QStringLiteral("ok: stored under key \"%1\"")
          .arg(QString::fromStdString(k));
    }

    QVariant read(QString key)
    {
      const std::string k = key.toStdString();
      if(k.empty())
      {
        throw std::runtime_error("notes/read: `key` is required");
      }
      auto stmt = m_db->prepare("SELECT value, tags, created_at, updated_at "
                                "FROM notes WHERE key = ?1");
      stmt.bind(1, k);
      const int rc = stmt.step();
      if(rc == SQLITE_DONE)
      {
        throw std::runtime_error("notes/read: no note with key \"" + k + "\"");
      }
      if(rc != SQLITE_ROW)
      {
        throw SqliteError(std::string{ "notes/read: " }
                          + sqlite3_errmsg(m_db->handle()));
      }
      return QJsonObject{
        { "key", jsv(k) },
        { "value", jsv(stmt.text(0)) },
        { "tags", jsv(stmt.is_null(1) ? "" : stmt.text(1)) },
        { "createdAt", jiv(stmt.i64(2)) },
        { "updatedAt", jiv(stmt.i64(3)) },
      };
    }

    QVariant list(QString tag, QString prefix, int limit)
    {
      const std::string tag_s = tag.toStdString();
      const std::string prefix_s = prefix.toStdString();
      std::int64_t lim = limit <= 0 ? 50 : limit;
      lim = std::clamp<std::int64_t>(lim, 1, 1000);

      std::string sql = "SELECT key, tags, updated_at FROM notes WHERE 1=1";
      QList<QString> binds;
      if(!prefix_s.empty())
      {
        sql += " AND key LIKE ?";
        binds.push_back(QString::fromStdString(prefix_s) + QLatin1Char('%'));
      }
      if(!tag_s.empty())
      {
        sql += " AND (',' || tags || ',') LIKE ?";
        binds.push_back(QStringLiteral("%,") + QString::fromStdString(tag_s)
                        + QStringLiteral(",%"));
      }
      sql += " ORDER BY updated_at DESC LIMIT ?";

      auto stmt = m_db->prepare(sql);
      int idx = 1;
      for(const auto& b : binds)
      {
        stmt.bind(idx++, b.toStdString());
      }
      stmt.bind(idx, lim);

      QJsonArray items;
      while(stmt.step() == SQLITE_ROW)
      {
        items.append(QJsonObject{
            { "key", jsv(stmt.text(0)) },
            { "tags", jsv(stmt.is_null(1) ? "" : stmt.text(1)) },
            { "updatedAt", jiv(stmt.i64(2)) },
        });
      }
      return QJsonObject{ { "notes", items } };
    }

    QVariant search(QString query, int limit)
    {
      const std::string q = query.toStdString();
      if(q.empty())
      {
        throw std::runtime_error("notes/search: `query` is required");
      }
      std::int64_t lim = limit <= 0 ? 25 : limit;
      lim = std::clamp<std::int64_t>(lim, 1, 200);

      std::string fts_query = q;
      if(fts_query.find_first_of("\"-+*()") != std::string::npos)
      {
        std::string wrapped = "\"";
        for(char c : q)
        {
          if(c == '"')
          {
            wrapped += '"';
          }
          wrapped += c;
        }
        wrapped += '"';
        fts_query = std::move(wrapped);
      }

      auto stmt = m_db->prepare(R"sql(
            SELECT n.key, n.value, n.tags, n.updated_at,
                   snippet(notes_fts, 1, '<<', '>>', '…', 12) AS snip,
                   bm25(notes_fts)                            AS score
            FROM notes_fts
            JOIN notes n ON n.rowid = notes_fts.rowid
            WHERE notes_fts MATCH ?1
            ORDER BY score
            LIMIT ?2
        )sql");
      stmt.bind(1, fts_query);
      stmt.bind(2, lim);

      QJsonArray hits;
      while(stmt.step() == SQLITE_ROW)
      {
        hits.append(QJsonObject{
            { "key", jsv(stmt.text(0)) },
            { "value", jsv(stmt.text(1)) },
            { "tags", jsv(stmt.is_null(2) ? "" : stmt.text(2)) },
            { "updatedAt", jiv(stmt.i64(3)) },
            { "snippet", jsv(stmt.text(4)) },
        });
      }
      return QJsonObject{ { "matches", hits } };
    }

    QString remove(QString key, bool confirm)
    {
      if(m_readonly)
      {
        throw std::runtime_error("notes/delete: server is read-only");
      }
      const std::string k = key.toStdString();
      if(k.empty())
      {
        throw std::runtime_error("notes/delete: `key` is required");
      }
      const bool is_glob = k.find_first_of("*%") != std::string::npos;

      if(is_glob || !confirm)
      {
        const mcp::ElicitRequestParams params =
                mcp::ElicitFormRequestParams{
                    .message = QStringLiteral("Delete note(s) matching key "
                                              "\"") +
                               QString::fromStdString(k) +
                               QStringLiteral("\"? This cannot be undone."),
                    .requested_schema =
                        QJsonObject{
                            {"type", "object"},
                            {"properties",
                             QJsonObject{
                                 {"confirm",
                                  QJsonObject{
                                      {"type", "boolean"},
                                      {"description",
                                       "Set true to delete; false / decline "
                                       "to abort."}}},
                             }},
                            {"required", QJsonArray{"confirm"}},
                        },
                };
        const QVariant encoded = m_host.callFunc(
            QStringLiteral("QVariant mcp::Host::elicit(QVariant)"),
            { QVariant::fromValue(mcp::to_json(params)) });
        const mcp::ElicitResult result
            = mcp::from_json<mcp::ElicitResult>(encoded.value<QJsonValue>());
        const bool accepted = result.action == mcp::ElicitAction::accept
                              && !result.content.isNull()
                              && result.content.toObject()
                                     .value(QStringLiteral("confirm"))
                                     .toBool();
        if(!accepted)
        {
          throw std::runtime_error("notes/delete: cancelled by user");
        }
      }

      std::string sql = is_glob ? "DELETE FROM notes WHERE key LIKE ?1"
                                : "DELETE FROM notes WHERE key = ?1";
      auto stmt = m_db->prepare(sql);
      std::string bind_key = k;
      for(char& c : bind_key)
      {
        if(c == '*')
        {
          c = '%';
        }
      }
      stmt.bind(1, bind_key);
      if(stmt.step() != SQLITE_DONE)
      {
        throw SqliteError(std::string{ "notes/delete: " }
                          + sqlite3_errmsg(m_db->handle()));
      }
      const int n = sqlite3_changes(m_db->handle());
      return QStringLiteral("%1 note(s) deleted").arg(n);
    }

    QVariant info(void)
    {
      std::int64_t row_count = 0;
      {
        auto stmt = m_db->prepare("SELECT COUNT(*) FROM notes");
        if(stmt.step() == SQLITE_ROW)
        {
          row_count = stmt.i64(0);
        }
      }
      return QJsonObject{
        { "path", jsv(m_db_path) },
        { "readOnly", m_readonly },
        { "noteCount", jiv(row_count) },
        { "sqliteVersion", jsv(sqlite3_libversion()) },
      };
    }

    // Reader for the notes://all resource.
    QString readAll(QString uri)
    {
      (void)uri;
      QJsonArray items;
      auto stmt = m_db->prepare(
          "SELECT key, value, tags, updated_at FROM notes ORDER BY key ASC");
      while(stmt.step() == SQLITE_ROW)
      {
        items.append(QJsonObject{
            { "key", jsv(stmt.text(0)) },
            { "value", jsv(stmt.text(1)) },
            { "tags", jsv(stmt.is_null(2) ? "" : stmt.text(2)) },
            { "updatedAt", jiv(stmt.i64(3)) },
        });
      }
      const QJsonObject doc{ { "notes", items } };
      return QString::fromStdString(
          QJsonDocument(doc).toJson(QJsonDocument::Indented).toStdString());
    }

  private:
    std::unique_ptr<Db> m_db;
    bool m_readonly = false;
    std::string m_db_path;
  };

} // namespace

#include "notes_plugin.moc"