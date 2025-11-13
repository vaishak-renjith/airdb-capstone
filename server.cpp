#include "crow.h"
#include "airdb.h"
#include "crow/json.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <cstdint>

#ifdef _WIN32
#include <cstdlib>
static int read_port() {
    char* buf = nullptr; size_t len = 0;
    int port = 18080;
    if (_dupenv_s(&buf, &len, "PORT") == 0 && buf) {
        port = std::atoi(buf);
        free(buf);
    }
    return port;
}
#else
#include <cstdlib>
static int read_port() {
    const char* p = std::getenv("PORT");
    return p ? std::atoi(p) : 18080;
}
#endif


// ---------- helpers ----------
static bool read_file_strict(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::string read_file(const std::string& path) {
    std::string data;
    if (!read_file_strict(path, data)) return {};
    return data;
}

static std::array<uint32_t, 256> make_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            if (c & 1) c = 0xEDB88320u ^ (c >> 1);
            else c >>= 1;
        }
        table[i] = c;
    }
    return table;
}

static uint32_t crc32(const std::string& data) {
    static const auto table = make_crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void write_le16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

static void write_le32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

struct ZipCentralEntry {
    std::string name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
};

// Builds a minimal ZIP archive (stored entries only) directly in-memory so the
// server can stream an attachment without touching disk.
static std::string build_zip_from_files(const std::vector<std::string>& files) {
    std::string zip;
    std::vector<ZipCentralEntry> central_entries;
    central_entries.reserve(files.size());

    for (const auto& path : files) {
        std::string data;
        if (!read_file_strict(path, data)) {
            return {};
        }

        const uint32_t offset = static_cast<uint32_t>(zip.size());
        const uint32_t crc = crc32(data);
        const uint32_t size = static_cast<uint32_t>(data.size());
        const uint16_t name_len = static_cast<uint16_t>(path.size());

        write_le32(zip, 0x04034b50);
        write_le16(zip, 20);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le16(zip, 0);
        write_le32(zip, crc);
        write_le32(zip, size);
        write_le32(zip, size);
        write_le16(zip, name_len);
        write_le16(zip, 0);
        zip.append(path);
        zip.append(data);

        central_entries.push_back({ path, crc, size, offset });
    }

    const uint32_t central_dir_offset = static_cast<uint32_t>(zip.size());
    std::string central_dir;
    for (const auto& entry : central_entries) {
        write_le32(central_dir, 0x02014b50);
        write_le16(central_dir, 20);
        write_le16(central_dir, 20);
        write_le16(central_dir, 0);
        write_le16(central_dir, 0);
        write_le16(central_dir, 0);
        write_le16(central_dir, 0);
        write_le32(central_dir, entry.crc);
        write_le32(central_dir, entry.size);
        write_le32(central_dir, entry.size);
        write_le16(central_dir, static_cast<uint16_t>(entry.name.size()));
        write_le16(central_dir, 0);
        write_le16(central_dir, 0);
        write_le16(central_dir, 0);
        write_le16(central_dir, 0);
        write_le32(central_dir, 0);
        write_le32(central_dir, entry.offset);
        central_dir.append(entry.name);
    }

    zip.append(central_dir);

    write_le32(zip, 0x06054b50);
    write_le16(zip, 0);
    write_le16(zip, 0);
    write_le16(zip, static_cast<uint16_t>(central_entries.size()));
    write_le16(zip, static_cast<uint16_t>(central_entries.size()));
    write_le32(zip, static_cast<uint32_t>(central_dir.size()));
    write_le32(zip, central_dir_offset);
    write_le16(zip, 0);

    return zip;
}

static crow::response not_found(const std::string& msg = "Not found") {
    return crow::response(404, msg);
}

// CSV escape helper: wrap fields with commas/quotes/newlines in double quotes,
// and double-up any embedded quotes per RFC 4180.
static std::string csv_escape(const std::string& s) {
    bool need_quotes = s.find_first_of(",\"\n\r") != std::string::npos;
    if (!need_quotes) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}


// ---------- main ----------
int main() {
    crow::SimpleApp app;
    AirTravelDB db;

    // Load data (adjust paths if needed)
    db.LoadAirlinesCSV("airlines.dat");
    db.LoadAirportsCSV("airports.dat");
    db.LoadRoutesCSV("routes.dat");

    // ---------- Static files ----------
    CROW_ROUTE(app, "/")
        ([] {
        auto html = read_file("index.html");
        if (html.empty()) return crow::response(500, "index.html missing");
        crow::response res{ html };
        res.add_header("Content-Type", "text/html; charset=utf-8");
        return res;
            });

    CROW_ROUTE(app, "/static/style.css")
        ([] {
        auto css = read_file("style.css");
        if (css.empty()) return crow::response(404);
        crow::response res{ css };
        res.add_header("Content-Type", "text/css; charset=utf-8");
        return res;
            });

    CROW_ROUTE(app, "/static/app.js")
        ([] {
        auto js = read_file("app.js");
        if (js.empty()) return crow::response(404);
        crow::response res{ js };
        res.add_header("Content-Type", "application/javascript; charset=utf-8");
        return res;
            });

    CROW_ROUTE(app, "/download/source")
        ([] {
        const std::vector<std::string> files = {
            "server.cpp",
            "airdp.cpp",
            "airdb.h",
            "index.html",
            "style.css",
            "app.js"
        };
        auto zipped = build_zip_from_files(files);
        if (zipped.empty()) {
            return crow::response(500, "Source zip unavailable");
        }
        crow::response res{ zipped };
        res.add_header("Content-Type", "application/zip");
        res.add_header("Content-Disposition", "attachment; filename=\"air-travel-source.zip\"");
        return res;
            });

    // ---------- Section III.1: Individual Entity Retrieval ----------

    // 1.1: Airline lookup by IATA (flexible: also supports ICAO and name search)
    CROW_ROUTE(app, "/airline/<string>")
        ([&db](const std::string& term) {
        // Try IATA first
        if (auto a = db.GetAirlineByIATA(term)) {
            return crow::response(a->toJSON());
        }
        // Try ICAO if term is 3 characters
        if (term.size() == 3) {
            if (auto a = db.GetAirlineByICAO(term)) {
                return crow::response(a->toJSON());
            }
        }
        // Fallback: search by name (case-insensitive)
        auto all = db.GetAllAirlines();
        std::string ql = term;
        std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
        for (const auto& a : all) {
            std::string name = a.name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find(ql) != std::string::npos) {
                crow::json::wvalue j;
                j["id"] = a.id; j["name"] = a.name; j["alias"] = a.alias;
                j["iata"] = a.iata; j["icao"] = a.icao;
                j["callsign"] = a.callsign; j["country"] = a.country; j["active"] = a.active;
                return crow::response(j);
            }
        }
        return not_found("Airline not found");
            });

    // Explicit ICAO endpoint for autocomplete
    CROW_ROUTE(app, "/api/airline/by-icao/<string>")
        ([&db](const std::string& icao) {
        if (auto a = db.GetAirlineByICAO(icao)) {
            return crow::response(a->toJSON());
        }
        return crow::response(404);
            });

    // Airline suggestions for autocomplete
    CROW_ROUTE(app, "/api/airlines/suggest")
        ([&db](const crow::request& req) {
        auto qit = req.url_params.get("q");
        int limit = 10;
        crow::json::wvalue out;
        out["items"] = crow::json::wvalue::list();
        if (!qit || std::string(qit).empty()) return crow::response(out);

        std::string q = qit;
        std::string ql = q; std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

        auto all = db.GetAllAirlines();
        struct Item { std::string name, iata, icao; };
        std::vector<Item> items;
        items.reserve(all.size());
        for (const auto& a : all) {
            std::string nm = a.name, ia = a.iata, ic = a.icao;
            std::string nm_l = nm; std::transform(nm_l.begin(), nm_l.end(), nm_l.begin(), ::tolower);
            std::string ia_l = ia; std::transform(ia_l.begin(), ia_l.end(), ia_l.begin(), ::tolower);
            std::string ic_l = ic; std::transform(ic_l.begin(), ic_l.end(), ic_l.begin(), ::tolower);
            if (nm_l.find(ql) != std::string::npos || ia_l.find(ql) != std::string::npos || ic_l.find(ql) != std::string::npos) {
                items.push_back({ nm, ia, ic });
            }
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.name < b.name;
            });
        if ((int)items.size() > limit) items.resize(limit);

        for (size_t i = 0; i < items.size(); ++i) {
            crow::json::wvalue it;
            it["name"] = items[i].name;
            it["iata"] = items[i].iata;
            it["icao"] = items[i].icao;
            out["items"][i] = std::move(it);
        }
        return crow::response(out);
            });

    // 1.2: Airport lookup by IATA (flexible: also supports ID, ICAO, and name/city search)
    CROW_ROUTE(app, "/airport/<string>")
        ([&db](const std::string& term) {
        // Try numeric ID
        if (!term.empty() && std::all_of(term.begin(), term.end(), ::isdigit)) {
            int id = std::stoi(term);
            if (auto ap = db.GetAirportByID(id)) return crow::response(ap->toJSON());
        }
        // Try IATA
        if (auto ap = db.GetAirportByIATA(term)) return crow::response(ap->toJSON());
        // Try ICAO if 4 characters
        if (term.size() == 4) {
            if (auto ap = db.GetAirportByICAO(term)) return crow::response(ap->toJSON());
        }
        // Fallback: search by name or city
        auto all = db.GetAllAirports();
        std::string ql = term; std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
        for (const auto& ap : all) {
            std::string nm = ap.name, ct = ap.city;
            std::transform(nm.begin(), nm.end(), nm.begin(), ::tolower);
            std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
            if (nm.find(ql) != std::string::npos || ct.find(ql) != std::string::npos) {
                crow::json::wvalue j = ap.toJSON();
                return crow::response(j);
            }
        }
        return not_found("Airport not found");
            });

    // Airport suggestions for autocomplete
    CROW_ROUTE(app, "/api/airports/suggest")
        ([&db](const crow::request& req) {
        auto qit = req.url_params.get("q");
        int limit = 10;
        crow::json::wvalue out;
        out["items"] = crow::json::wvalue::list();
        if (!qit || std::string(qit).empty()) return crow::response(out);

        std::string q = qit;
        std::string ql = q; std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

        auto all = db.GetAllAirports();
        struct Item { std::string name, city, country, iata, icao; };
        std::vector<Item> items;
        items.reserve(all.size());
        for (const auto& ap : all) {
            std::string name = ap.name, city = ap.city, country = ap.country, iata = ap.iata, icao = ap.icao;
            std::string n = name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            std::string c = city; std::transform(c.begin(), c.end(), c.begin(), ::tolower);
            std::string co = country; std::transform(co.begin(), co.end(), co.begin(), ::tolower);
            std::string ia = iata; std::transform(ia.begin(), ia.end(), ia.begin(), ::tolower);
            std::string ic = icao; std::transform(ic.begin(), ic.end(), ic.begin(), ::tolower);
            if (n.find(ql) != std::string::npos || c.find(ql) != std::string::npos || co.find(ql) != std::string::npos
                || ia.find(ql) != std::string::npos || ic.find(ql) != std::string::npos) {
                items.push_back({ name, city, country, iata, icao });
            }
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.name < b.name;
            });
        if ((int)items.size() > limit) items.resize(limit);

        for (size_t i = 0; i < items.size(); ++i) {
            crow::json::wvalue it;
            it["name"] = items[i].name; it["city"] = items[i].city; it["country"] = items[i].country;
            it["iata"] = items[i].iata; it["icao"] = items[i].icao;
            out["items"][i] = std::move(it);
        }
        return crow::response(out);
            });

    // ---------- Section III.2.1.a: Airline -> Airports Report (ordered by # routes) ----------

    // JSON version
    CROW_ROUTE(app, "/report/airline/<string>/airports-by-routes.json")
        ([&db](const std::string& airline_iata) {
        auto routes = db.SearchRoutes(airline_iata);
        std::unordered_map<std::string, int> counts;
        for (const auto& r : routes) {
            if (r.airline_iata != airline_iata) continue;
            ++counts[r.src_iata];
            ++counts[r.dst_iata];
        }

        struct Row { std::string iata; std::string name; int n; };
        std::vector<Row> rows; rows.reserve(counts.size());
        for (auto& kv : counts) {
            auto ap = db.GetAirportByIATA(kv.first);
            rows.push_back({ kv.first, ap ? ap->name : std::string{}, kv.second });
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.n != b.n) return a.n > b.n;
            return a.iata < b.iata;
            });

        std::string airline_name;
        if (auto a = db.GetAirlineByIATA(airline_iata)) airline_name = a->name;

        crow::json::wvalue out;
        out["airline_iata"] = airline_iata;
        out["airline_name"] = airline_name;
        out["items"] = crow::json::wvalue::list();
        unsigned idx = 0;
        for (auto& r : rows) {
            crow::json::wvalue it;
            it["airport_iata"] = r.iata;
            it["airport_name"] = r.name;
            it["routes_count"] = r.n;
            out["items"][idx++] = std::move(it);
        }
        return crow::response(out);
            });

    // CSV version
    CROW_ROUTE(app, "/report/airline/<string>/airports-by-routes.csv")
        ([&db](const std::string& airline_iata) {
        auto routes = db.SearchRoutes(airline_iata);
        std::unordered_map<std::string, int> counts;
        for (const auto& r : routes) {
            if (r.airline_iata != airline_iata) continue;
            ++counts[r.src_iata];
            ++counts[r.dst_iata];
        }
        struct Row { std::string iata; std::string name; int n; };
        std::vector<Row> rows; rows.reserve(counts.size());
        for (auto& kv : counts) {
            auto ap = db.GetAirportByIATA(kv.first);
            rows.push_back({ kv.first, ap ? ap->name : std::string{}, kv.second });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.n != b.n) return a.n > b.n;
            return a.iata < b.iata;
            });

        std::ostringstream ss;
        ss << "airport_iata,airport_name,routes_count\r\n";
        for (auto& r : rows) {
            ss << csv_escape(r.iata) << ','
                << csv_escape(r.name) << ','
                << r.n << "\r\n";
        }
        crow::response res{ ss.str() };
        res.add_header("Content-Type", "text/csv; charset=utf-8");
        res.add_header("Content-Disposition", "attachment; filename=\"airline_" + airline_iata + "_airports.csv\"");
        return res;
            });

    // ---------- Section III.2.1.b: Airport -> Airlines Report (ordered by # routes) ----------

    // JSON version
    CROW_ROUTE(app, "/report/airport/<string>/airlines-by-routes.json")
        ([&db](const std::string& airport_iata) {
        auto routes = db.SearchRoutes(airport_iata);
        std::unordered_map<std::string, int> counts;
        for (const auto& r : routes) {
            if (r.src_iata == airport_iata || r.dst_iata == airport_iata) {
                ++counts[r.airline_iata];
            }
        }

        struct Row { std::string iata; std::string name; int n; };
        std::vector<Row> rows; rows.reserve(counts.size());
        for (auto& kv : counts) {
            auto al = db.GetAirlineByIATA(kv.first);
            rows.push_back({ kv.first, al ? al->name : std::string{}, kv.second });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.n != b.n) return a.n > b.n;
            return a.iata < b.iata;
            });

        std::string airport_name;
        if (auto ap = db.GetAirportByIATA(airport_iata)) airport_name = ap->name;

        crow::json::wvalue out;
        out["airport_iata"] = airport_iata;
        out["airport_name"] = airport_name;
        out["items"] = crow::json::wvalue::list();
        unsigned idx = 0;
        for (auto& r : rows) {
            crow::json::wvalue it;
            it["airline_iata"] = r.iata;
            it["airline_name"] = r.name;
            it["routes_count"] = r.n;
            out["items"][idx++] = std::move(it);
        }
        return crow::response(out);
            });

    // CSV version
    CROW_ROUTE(app, "/report/airport/<string>/airlines-by-routes.csv")
        ([&db](const std::string& airport_iata) {
        auto routes = db.SearchRoutes(airport_iata);
        std::unordered_map<std::string, int> counts;
        for (const auto& r : routes) {
            if (r.src_iata == airport_iata || r.dst_iata == airport_iata) {
                ++counts[r.airline_iata];
            }
        }
        struct Row { std::string iata; std::string name; int n; };
        std::vector<Row> rows; rows.reserve(counts.size());
        for (auto& kv : counts) {
            auto al = db.GetAirlineByIATA(kv.first);
            rows.push_back({ kv.first, al ? al->name : std::string{}, kv.second });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.n != b.n) return a.n > b.n;
            return a.iata < b.iata;
            });

        std::ostringstream ss;
        ss << "airline_iata,airline_name,routes_count\r\n";
        for (auto& r : rows) {
            ss << csv_escape(r.iata) << ','
                << csv_escape(r.name) << ','
                << r.n << "\r\n";
        }
        crow::response res{ ss.str() };
        res.add_header("Content-Type", "text/csv; charset=utf-8");
        res.add_header("Content-Disposition", "attachment; filename=\"airport_" + airport_iata + "_airlines.csv\"");
        return res;
            });

    // ---------- Section III.2.2: Reports Ordered by IATA Code ----------

    // 2.2.a: All Airlines ordered by IATA - JSON
    CROW_ROUTE(app, "/report/airlines/by-iata.json")
        ([&db] {
        auto all = db.GetAllAirlines();
        std::sort(all.begin(), all.end(), [](const Airline& a, const Airline& b) {
            const bool ae = a.iata.empty() || a.iata == "\\N";
            const bool be = b.iata.empty() || b.iata == "\\N";
            if (ae != be) return !ae;
            return a.iata < b.iata;
            });
        crow::json::wvalue arr = crow::json::wvalue::list();
        for (const auto& a : all) {
            crow::json::wvalue j;
            j["iata"] = a.iata; j["icao"] = a.icao; j["name"] = a.name;
            j["alias"] = a.alias; j["country"] = a.country; j["active"] = a.active;
            arr[arr.size()] = std::move(j);
        }
        return crow::response(arr);
            });

    // 2.2.a: All Airlines ordered by IATA - CSV
    CROW_ROUTE(app, "/report/airlines/by-iata.csv")
        ([&db] {
        auto all = db.GetAllAirlines();
        std::sort(all.begin(), all.end(), [](const Airline& a, const Airline& b) {
            const bool ae = a.iata.empty() || a.iata == "\\N";
            const bool be = b.iata.empty() || b.iata == "\\N";
            if (ae != be) return !ae;
            return a.iata < b.iata;
            });
        std::ostringstream ss;
        ss << "iata,icao,name,alias,country,active\r\n";
        for (const auto& a : all) {
            ss << csv_escape(a.iata) << ','
                << csv_escape(a.icao) << ','
                << csv_escape(a.name) << ','
                << csv_escape(a.alias) << ','
                << csv_escape(a.country) << ','
                << csv_escape(a.active) << "\r\n";
        }
        crow::response res{ ss.str() };
        res.add_header("Content-Type", "text/csv; charset=utf-8");
        res.add_header("Content-Disposition", "attachment; filename=\"all_airlines_by_iata.csv\"");
        return res;
            });

    // 2.2.b: All Airports ordered by IATA - JSON
    CROW_ROUTE(app, "/report/airports/by-iata.json")
        ([&db] {
        auto all = db.GetAllAirports();
        std::sort(all.begin(), all.end(), [](const Airport& a, const Airport& b) {
            const bool ae = a.iata.empty() || a.iata == "\\N";
            const bool be = b.iata.empty() || b.iata == "\\N";
            if (ae != be) return !ae;
            return a.iata < b.iata;
            });
        crow::json::wvalue arr = crow::json::wvalue::list();
        for (const auto& ap : all) {
            crow::json::wvalue j = ap.toJSON();
            arr[arr.size()] = std::move(j);
        }
        return crow::response(arr);
            });

    // 2.2.b: All Airports ordered by IATA - CSV
    CROW_ROUTE(app, "/report/airports/by-iata.csv")
        ([&db] {
        auto all = db.GetAllAirports();
        std::sort(all.begin(), all.end(), [](const Airport& a, const Airport& b) {
            const bool ae = a.iata.empty() || a.iata == "\\N";
            const bool be = b.iata.empty() || b.iata == "\\N";
            if (ae != be) return !ae;
            return a.iata < b.iata;
            });
        std::ostringstream ss;
        ss << "iata,icao,name,city,country,latitude,longitude\r\n";
        for (const auto& ap : all) {
            ss << csv_escape(ap.iata) << ','
                << csv_escape(ap.icao) << ','
                << csv_escape(ap.name) << ','
                << csv_escape(ap.city) << ','
                << csv_escape(ap.country) << ','
                << ap.latitude << ','
                << ap.longitude << "\r\n";
        }
        crow::response res{ ss.str() };
        res.add_header("Content-Type", "text/csv; charset=utf-8");
        res.add_header("Content-Disposition", "attachment; filename=\"all_airports_by_iata.csv\"");
        return res;
            });

    // ---------- Section III.2.3: Student ID ----------
    CROW_ROUTE(app, "/api/student-id")
        ([] {
        crow::json::wvalue j;
        // ⚠️⚠️⚠️ REPLACE THESE WITH YOUR ACTUAL INFORMATION ⚠️⚠️⚠️
        j["student_id"] = "20612701";
        j["name"] = "Vaishak Renjith";
        // ⚠️⚠️⚠️ REPLACE THESE WITH YOUR ACTUAL INFORMATION ⚠️⚠️⚠️
        j["course"] = "CIS 22C";
        j["project"] = "Air Travel Database Capstone";
        j["quarter"] = "Fall 2024";
        return crow::response(j);
            });

    // ---------- Section IV.3: One-Hop Routes (EXTRA CREDIT) ----------
    CROW_ROUTE(app, "/onehop/<string>/<string>")
        ([&db](const std::string& src, const std::string& dst) -> crow::response {
        // Disallow same src/dst
        if (src == dst) {
            crow::json::wvalue arr = crow::json::wvalue::list();
            return crow::response(arr);
        }

        auto src_ap = db.GetAirportByIATA(src);
        auto dst_ap = db.GetAirportByIATA(dst);
        if (!src_ap || !dst_ap) {
            return not_found("Source or destination airport not found");
        }

        // Find first-leg candidates from src with 0 stops
        std::vector<Route> from_src;
        {
            auto routes = db.SearchRoutes(src);
            for (const auto& r : routes) {
                if (r.src_iata == src && r.dst_iata != dst && r.stops == 0) {
                    from_src.push_back(r);
                }
            }
        }

        struct OneHopRoute {
            std::string src_iata, via_iata, dst_iata;
            std::string leg1_airline, leg2_airline;
            int total_distance_miles;
        };
        std::vector<OneHopRoute> results;

        // For each potential connecting airport, find second leg
        for (const auto& leg1 : from_src) {
            auto to_dst = db.GetRoutesFromTo(leg1.dst_iata, dst);
            // Filter to only 0-stop routes
            to_dst.erase(std::remove_if(to_dst.begin(), to_dst.end(),
                [](const Route& r) { return r.stops != 0; }), to_dst.end());
            if (to_dst.empty()) continue;

            auto via_ap = db.GetAirportByIATA(leg1.dst_iata);
            if (!via_ap) continue;

            // Calculate total distance using GPS coordinates
            double d1_km = db.CalculateDistanceKm(src_ap->latitude, src_ap->longitude,
                via_ap->latitude, via_ap->longitude);
            double d2_km = db.CalculateDistanceKm(via_ap->latitude, via_ap->longitude,
                dst_ap->latitude, dst_ap->longitude);
            int miles = static_cast<int>(std::lround((d1_km + d2_km) * 0.621371));

            for (const auto& leg2 : to_dst) {
                results.push_back({ leg1.src_iata, leg1.dst_iata, leg2.dst_iata,
                                   leg1.airline_iata, leg2.airline_iata, miles });
            }
        }

        // Sort by total distance (shortest first)
        std::sort(results.begin(), results.end(),
            [](const OneHopRoute& a, const OneHopRoute& b) {
                return a.total_distance_miles < b.total_distance_miles;
            });

        crow::json::wvalue arr = crow::json::wvalue::list();
        for (const auto& r : results) {
            crow::json::wvalue j;
            j["src"] = r.src_iata;
            j["via"] = r.via_iata;
            j["dst"] = r.dst_iata;
            j["leg1_airline"] = r.leg1_airline;
            j["leg2_airline"] = r.leg2_airline;
            j["total_miles"] = r.total_distance_miles;
            arr[arr.size()] = std::move(j);
        }
        return crow::response(arr);
            });

    // ---------- Section IV.2: Source Code Viewer (EXTRA CREDIT) ----------
    CROW_ROUTE(app, "/api/source-code")
        ([] {
        std::ostringstream combined;

        std::vector<std::string> files = {
            "server.cpp", "airdb.h", "airdp.cpp",
            "index.html", "style.css"
        };

        combined << "=================================================\n";
        combined << "  Air Travel Database - Complete Source Code\n";
        combined << "  CIS 22C Capstone Project\n";
        combined << "=================================================\n\n";

        for (const auto& filename : files) {
            combined << "\n\n";
            combined << "╔════════════════════════════════════════════════╗\n";
            combined << "║  FILE: " << filename;
            for (size_t i = filename.length(); i < 38; ++i) combined << " ";
            combined << "║\n";
            combined << "╚════════════════════════════════════════════════╝\n\n";

            auto content = read_file(filename);
            if (!content.empty()) {
                combined << content << "\n";
            }
            else {
                combined << "// [File not found or could not be read]\n";
            }
        }

        combined << "\n\n=================================================\n";
        combined << "  End of Source Code\n";
        combined << "=================================================\n";

        crow::response res{ combined.str() };
        res.add_header("Content-Type", "text/plain; charset=utf-8");
        res.add_header("Content-Disposition", "inline; filename=\"airdb_source.txt\"");
        return res;
            });

    // ---------- Additional Helper Routes ----------

    // Direct routes list (helper for one-hop calculation)
    CROW_ROUTE(app, "/routes/<string>/<string>")
        ([&db](const std::string& src, const std::string& dst) {
        auto vec = db.GetRoutesFromTo(src, dst);
        crow::json::wvalue arr = crow::json::wvalue::list();
        for (const auto& r : vec) arr[arr.size()] = r.toJSON();
        return crow::response(arr);
            });

    // Legacy /code endpoint
    CROW_ROUTE(app, "/code")
        ([] {
        return crow::response("Use /api/source-code to view the complete source code.");
            });

    // ---------- Run Server ----------
    int port = read_port();

    std::cout << "\n";
    std::cout << "===================================\n";
    std::cout << "  Air Travel Database Server\n";
    std::cout << "  CIS 22C Capstone Project\n";
    std::cout << "===================================\n";
    std::cout << "Server running on port: " << port << "\n";
    std::cout << "Access at: http://localhost:" << port << "\n";
    std::cout << "===================================\n";
    std::cout << "\nEndpoints Available:\n";
    std::cout << "  - Entity Lookup:\n";
    std::cout << "    GET /airline/<term>\n";
    std::cout << "    GET /airport/<term>\n";
    std::cout << "  - Reports:\n";
    std::cout << "    GET /report/airline/<iata>/airports-by-routes.json|csv\n";
    std::cout << "    GET /report/airport/<iata>/airlines-by-routes.json|csv\n";
    std::cout << "    GET /report/airlines/by-iata.json|csv\n";
    std::cout << "    GET /report/airports/by-iata.json|csv\n";
    std::cout << "  - One-Hop Routes:\n";
    std::cout << "    GET /onehop/<src>/<dst>\n";
    std::cout << "  - Student Info:\n";
    std::cout << "    GET /api/student-id\n";
    std::cout << "  - Source Code:\n";
    std::cout << "    GET /api/source-code\n";
    std::cout << "===================================\n\n";

    app.port(port).multithreaded().run();
    return 0;
}
