#define _USE_MATH_DEFINES
#include <cmath>

#include "airdb.h"
#include "crow/json.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>

using crow::json::wvalue;

// ---------------- JSON helpers ----------------
wvalue Airline::toJSON() const {
    wvalue j;
    j["id"] = id;
    j["name"] = name;
    j["alias"] = alias;
    j["iata"] = iata;
    j["icao"] = icao;
    j["callsign"] = callsign;
    j["country"] = country;
    j["active"] = active;
    return j;
}

wvalue Airport::toJSON() const {
    wvalue j;
    j["id"] = id;
    j["name"] = name;
    j["city"] = city;
    j["country"] = country;
    j["iata"] = iata;
    j["icao"] = icao;
    j["latitude"] = latitude;
    j["longitude"] = longitude;
    j["altitude_ft"] = altitude_ft;
    j["tz_offset"] = tz_offset;
    j["dst"] = dst;
    j["tz_db"] = tz_db;
    j["type"] = type;
    j["source"] = source;
    return j;
}

wvalue Route::toJSON() const {
    wvalue j;
    j["airline_iata"] = airline_iata;
    j["airline_id"] = airline_id;
    j["src_iata"] = src_iata;
    j["src_id"] = src_id;
    j["dst_iata"] = dst_iata;
    j["dst_id"] = dst_id;
    j["codeshare"] = codeshare;
    j["stops"] = stops;
    j["equipment"] = equipment;
    return j;
}

// ---------------- CSV helpers ----------------
std::string AirTravelDB::cleanField(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string t = s.substr(1, s.size() - 2);
        // unescape "" -> "
        std::string out;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '"' && i + 1 < t.size() && t[i + 1] == '"') { out.push_back('"'); ++i; }
            else out.push_back(t[i]);
        }
        return out;
    }
    return s;
}

std::vector<std::string> AirTravelDB::parseCSVLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool inq = false;
    for (char c : line) {
        if (c == '"') { inq = !inq; cur.push_back(c); }
        else if (c == ',' && !inq) { fields.push_back(cleanField(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    fields.push_back(cleanField(cur));
    return fields;
}

// ---------------- Loaders ----------------
static inline int toInt(const std::string& s) {
    if (s.empty() || s == "\\N") return -1;
    try { return std::stoi(s); }
    catch (...) { return -1; }
}
static inline double toDouble(const std::string& s) {
    if (s.empty() || s == "\\N") return 0.0;
    try { return std::stod(s); }
    catch (...) { return 0.0; }
}

bool AirTravelDB::LoadAirlinesCSV(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Failed to open " << path << "\n"; return false; }
    std::string line; size_t cnt = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto fields = parseCSVLine(line);
        // OpenFlights airlines.dat format (no header)
        // id, name, alias, IATA, ICAO, callsign, country, active
        if (fields.size() < 8) continue;
        auto a = std::make_shared<Airline>();
        a->id = toInt(fields[0]);
        a->name = fields[1];
        a->alias = fields[2];
        a->iata = fields[3];
        a->icao = fields[4];
        a->callsign = fields[5];
        a->country = fields[6];
        a->active = fields[7];
        std::lock_guard<std::mutex> lk(mtx_);
        if (!a->iata.empty() && a->iata != "\\N") airlines_by_iata_[a->iata] = a;
        // NEW: index by ICAO too
        if (!a->icao.empty() && a->icao != "\\N") airlines_by_icao_[a->icao] = a;
        airlines_by_id_[a->id] = a;
        ++cnt;
    }
    std::cout << "Loaded " << cnt << " airlines\n";
    return true;
}

bool AirTravelDB::LoadAirportsCSV(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Failed to open " << path << "\n"; return false; }
    std::string line; size_t cnt = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto fields = parseCSVLine(line);
        // OpenFlights airports.dat (no header)
        // id, name, city, country, IATA, ICAO, lat, lon, alt, tz, dst, tzdb, type, source
        if (fields.size() < 14) continue;
        auto ap = std::make_shared<Airport>();
        ap->id = toInt(fields[0]);
        ap->name = fields[1];
        ap->city = fields[2];
        ap->country = fields[3];
        ap->iata = fields[4];
        ap->icao = fields[5];
        ap->latitude = toDouble(fields[6]);
        ap->longitude = toDouble(fields[7]);
        ap->altitude_ft = toInt(fields[8]);
        ap->tz_offset = toDouble(fields[9]);
        ap->dst = fields[10];
        ap->tz_db = fields[11];
        ap->type = fields[12];
        ap->source = fields[13];
        std::lock_guard<std::mutex> lk(mtx_);
        if (!ap->iata.empty() && ap->iata != "\\N") airports_by_iata_[ap->iata] = ap;
        if (!ap->icao.empty()) airports_by_icao_[ap->icao] = ap;
        airports_by_id_[ap->id] = ap;
        ++cnt;
    }
    std::cout << "Loaded " << cnt << " airports\n";
    return true;
}

bool AirTravelDB::LoadRoutesCSV(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "Failed to open " << path << "\n"; return false; }
    std::string line; size_t cnt = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto fields = parseCSVLine(line);
        // OpenFlights routes.dat (no header)
        // airline, airline_id, src, src_id, dst, dst_id, codeshare, stops, equipment
        if (fields.size() < 9) continue;
        Route r;
        r.airline_iata = fields[0];
        r.airline_id = toInt(fields[1]);
        r.src_iata = fields[2];
        r.src_id = toInt(fields[3]);
        r.dst_iata = fields[4];
        r.dst_id = toInt(fields[5]);
        r.codeshare = fields[6];
        r.stops = toInt(fields[7]);
        r.equipment = fields[8];
        std::lock_guard<std::mutex> lk(mtx_);
        routes_.push_back(std::move(r));
        ++cnt;
    }
    std::cout << "Loaded " << cnt << " routes\n";
    return true;
}

