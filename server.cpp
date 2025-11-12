#include "crow.h"
#include "airdb.h"
#include "crow/json.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <string>

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
static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static crow::response not_found(const std::string& msg = "Not found") {
    return crow::response(404, msg);
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

    // ---------- Lookup: Airlines ----------
    // Flexible: IATA -> ICAO -> name-contains
    CROW_ROUTE(app, "/airline/<string>")
        ([&db](const std::string& term) {
        if (auto a = db.GetAirlineByIATA(term)) {
            return crow::response(a->toJSON());
        }
        if (term.size() == 3) {
            if (auto a = db.GetAirlineByICAO(term)) {
                return crow::response(a->toJSON());
            }
        }
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

    // Explicit ICAO endpoint (used by autocomplete fallback)
    CROW_ROUTE(app, "/api/airline/by-icao/<string>")
        ([&db](const std::string& icao) {
        if (auto a = db.GetAirlineByICAO(icao)) {
            return crow::response(a->toJSON());
        }
        return crow::response(404);
            });

    // Suggestions by airline name/IATA/ICAO
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

    // ---------- Lookup: Airports ----------
    // Flexible: ID -> IATA -> ICAO -> name/city contains
    CROW_ROUTE(app, "/airport/<string>")
        ([&db](const std::string& term) {
        // numeric ID?
        if (!term.empty() && std::all_of(term.begin(), term.end(), ::isdigit)) {
            int id = std::stoi(term);
            if (auto ap = db.GetAirportByID(id)) return crow::response(ap->toJSON());
        }
        if (auto ap = db.GetAirportByIATA(term)) return crow::response(ap->toJSON());
        if (term.size() == 4) {
            if (auto ap = db.GetAirportByICAO(term)) return crow::response(ap->toJSON());
        }
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

    // Suggestions for airports (name/city/country + IATA/ICAO)
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

    // ---------- Routes ----------
    // Direct routes list
    CROW_ROUTE(app, "/routes/<string>/<string>")
        ([&db](const std::string& src, const std::string& dst) {
        auto vec = db.GetRoutesFromTo(src, dst);
        crow::json::wvalue arr = crow::json::wvalue::list();
        for (const auto& r : vec) arr[arr.size()] = r.toJSON();
        return crow::response(arr);
            });

    // One-hop (two legs) with stops==0 on BOTH legs; ordered by total miles
    CROW_ROUTE(app, "/onehop/<string>/<string>")
([&db](const std::string& src, const std::string& dst) -> crow::response {
    // Disallow same src/dst — return empty JSON array
    if (src == dst) {
        crow::json::wvalue arr = crow::json::wvalue::list();
        return crow::response(arr);
    }

    auto src_ap = db.GetAirportByIATA(src);
    auto dst_ap = db.GetAirportByIATA(dst);
    if (!src_ap || !dst_ap) {
        return not_found("Source or destination airport not found");
    }

    // First-leg candidates from src with 0 stops and not already dst
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

    for (const auto& leg1 : from_src) {
        auto to_dst = db.GetRoutesFromTo(leg1.dst_iata, dst);
        to_dst.erase(std::remove_if(to_dst.begin(), to_dst.end(),
                     [](const Route& r){ return r.stops != 0; }), to_dst.end());
        if (to_dst.empty()) continue;

        auto via_ap = db.GetAirportByIATA(leg1.dst_iata);
        if (!via_ap) continue;

        double d1_km = db.CalculateDistanceKm(src_ap->latitude, src_ap->longitude,
                                              via_ap->latitude, via_ap->longitude);
        double d2_km = db.CalculateDistanceKm(via_ap->latitude, via_ap->longitude,
                                              dst_ap->latitude, dst_ap->longitude);
        int miles = static_cast<int>(std::lround((d1_km + d2_km) * 0.621371));

        for (const auto& leg2 : to_dst) {
            results.push_back({leg1.src_iata, leg1.dst_iata, leg2.dst_iata,
                               leg1.airline_iata, leg2.airline_iata, miles});
        }
    }

    std::sort(results.begin(), results.end(),
              [](const OneHopRoute& a, const OneHopRoute& b){
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


    // ---------- Misc ----------
    CROW_ROUTE(app, "/code")
        ([] { return crow::response("See source code in this service."); });

    // Run
    int port = read_port();

    app.port(port).multithreaded().run();
    return 0;
}