// ---------------- Queries ----------------
std::shared_ptr<Airline> AirTravelDB::GetAirlineByIATA(const std::string& iata) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = airlines_by_iata_.find(iata);
    return it == airlines_by_iata_.end() ? nullptr : it->second;
}
std::shared_ptr<Airline> AirTravelDB::GetAirlineByID(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = airlines_by_id_.find(id);
    return it == airlines_by_id_.end() ? nullptr : it->second;
}

std::shared_ptr<Airport> AirTravelDB::GetAirportByIATA(const std::string& iata) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = airports_by_iata_.find(iata);
    return it == airports_by_iata_.end() ? nullptr : it->second;
}
std::shared_ptr<Airport> AirTravelDB::GetAirportByID(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = airports_by_id_.find(id);
    return it == airports_by_id_.end() ? nullptr : it->second;
}

std::vector<Route> AirTravelDB::GetRoutesFromTo(const std::string& src_iata,
    const std::string& dst_iata) const {
    std::vector<Route> out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& r : routes_) {
        if (r.src_iata == src_iata && r.dst_iata == dst_iata) out.push_back(r);
    }
    return out;
}

std::vector<Route> AirTravelDB::SearchRoutes(const std::string& token) const {
    std::vector<Route> out;
    std::string t = token;
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& r : routes_) {
        std::string a = r.airline_iata, s = r.src_iata, d = r.dst_iata;
        std::transform(a.begin(), a.end(), a.begin(), ::toupper);
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        std::transform(d.begin(), d.end(), d.begin(), ::toupper);
        if (a.find(t) != std::string::npos || s.find(t) != std::string::npos || d.find(t) != std::string::npos)
            out.push_back(r);
    }
    return out;
}

// ---------------- Geo ----------------
double AirTravelDB::CalculateDistanceKm(double lat1, double lon1,
    double lat2, double lon2) const {
    constexpr double R = 6371.0; // km
    auto deg2rad = [](double v) { return v * M_PI / 180.0; };
    double dLat = deg2rad(lat2 - lat1);
    double dLon = deg2rad(lon2 - lon1);
    lat1 = deg2rad(lat1); lat2 = deg2rad(lat2);
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
        std::cos(lat1) * std::cos(lat2) * std::sin(dLon / 2) * std::sin(dLon / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return R * c;
}

std::vector<std::pair<std::shared_ptr<Airport>, int>>
AirTravelDB::GetAirportsWithinRadiusKm(double lat, double lon, double radius_km) const {
    std::vector<std::pair<std::shared_ptr<Airport>, int>> out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& kv : airports_by_id_) {
        const auto& ap = kv.second;
        double dist = CalculateDistanceKm(lat, lon, ap->latitude, ap->longitude);
        if (dist <= radius_km) {
            out.emplace_back(ap, static_cast<int>(std::lround(dist)));
        }
    }
    return out;
}

std::vector<Airline> AirTravelDB::GetAllAirlines() const {
    std::vector<Airline> out;
    std::lock_guard<std::mutex> lk(mtx_);
    out.reserve(airlines_by_id_.size());
    for (const auto& kv : airlines_by_id_) {
        if (kv.second) out.push_back(*kv.second);
    }
    std::sort(out.begin(), out.end(), [](const Airline& a, const Airline& b) {
        if (a.name == b.name) return a.iata < b.iata;
        return a.name < b.name;
        });
    return out;
}

std::shared_ptr<Airline> AirTravelDB::GetAirlineByICAO(const std::string& icao) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = airlines_by_icao_.find(icao);
    return it == airlines_by_icao_.end() ? nullptr : it->second;
}

std::shared_ptr<Airport> AirTravelDB::GetAirportByICAO(const std::string& icao) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = airports_by_icao_.find(icao);
    return it == airports_by_icao_.end() ? nullptr : it->second;
}

std::vector<Airport> AirTravelDB::GetAllAirports() const {
    std::vector<Airport> out;
    std::lock_guard<std::mutex> lk(mtx_);
    out.reserve(airports_by_id_.size());
    for (const auto& kv : airports_by_id_) {
        if (kv.second) out.push_back(*kv.second);
    }
    std::sort(out.begin(), out.end(), [](const Airport& a, const Airport& b) {
        if (a.name == b.name) return a.iata < b.iata;
        return a.name < b.name;
        });
    return out;
}
